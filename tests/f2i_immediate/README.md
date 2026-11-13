# Native F2I immediate regression

This game-free fixture constructs its instructions from independently chosen
registers, float values and bit patterns. It calls the production Ampere lowerer,
Maxwell decoder and conversion implementations, then emits real IR and SPIR-V.
No game, dump, cache, driver or GPU is used.

The immediate contains all 32 source bits. Bits 62 and 63 of the native
instruction belong to that literal; they are not the register source's abs/neg
modifiers. The test checks literal preservation, signedness, destination width,
four rounding modes, FTZ, ordinary/inverted predicates and `@!PT` skipping. It
also checks existing register/cbuf/uniform source forms and explicitly rejects
immediate F16, F64, 64-bit output, non-NTZ and unknown modes. The accepted U16
form remains FTZ/nearest only.

```sh
CC=clang CXX=clang++ cmake -S tests/f2i_immediate -B build-f2i \
  -DREPO_ROOT="$PWD" -DSPIRV_HEADERS_ROOT=/path/to/SPIRV-Headers
cmake --build build-f2i --parallel 2
build-f2i/f2i_immediate build-f2i/modules
for module in build-f2i/modules/*.spv; do
  spirv-val --target-env vulkan1.3 --uniform-buffer-standard-layout "$module"
done
```

It also takes literal inputs through F2I.S32.TRUNC from F32 and I2FP.F32.U32.RZ and
evaluates the IR the production passes leave (a small evaluator in the fixture): F2I
must saturate 2^31, 2^32, the largest float and +inf to INT32_MAX without handing
OpConvertFToS a value out of its range, and I2F must round toward zero (0xffffffff
to 0x4f7fffff), not to nearest as OpConvertUToF does.

The unmodified `232eecb` lowerer fails the first authored immediate case with
`unsupported native opcode`. The fixture tests the conversion path and its IR
controls; SPIR-V validation does not establish numerical results on a GPU or a
fix for any particular visible game symptom. The value checks are of the IR as
SPIR-V defines its instructions, not of a driver's results.

Encoding reference: [Mesa NAK, pinned sm70_encode.rs](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/b1fe47e94409d81924c8663f58fac97e66ab4527/src/nouveau/compiler/nak/sm70_encode.rs#2134)
(`OpF2I`, `encode_alu_base` and `encode_alu_imm`). The `0x105` base plus immediate
form 4 in bits 9–11 gives `0x905`. Inputs or outputs wider than 32 bits use a
different base, `0x111`. [NVIDIA PTX conversion documentation](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html#data-movement-and-conversion-instructions-cvt)
describes rounding, FTZ, saturation and NaN results; it is not a SASS encoding
specification.
