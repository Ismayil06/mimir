#pragma once

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Dialect.h"

namespace mlir::hacc {

enum class FunctionKind { Device, Host };

namespace detail {
struct FunctionKindAttrStorage;
} // namespace detail

class FunctionKindAttr : public Attribute::AttrBase<FunctionKindAttr, Attribute, detail::FunctionKindAttrStorage> {
public:
    using Base::Base;
    static FunctionKindAttr get(MLIRContext* ctx, FunctionKind kind);
    FunctionKind getKind() const;
};

class HACCDialect : public Dialect {
public:
    explicit HACCDialect(MLIRContext* context);
    static constexpr llvm::StringLiteral getDialectNamespace() { return "hacc"; }

    Attribute parseAttribute(DialectAsmParser& parser, Type type) const override;
    void printAttribute(Attribute attr, DialectAsmPrinter& printer) const override;

private:
    void initialize();
};

} // namespace mlir::hacc

MLIR_DECLARE_EXPLICIT_TYPE_ID(::mlir::hacc::HACCDialect)
MLIR_DECLARE_EXPLICIT_TYPE_ID(::mlir::hacc::FunctionKindAttr)
