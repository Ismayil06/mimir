#include "mim/plug/ascend/ascend.h"

#include <cctype>

#include <sstream>
#include <string>
#include <vector>

#include <mim/axm.h>
#include <mim/lam.h>
#include <mim/phase.h>
#include <mim/plugin.h>
#include <mim/tuple.h>

#include <mim/plug/math/math.h>
#include <mim/plug/tensor/tensor.h>

namespace mim::plug::ascend {

namespace {

class Emitter {
public:
    Emitter(World& world, std::ostream& os)
        : world_(world)
        , os_(os) {}

    void run() {
        Lam* entry = nullptr;
        for (auto external : world_.externals().muts()) {
            auto lam = external->isa_mut<Lam>();
            if (!lam) fe::throwf("ascend backend expected an external function, but got `{}`", external);
            if (entry) fe::throwf("ascend backend supports exactly one external function");
            entry = lam;
        }

        if (!entry) fe::throwf("ascend backend requires one external function");
        emit(entry);
    }

private:
    std::string type(const Def* def) const {
        auto dims = std::vector<nat_t>();
        while (auto arr = def->isa<Arr>()) {
            auto extent = Lit::isa<nat_t>(arr->arity());
            if (!extent) fe::throwf("ascend backend requires static tensor extents, but got `{}`", arr->arity());
            dims.emplace_back(*extent);
            def = arr->body();
        }

        if (dims.empty()) fe::throwf("ascend backend expected a tensor type, but got `{}`", def);
        if (!math::match_f32(def)) fe::throwf("ascend backend only supports F32 tensor elements, but got `{}`", def);

        auto s = std::ostringstream();
        s << "tensor<";
        for (auto dim : dims)
            s << dim << 'x';
        s << "f32>";
        return s.str();
    }

    static std::string symbol(Lam* lam) {
        auto name = lam->sym().view();
        if (name.empty()) fe::throwf("ascend backend requires a named external function");

        for (auto c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.'))
                fe::throwf("ascend backend cannot emit function name `{}`", name);
        return std::string(name);
    }

    static bool is_f32_add(const Def* def) {
        if (!Axm::isa<math::arith, 1>(math::arith::add, def)) return false;
        auto args = App::uncurry_args<2>(def);
        return Lit::isa(args[0]) == 0 && math::match_f32(def->type()->as<Pi>()->codom());
    }

    std::string value(const Def* def) {
        if (auto i = values_.find(def); i != values_.end()) return i->second;

        auto binary = Axm::isa<tensor::binary>(def);
        if (!binary) fe::throwf("ascend backend does not support value `{}`", def);

        auto args = binary->uncurry_args<4>();
        if (!is_f32_add(args[1])) fe::throwf("ascend backend only supports tensor.binary with F32 addition");

        auto [lhs, rhs] = args[3]->projs<2>();
        auto lhs_name   = value(lhs);
        auto rhs_name   = value(rhs);
        auto tensor_ty  = type(def->type());
        if (type(lhs->type()) != tensor_ty || type(rhs->type()) != tensor_ty)
            fe::throwf("ascend backend requires equal input and output tensor types");

        auto empty  = "%empty" + std::to_string(next_);
        auto result = "%result" + std::to_string(next_++);
        body_ << "    " << empty << " = tensor.empty() : " << tensor_ty << '\n';
        body_ << "    " << result << " = linalg.elemwise_binary {fun = #linalg.binary_fn<add>} "
              << "ins(" << lhs_name << ", " << rhs_name << " : " << tensor_ty << ", " << tensor_ty << ") "
              << "outs(" << empty << " : " << tensor_ty << ") -> " << tensor_ty << '\n';
        values_.emplace(def, result);
        return result;
    }

    void emit(Lam* lam) {
        if (!lam->is_set() || Pi::isa_cn(lam->type()))
            fe::throwf("ascend backend requires a set direct-style external function, but got `{}`", lam);

        auto n = lam->num_vars();
        if (n != 2) fe::throwf("ascend ADD requires exactly two tensor parameters, but got {}", n);

        auto args = std::ostringstream();
        for (size_t i = 0; i != n; ++i) {
            auto var = lam->var(n, i);
            auto arg = "%arg" + std::to_string(i);
            if (i != 0) args << ", ";
            args << arg << ": " << type(var->type());
            values_.emplace(var, arg);
        }

        auto result_ty = type(lam->codom());
        auto result    = value(lam->body());

        os_ << "module {\n";
        os_ << "  func.func @" << symbol(lam) << '(' << args.str() << ") -> " << result_ty
            << " attributes {hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>} {\n";
        os_ << body_.str();
        os_ << "    return " << result << " : " << result_ty << '\n';
        os_ << "  }\n";
        os_ << "}\n";
    }

    World& world_;
    std::ostream& os_;
    DefMap<std::string> values_;
    std::ostringstream body_;
    size_t next_ = 0;
};

class EmitHFusion : public Phase {
public:
    EmitHFusion(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name = world().name() ? world().name().str() : std::string("a");
        auto path = name + ".ascend.mlir";
        if (auto output = arg_value(args(), "o", "output")) path = *output;

        auto out = Out(path);
        Emitter(world(), *out.os()).run();
    }
};

void register_phases(Flags2Phases& phases) { Phase::hook<ascend::emit_hfusion, EmitHFusion>(phases); }

constexpr PluginArg Args[] = {
    {"o=<file>, output=<file>", "Writes Linalg/HFusion MLIR to `<file>`; use `-` for stdout."},
};

} // namespace

} // namespace mim::plug::ascend

using namespace mim;

extern "C" MIM_EXPORT mim::Plugin mim_get_plugin() {
    return {"ascend",
            MIM_VERSION,
            {},
            mim::plug::ascend::register_phases,
            mim::plug::ascend::Args,
            std::size(mim::plug::ascend::Args),
            {},
            0};
}
