#include "hivm.h"

#include "mim/plug/ascend/ascend.h"

#include "mlir/hacc_dialect.h"
#include "mlir/hivm_dialect.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mim/lam.h>
#include <mim/phase.h>
#include <mim/plugin.h>
#include <mim/tuple.h>

#include <mim/plug/math/math.h>
#include <mim/plug/tensor/tensor.h>

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"

namespace mim::plug::ascend {

namespace {

// UB_SIZE of Ascend950PR_9579 (dav-c310), read from the bishengir target spec.
constexpr std::uint64_t ascend950_ub_bytes = 2031616;

struct Shape {
    std::vector<int64_t> dims;
    bool operator==(const Shape& other) const { return dims == other.dims; }
};

Shape shape_of(const Def* type) {
    auto dims = std::vector<int64_t>();
    while (auto arr = type->isa<Arr>()) {
        auto extent = Lit::isa<nat_t>(arr->arity());
        if (!extent) fe::throwf("ascend backend requires static tensor extents, but got `{}`", arr->arity());
        if (*extent > static_cast<nat_t>(std::numeric_limits<int64_t>::max()))
            fe::throwf("ascend backend tensor extent `{}` is too large", *extent);
        dims.push_back(static_cast<int64_t>(*extent));
        type = arr->body();
    }
    if (dims.empty()) fe::throwf("ascend backend expected a tensor type, but got `{}`", type);
    if (!math::match_f32(type)) fe::throwf("ascend backend only supports F32 tensor elements, but got `{}`", type);
    return {std::move(dims)};
}

void require_same_shape(const Shape& expected, const Def* type) {
    if (shape_of(type) != expected) fe::throwf("ascend backend requires equal input and output tensor types");
}

std::string symbol(Lam* lam) {
    auto name = lam->sym().view();
    if (name.empty()) fe::throwf("ascend backend requires a named external function");
    for (auto c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.'))
            fe::throwf("ascend backend cannot emit function name `{}`", name);
    return std::string(name);
}

std::optional<mlir::hivm::VecBin> match_f32_arith(const Def* def) {
    auto kind = std::optional<mlir::hivm::VecBin>();
    if (Axm::isa<math::arith, 1>(math::arith::add, def))
        kind = mlir::hivm::VecBin::Add;
    else if (Axm::isa<math::arith, 1>(math::arith::sub, def))
        kind = mlir::hivm::VecBin::Sub;
    else if (Axm::isa<math::arith, 1>(math::arith::mul, def))
        kind = mlir::hivm::VecBin::Mul;
    else if (Axm::isa<math::arith, 1>(math::arith::div, def))
        kind = mlir::hivm::VecBin::Div;
    if (!kind) return {};

    auto args = App::uncurry_args<2>(def);
    if (Lit::isa(args[0]) != 0 || !math::match_f32(def->type()->as<Pi>()->codom())) return {};
    return kind;
}

struct Instr {
    enum class Kind { Load, Store, Bin } kind;
    mlir::hivm::VecBin bin = mlir::hivm::VecBin::Add;
    int dst                = -1;
    int lhs                = -1;
    int rhs                = -1;
};

struct Program {
    std::string name;
    Shape shape;
    int num_args = 0;
    int num_ub   = 0;
    std::vector<Instr> instrs;
};

Program analyze(World& world, std::uint64_t ub_bytes) {
    Lam* entry = nullptr;
    for (auto external : world.externals().muts()) {
        auto lam = external->isa_mut<Lam>();
        if (!lam) fe::throwf("ascend backend expected an external function, but got `{}`", external);
        if (entry) fe::throwf("ascend backend supports exactly one external function");
        entry = lam;
    }
    if (!entry) fe::throwf("ascend backend requires one external function");
    if (!entry->is_set() || Pi::isa_cn(entry->type()))
        fe::throwf("ascend backend requires a set direct-style external function, but got `{}`", entry);

    auto program  = Program{};
    program.name  = symbol(entry);
    program.shape = shape_of(entry->codom());

    auto n = entry->num_vars();
    if (n == 0) fe::throwf("ascend backend requires at least one tensor parameter");
    auto args = DefMap<int>();
    for (size_t i = 0; i != n; ++i) {
        auto var = entry->var(n, i);
        require_same_shape(program.shape, var->type());
        args.emplace(var, static_cast<int>(i));
    }
    program.num_args = static_cast<int>(n);

    auto ids   = DefMap<int>();
    auto next  = 0;
    auto lower = [&](auto& self, const Def* def) -> int {
        if (auto known = ids.find(def); known != ids.end()) return known->second;
        if (auto arg = args.find(def); arg != args.end()) {
            auto id = next++;
            program.instrs.push_back({Instr::Kind::Load, {}, id, arg->second, -1});
            ids.emplace(def, id);
            return id;
        }

        auto binary = Axm::isa<tensor::binary>(def);
        if (!binary) fe::throwf("ascend backend does not support value `{}`", def);
        auto operands = binary->uncurry_args<4>();
        auto kind     = match_f32_arith(operands[1]);
        if (!kind)
            fe::throwf(
                "ascend backend only supports tensor.binary of F32 math.arith add, sub, mul, or div, but got `{}`",
                operands[1]);
        require_same_shape(program.shape, def->type());
        auto [lhs, rhs] = operands[3]->projs<2>();
        require_same_shape(program.shape, lhs->type());
        require_same_shape(program.shape, rhs->type());
        auto l  = self(self, lhs);
        auto r  = self(self, rhs);
        auto id = next++;
        program.instrs.push_back({Instr::Kind::Bin, *kind, id, l, r});
        ids.emplace(def, id);
        return id;
    };

    auto result = lower(lower, entry->body());
    program.instrs.push_back({Instr::Kind::Store, {}, -1, result, -1});
    program.num_ub = next;

    auto elements = nat_t{1};
    for (auto dim : program.shape.dims) {
        if (dim != 0 && elements > std::numeric_limits<nat_t>::max() / static_cast<nat_t>(dim))
            fe::throwf("ascend backend tensor is too large for the unified buffer");
        elements *= static_cast<nat_t>(dim);
    }
    auto buffers = static_cast<nat_t>(program.num_ub);
    auto bytes   = nat_t{0};
    if (elements != 0 && buffers != 0) {
        if (buffers > std::numeric_limits<nat_t>::max() / elements / 4)
            fe::throwf("ascend backend tensor is too large for the unified buffer");
        bytes = elements * 4 * buffers;
    }
    if (bytes > ub_bytes)
        fe::throwf("ascend backend requires the kernel to fit in one unified-buffer allocation ({} bytes), but this "
                   "kernel needs {} bytes",
                   ub_bytes, bytes);
    return program;
}

struct Built {
    std::string mlir;
    std::string error;
};

Built build(const Program& program) {
    // libMLIR is compiled without exception tables, so this function does not throw.
    auto ctx = mlir::MLIRContext();
    ctx.loadDialect<mlir::func::FuncDialect, mlir::memref::MemRefDialect, mlir::hivm::HIVMDialect,
                    mlir::hacc::HACCDialect>();

    auto builder = mlir::OpBuilder(&ctx);
    auto loc     = builder.getUnknownLoc();
    auto module  = mlir::ModuleOp::create(loc);
    builder.setInsertionPointToEnd(&module.getBodyRegion().front());

    auto f32  = builder.getF32Type();
    auto gm   = mlir::hivm::AddressSpaceAttr::get(&ctx, mlir::hivm::AddressSpace::GM);
    auto ub   = mlir::hivm::AddressSpaceAttr::get(&ctx, mlir::hivm::AddressSpace::UB);
    auto gmty = mlir::MemRefType::get(program.shape.dims, f32, mlir::AffineMap(), gm);
    auto ubty = mlir::MemRefType::get(program.shape.dims, f32, mlir::AffineMap(), ub);

    auto inputs = mlir::SmallVector<mlir::Type>(static_cast<size_t>(program.num_args) + 1, gmty);
    auto func   = builder.create<mlir::func::FuncOp>(loc, program.name, builder.getFunctionType(inputs, {}));
    func->setAttr("hacc.entry", mlir::UnitAttr::get(&ctx));
    func->setAttr("hacc.function_kind", mlir::hacc::FunctionKindAttr::get(&ctx, mlir::hacc::FunctionKind::Device));

    auto* entry = func.addEntryBlock();
    builder.setInsertionPointToStart(entry);
    auto gm_args = entry->getArguments();
    auto ub_vals = std::vector<mlir::Value>(static_cast<size_t>(program.num_ub));

    auto binary = [&](mlir::hivm::VecBin kind, mlir::Value lhs, mlir::Value rhs, mlir::Value dst) {
        switch (kind) {
            case mlir::hivm::VecBin::Add: builder.create<mlir::hivm::VAddOp>(loc, lhs, rhs, dst); break;
            case mlir::hivm::VecBin::Sub: builder.create<mlir::hivm::VSubOp>(loc, lhs, rhs, dst); break;
            case mlir::hivm::VecBin::Mul: builder.create<mlir::hivm::VMulOp>(loc, lhs, rhs, dst); break;
            case mlir::hivm::VecBin::Div: builder.create<mlir::hivm::VDivOp>(loc, lhs, rhs, dst); break;
        }
    };

    for (auto& instr : program.instrs) {
        switch (instr.kind) {
            case Instr::Kind::Load: {
                auto alloc = builder.create<mlir::memref::AllocOp>(loc, ubty);
                builder.create<mlir::hivm::LoadOp>(loc, gm_args[instr.lhs], alloc);
                ub_vals[static_cast<size_t>(instr.dst)] = alloc;
                break;
            }
            case Instr::Kind::Bin: {
                auto alloc = builder.create<mlir::memref::AllocOp>(loc, ubty);
                binary(instr.bin, ub_vals[static_cast<size_t>(instr.lhs)], ub_vals[static_cast<size_t>(instr.rhs)],
                       alloc);
                ub_vals[static_cast<size_t>(instr.dst)] = alloc;
                break;
            }
            case Instr::Kind::Store:
                builder.create<mlir::hivm::StoreOp>(loc, ub_vals[static_cast<size_t>(instr.lhs)], gm_args.back());
                break;
        }
    }
    builder.create<mlir::func::ReturnOp>(loc);

    auto error   = std::string();
    auto handler = mlir::ScopedDiagnosticHandler(&ctx, [&](mlir::Diagnostic& diag) {
        auto os = llvm::raw_string_ostream(error);
        os << diag << '\n';
    });
    if (failed(module.verify())) {
        if (error.empty()) error = "verification failed\n";
        return {{}, std::move(error)};
    }

    auto text = std::string();
    auto os   = llvm::raw_string_ostream(text);
    module.print(os);
    os.flush();
    return {std::move(text), {}};
}

std::uint64_t parse_ub(std::string_view raw) {
    auto value     = std::uint64_t{0};
    auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || value == 0)
        fe::throwf("ascend backend expected a positive byte count for `ub`, but got `{}`", raw);
    return value;
}

class EmitHIVM : public Phase {
public:
    EmitHIVM(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name = world().name() ? world().name().str() : std::string("a");
        auto path = name + ".ascend.mlir";
        if (auto output = arg_value(args(), "o", "output")) path = *output;

        auto ub = ascend950_ub_bytes;
        if (auto raw = arg_value(args(), "ub")) ub = parse_ub(*raw);

        auto program = analyze(world(), ub);
        auto built   = build(program);
        if (!built.error.empty())
            fe::throwf("ascend backend generated HIVM that failed MLIR verification: {}", built.error);

        auto out = Out(std::move(path));
        *out.os() << built.mlir;
    }
};

} // namespace

void register_hivm(Flags2Phases& phases) { Phase::hook<ascend::emit_hivm, EmitHIVM>(phases); }

} // namespace mim::plug::ascend
