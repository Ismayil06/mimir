#pragma once

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"

namespace mlir::hivm {

enum class AddressSpace { GM, UB };

namespace detail {
struct AddressSpaceAttrStorage;
} // namespace detail

class AddressSpaceAttr : public Attribute::AttrBase<AddressSpaceAttr, Attribute, detail::AddressSpaceAttrStorage> {
public:
    using Base::Base;
    static AddressSpaceAttr get(MLIRContext* ctx, AddressSpace space);
    AddressSpace getSpace() const;
};

class HIVMDialect : public Dialect {
public:
    explicit HIVMDialect(MLIRContext* context);
    static constexpr llvm::StringLiteral getDialectNamespace() { return "hivm"; }

    Attribute parseAttribute(DialectAsmParser& parser, Type type) const override;
    void printAttribute(Attribute attr, DialectAsmPrinter& printer) const override;

private:
    void initialize();
};

void printInsOuts(Operation* op, OpAsmPrinter& printer, unsigned num_ins);
LogicalResult verifyMemrefs(Operation* op, llvm::ArrayRef<AddressSpace> spaces);

class LoadOp : public Op<LoadOp, OpTrait::ZeroRegions, OpTrait::ZeroResults, OpTrait::ZeroSuccessors,
                         OpTrait::NOperands<2>::Impl> {
public:
    using Op::Op;
    static constexpr llvm::StringLiteral getOperationName() { return "hivm.hir.load"; }
    static llvm::ArrayRef<llvm::StringRef> getAttributeNames() { return {}; }
    static void build(OpBuilder&, OperationState& state, Value src, Value dst) { state.addOperands({src, dst}); }
    void print(OpAsmPrinter& printer) { printInsOuts(getOperation(), printer, 1); }
    LogicalResult verify() { return verifyMemrefs(getOperation(), {AddressSpace::GM, AddressSpace::UB}); }
};

class StoreOp : public Op<StoreOp, OpTrait::ZeroRegions, OpTrait::ZeroResults, OpTrait::ZeroSuccessors,
                          OpTrait::NOperands<2>::Impl> {
public:
    using Op::Op;
    static constexpr llvm::StringLiteral getOperationName() { return "hivm.hir.store"; }
    static llvm::ArrayRef<llvm::StringRef> getAttributeNames() { return {}; }
    static void build(OpBuilder&, OperationState& state, Value src, Value dst) { state.addOperands({src, dst}); }
    void print(OpAsmPrinter& printer) { printInsOuts(getOperation(), printer, 1); }
    LogicalResult verify() { return verifyMemrefs(getOperation(), {AddressSpace::UB, AddressSpace::GM}); }
};

enum class VecBin { Add, Sub, Mul, Div };

template<VecBin Kind> struct VecOpName;
template<> struct VecOpName<VecBin::Add> {
    static constexpr llvm::StringLiteral value = "hivm.hir.vadd";
};
template<> struct VecOpName<VecBin::Sub> {
    static constexpr llvm::StringLiteral value = "hivm.hir.vsub";
};
template<> struct VecOpName<VecBin::Mul> {
    static constexpr llvm::StringLiteral value = "hivm.hir.vmul";
};
template<> struct VecOpName<VecBin::Div> {
    static constexpr llvm::StringLiteral value = "hivm.hir.vdiv";
};

template<VecBin Kind>
class VecBinaryOp : public Op<VecBinaryOp<Kind>, OpTrait::ZeroRegions, OpTrait::ZeroResults, OpTrait::ZeroSuccessors,
                              OpTrait::NOperands<3>::Impl> {
public:
    using Op<VecBinaryOp<Kind>, OpTrait::ZeroRegions, OpTrait::ZeroResults, OpTrait::ZeroSuccessors,
             OpTrait::NOperands<3>::Impl>::Op;
    static constexpr llvm::StringLiteral getOperationName() { return VecOpName<Kind>::value; }
    static llvm::ArrayRef<llvm::StringRef> getAttributeNames() { return {}; }
    static void build(OpBuilder&, OperationState& state, Value lhs, Value rhs, Value dst) {
        state.addOperands({lhs, rhs, dst});
    }
    void print(OpAsmPrinter& printer) { printInsOuts(this->getOperation(), printer, 2); }
    LogicalResult verify() {
        return verifyMemrefs(this->getOperation(), {AddressSpace::UB, AddressSpace::UB, AddressSpace::UB});
    }
};

using VAddOp = VecBinaryOp<VecBin::Add>;
using VSubOp = VecBinaryOp<VecBin::Sub>;
using VMulOp = VecBinaryOp<VecBin::Mul>;
using VDivOp = VecBinaryOp<VecBin::Div>;

} // namespace mlir::hivm

MLIR_DECLARE_EXPLICIT_TYPE_ID(::mlir::hivm::HIVMDialect)
MLIR_DECLARE_EXPLICIT_TYPE_ID(::mlir::hivm::AddressSpaceAttr)
