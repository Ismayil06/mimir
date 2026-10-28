#include "hacc_dialect.h"

#include "mlir/IR/DialectImplementation.h"

using namespace mlir;
using namespace mlir::hacc;

namespace mlir::hacc::detail {

struct FunctionKindAttrStorage : public AttributeStorage {
    using KeyTy = FunctionKind;

    explicit FunctionKindAttrStorage(FunctionKind kind)
        : kind(kind) {}

    bool operator==(const KeyTy& key) const { return key == kind; }
    static llvm::hash_code hashKey(const KeyTy& key) { return llvm::hash_value(static_cast<uint32_t>(key)); }
    static FunctionKindAttrStorage* construct(AttributeStorageAllocator& allocator, const KeyTy& key) {
        return new (allocator.allocate<FunctionKindAttrStorage>()) FunctionKindAttrStorage(key);
    }

    FunctionKind kind;
};

} // namespace mlir::hacc::detail

MLIR_DEFINE_EXPLICIT_TYPE_ID(::mlir::hacc::HACCDialect)
MLIR_DEFINE_EXPLICIT_TYPE_ID(::mlir::hacc::FunctionKindAttr)

FunctionKindAttr FunctionKindAttr::get(MLIRContext* ctx, FunctionKind kind) { return Base::get(ctx, kind); }

FunctionKind FunctionKindAttr::getKind() const { return getImpl()->kind; }

void HACCDialect::initialize() { addAttributes<FunctionKindAttr>(); }

HACCDialect::HACCDialect(MLIRContext* context)
    : Dialect(getDialectNamespace(), context, TypeID::get<HACCDialect>()) {
    initialize();
}

static StringRef spelling(FunctionKind kind) {
    switch (kind) {
        case FunctionKind::Device: return "DEVICE";
        case FunctionKind::Host:   return "HOST";
    }
    llvm_unreachable("function kind");
}

Attribute HACCDialect::parseAttribute(DialectAsmParser& parser, Type) const {
    StringRef mnemonic;
    if (failed(parser.parseKeyword(&mnemonic))) return {};
    if (mnemonic != "function_kind" || failed(parser.parseLess())) return {};

    StringRef kind;
    if (failed(parser.parseKeyword(&kind)) || failed(parser.parseGreater())) return {};
    if (kind == "DEVICE") return FunctionKindAttr::get(getContext(), FunctionKind::Device);
    if (kind == "HOST") return FunctionKindAttr::get(getContext(), FunctionKind::Host);
    parser.emitError(parser.getNameLoc(), "unknown hacc function kind");
    return {};
}

void HACCDialect::printAttribute(Attribute attr, DialectAsmPrinter& printer) const {
    printer << "function_kind<" << spelling(attr.cast<FunctionKindAttr>().getKind()) << ">";
}
