#include "hivm_dialect.h"

#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;
using namespace mlir::hivm;

namespace mlir::hivm::detail {

struct AddressSpaceAttrStorage : public AttributeStorage {
    using KeyTy = AddressSpace;

    explicit AddressSpaceAttrStorage(AddressSpace space)
        : space(space) {}

    bool operator==(const KeyTy& key) const { return key == space; }
    static llvm::hash_code hashKey(const KeyTy& key) { return llvm::hash_value(static_cast<uint32_t>(key)); }
    static AddressSpaceAttrStorage* construct(AttributeStorageAllocator& allocator, const KeyTy& key) {
        return new (allocator.allocate<AddressSpaceAttrStorage>()) AddressSpaceAttrStorage(key);
    }

    AddressSpace space;
};

} // namespace mlir::hivm::detail

MLIR_DEFINE_EXPLICIT_TYPE_ID(::mlir::hivm::HIVMDialect)
MLIR_DEFINE_EXPLICIT_TYPE_ID(::mlir::hivm::AddressSpaceAttr)

AddressSpaceAttr AddressSpaceAttr::get(MLIRContext* ctx, AddressSpace space) { return Base::get(ctx, space); }

AddressSpace AddressSpaceAttr::getSpace() const { return getImpl()->space; }

void HIVMDialect::initialize() {
    addAttributes<AddressSpaceAttr>();
    addOperations<LoadOp, StoreOp, VAddOp, VSubOp, VMulOp, VDivOp>();
}

HIVMDialect::HIVMDialect(MLIRContext* context)
    : Dialect(getDialectNamespace(), context, TypeID::get<HIVMDialect>()) {
    initialize();
}

static StringRef spelling(AddressSpace space) {
    switch (space) {
        case AddressSpace::GM: return "gm";
        case AddressSpace::UB: return "ub";
    }
    llvm_unreachable("address space");
}

Attribute HIVMDialect::parseAttribute(DialectAsmParser& parser, Type) const {
    StringRef mnemonic;
    if (failed(parser.parseKeyword(&mnemonic))) return {};
    if (mnemonic != "address_space" || failed(parser.parseLess())) return {};

    StringRef space;
    if (failed(parser.parseKeyword(&space)) || failed(parser.parseGreater())) return {};
    if (space == "gm") return AddressSpaceAttr::get(getContext(), AddressSpace::GM);
    if (space == "ub") return AddressSpaceAttr::get(getContext(), AddressSpace::UB);
    parser.emitError(parser.getNameLoc(), "unknown hivm address space");
    return {};
}

void HIVMDialect::printAttribute(Attribute attr, DialectAsmPrinter& printer) const {
    auto space = attr.cast<AddressSpaceAttr>();
    printer << "address_space<" << spelling(space.getSpace()) << ">";
}

void mlir::hivm::printInsOuts(Operation* op, OpAsmPrinter& printer, unsigned num_ins) {
    auto print_list = [&](unsigned begin, unsigned end) {
        printer << '(';
        for (unsigned i = begin; i != end; ++i) {
            if (i != begin) printer << ", ";
            printer.printOperand(op->getOperand(i));
        }
        printer << " : ";
        for (unsigned i = begin; i != end; ++i) {
            if (i != begin) printer << ", ";
            printer.printType(op->getOperand(i).getType());
        }
        printer << ')';
    };

    printer << " ins";
    print_list(0, num_ins);
    printer << " outs";
    print_list(num_ins, op->getNumOperands());
}

LogicalResult mlir::hivm::verifyMemrefs(Operation* op, ArrayRef<AddressSpace> spaces) {
    MemRefType expected;
    for (auto item : llvm::enumerate(op->getOperands())) {
        auto type = item.value().getType().dyn_cast<MemRefType>();
        if (!type || !type.getElementType().isF32()) return op->emitOpError("requires f32 memrefs");
        auto space = type.getMemorySpace().dyn_cast<AddressSpaceAttr>();
        if (!space || space.getSpace() != spaces[item.index()])
            return op->emitOpError("requires address space ") << spelling(spaces[item.index()]);
        if (!expected)
            expected = type;
        else if (type.getShape() != expected.getShape() || type.getElementType() != expected.getElementType())
            return op->emitOpError("requires equal memref types");
    }
    return success();
}
