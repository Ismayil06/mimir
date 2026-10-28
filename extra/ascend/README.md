# The `ascend` plugin

This plugin lowers one direct-style MimIR function to MLIR text for AscendNPU-IR. A test picks the phase with `_compile`:

```mim
extern lam _compile (): compile.Phase = ascend.emit_hivm;
```

`_compile` is internalized by `optimize` before the phase runs, so the emitter sees only the kernel. `-X ascend:o=<file>` chooses the output (`-` is stdout). With no `-X`, the file is `<world>.ascend.mlir`.

## Files

| File | Owns |
| --- | --- |
| `ascend.mim` | The two phase axioms. Bootstrap turns them into `build/include/mim/plug/ascend/autogen.h`. |
| `ascend.cpp` | `mim_get_plugin`. Registers both phases and the `-X` arguments. No lowering. |
| `hfusion.cpp` | `emit_hfusion`. Prints Linalg text with `ostringstream`. |
| `hivm.cpp` | `emit_hivm`. Checks the kernel, then builds an MLIR module and prints it. |
| `hivm_stub.cpp` | Stand-in for `hivm.cpp` when CMake finds no MLIR. The phase rejects every program. |
| `mlir/hivm_dialect.*` | The `hivm` ops and `#hivm.address_space` this plugin constructs. |
| `mlir/hacc_dialect.*` | `#hacc.function_kind`. `hacc.entry` is a plain unit attribute, not a dialect op. |
| `lit/` | FileCheck tests. They do not need CANN. |

`hfusion.cpp` and `hivm.cpp` do not call each other. The duplicated checks (one external function, a legal symbol, static F32 shapes) are intentional: the two printers do not share a kernel IR.

## How a phase is reached

```
ascend.mim axiom
    -> autogen.h enum class (emit_hfusion / emit_hivm)
    -> Phase::hook in register_hfusion / register_hivm
    -> EmitHFusion::start or EmitHIVM::start
```

`Phase::hook<Axiom, PhaseClass>` keys the phase class on the axiom's flags. The C++ function must not be named `emit_hivm` or `emit_hfusion`: those names are the axiom enum classes, and a function with the same name makes `Phase::hook<ascend::emit_hivm, ...>` fail to compile.

## `emit_hfusion` (`hfusion.cpp`)

`Emitter` walks one external `Lam` and prints text. It accepts only `tensor.binary` of `math.arith.add` at mode `0` on F32, with exactly two parameters, and the inputs and the result must have the same static shape. The function returns the tensor. It does not allocate memory or mention GM or UB. This is the secondary path: the vendor compiler is expected to schedule it.

SSA names are invented here (`%arg0`, `%empty0`, `%result0`). `values_` remembers a `Def*` so a shared subexpression is printed once.

## `emit_hivm` (`hivm.cpp`)

The HIVM kernel is void. Every tensor argument is a global-memory memref, and the MimIR result becomes one extra memref argument that the kernel writes. Values that the kernel computes live in the unified buffer.

`EmitHIVM::start` does three steps, in this order:

1. `analyze` reads the MimIR world and returns a `Program`, or throws. No MLIR object exists yet.
2. `build` turns that `Program` into a verified module and returns the printed text. It does not throw.
3. The phase throws if verification failed, then writes the text.

Step 2 must not throw. The MLIR libraries linked into the plugin are compiled without exception unwind tables, so an exception crossing an MLIR frame aborts. Diagnostics that name a MimIR `Def` belong in `analyze`. `build` only reports verifier output.

### `Program` and `Instr`

`analyze` lowers the body with a post-order walk. Each UB value gets an integer id. `ids` maps a MimIR `Def*` to that id, so a value used twice is loaded or computed once.

| `Instr::Kind` | `dst` | `lhs` | `rhs` | Meaning |
| --- | --- | --- | --- | --- |
| `Load` | new UB id | GM argument index | unused | First use of a parameter. |
| `Bin` | new UB id | UB id | UB id | One `hivm.hir.v*`. `bin` selects add, sub, mul, or div. |
| `Store` | unused | UB id of the result | unused | Write that buffer to the last memref argument. |

`num_args` is the number of MimIR parameters. The MLIR function has `num_args + 1` arguments; the last one is the result. `num_ub` is the number of UB ids, which is also the number of `memref.alloc` operations.

A kernel `add(a, b)` becomes: `Load a`, `Load b`, `Bin add`, `Store`. A kernel `((a + b) - a) * b / a` still has two `Load`s, because `a` and `b` are entered in `ids` on first use.

### What `analyze` accepts

- One set, direct-style external `Lam` (not a continuation).
- A non-empty parameter list. Every parameter and the result have the same static F32 shape. Rank is the nesting of `Arr`, so `«4; «4; F32»»` is `memref<4x4xf32>`.
- A body that is a DAG of `tensor.binary` applied to `math.arith` `add`, `sub`, `mul`, or `div`, at mode `0`, with an F32 scalar type. The scalar operation is `uncurry_args<4>()` of the binary, index 1. The two tensor operands are index 3, projected into a pair.
- A total UB footprint of `elements * 4 * num_ub` bytes, at most `-X ascend:ub`. The default, `ascend950_ub_bytes` in `hivm.cpp`, is `UB_SIZE` from the Ascend950PR_9579 target spec (`2031616`).

`tensor.binary` has four curried pieces: element types, the scalar function, the shape, and the operand tuple. `match_f32_arith` looks only at the scalar function.

### What `build` emits

`build` loads four dialects (`func`, `memref`, the local `hivm`, the local `hacc`), creates one `func.func`, and walks `instrs`. Each `Load` and `Bin` is an `memref.alloc` in `#hivm.address_space<ub>` followed by the HIVM op. `Store` writes to the last block argument. The function attributes are the unit attribute `hacc.entry` and `#hacc.function_kind<DEVICE>`.

`module.verify()` uses the checkers on the local ops (`verifyMemrefs` in `mlir/hivm_dialect.cpp`). A failure here is a bug in `build`: `analyze` should already have rejected a bad kernel.

## The local dialects

CANN ships `bishengir-compile` as a static binary. It does not ship the HIVM C++ headers, and that compiler is LLVM 19 while this plugin links the system MLIR 15 libraries (`libmlir-15-dev`). `mlir/hivm_dialect.*` and `mlir/hacc_dialect.*` are a print-compatible subset, not the vendor classes.

They exist so `OpBuilder` can construct values and `ModuleOp::print` emits the spelling BiSheng parses:

```text
#hivm.address_space<gm>     AddressSpace::GM
#hivm.address_space<ub>     AddressSpace::UB
hivm.hir.load  ins(...) outs(...)
hivm.hir.store ins(...) outs(...)
hivm.hir.vadd  ins(lhs, rhs : ...) outs(dst : ...)
#hacc.function_kind<DEVICE>
```

`printInsOuts` prints the operand list and then the types. `num_ins` is 1 for load and store and 2 for a binary. The op name is printed by MLIR before `print` is called, which is why `print` starts with a space.

`VecBinaryOp<Kind>` is one C++ class for four operation names. `VecOpName<Kind>::value` is the string MLIR registers. `VAddOp` and the other aliases are the names `build` uses.

`MLIR_DECLARE_EXPLICIT_TYPE_ID` / `MLIR_DEFINE_EXPLICIT_TYPE_ID` pin the dialect and attribute type ids to one shared object. The op classes use implicit type ids, which is safe only because every use is inside `libmim_ascend.so`.

## Adding an operation

1. Give it an MLIR name in `mlir/hivm_dialect.h`. A binary with the same `ins`/`outs` shape can be another `VecBin` value plus a `VecOpName` specialization. A different shape needs its own op class and a `print` / `verify`.
2. Register it in `HIVMDialect::initialize` (`mlir/hivm_dialect.cpp`).
3. Teach `match_f32_arith` to recognize the MimIR axiom.
4. Teach the `binary` lambda in `build` to create the op. `Instr` can stay as it is while every new op is still two inputs and one output.
5. Add a `lit` test that FileChecks the new line, and a rejection test if the axiom is only partly supported.

Unary ops do not fit `Instr::Kind::Bin`: `rhs` would be unused and `printInsOuts` would still be called with `num_ins == 2`. Add a kind, or stop encoding operands as three ints.

## Adding an element type

`shape_of` rejects anything that is not F32, and `verifyMemrefs` requires `f32`. Both have to change together. `build` hard-codes `builder.getF32Type()` and a 4-byte element in the size check in `analyze`. The size check is `elements * 4 * num_ub`.

## Tests

`lit/*.mim` runs `%mim %s -X ascend:o=-` and FileChecks the text or the diagnostic. `add.mim` is HFusion. The `*_hivm.mim` files are HIVM. `reject_hivm_ub.mim` passes `-X ascend:ub=191` so the test does not depend on the default capacity: two inputs and one result, 16 F32 elements, is 192 bytes.

After editing a test, stage it before trusting `lit`:

```sh
cmake --build build --target mim_lit_tests
python3 lit/lit build/lit -a --filter 'extra/ascend'
```

## Build

`MIM_ASCEND_MLIR` defaults to `ON`. CMake looks for `MLIRConfig.cmake`, including `/usr/lib/llvm-*/lib/cmake/mlir`. When MLIR is found, `hivm.cpp` and the two dialect sources are compiled and linked to `MLIRFuncDialect`, `MLIRMemRefDialect`, `MLIRIR`, and `MLIRSupport`. The plugin's rpath includes the LLVM library directory so `libLLVM-15.so` resolves when `mim` loads the plugin. When MLIR is not found, `hivm_stub.cpp` is compiled instead and `emit_hfusion` is unchanged.

`bishengir-compile -enable-hivm-compile` parses the printed module and rewrites `hivm.hir.vadd` into library calls plus `set_flag` / `wait_flag`. On the CANN 9.1.0 image used while writing this, that link then fails with `undefined symbol: _mlir_ciface_vadd_1d_float`. The same error happens for a handwritten copy of the documented VecAdd kernel, so it is the bitcode link in that install, not a spelling bug in `build`.
