# FidelityFX SDK

AMD FidelityFX SDK, used for the FSR 3.1 upscaler's GPU sources. Unmodified upstream copy.

- Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- Version: v1.1.4, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
- License: MIT, in `LICENSE.txt`. Keep it with any distribution.

What is here and why:

| | |
|---|---|
| `include/FidelityFX/gpu/` | The GPU-side headers the passes include: the common core, `fsr1/`, `spd/` (the single-pass downsampler the pyramids use), `fsr3upscaler/`, `frameinterpolation/` and `opticalflow/` |
| `shaders/fsr3upscaler/*.glsl` | The eight upscaler passes, as AMD ships them |
| `shaders/frameinterpolation/*.glsl` | The eleven frame-interpolation passes, from `sdk/src/backends/vk/shaders/frameinterpolation/` |
| `shaders/opticalflow/*.glsl` | The seven optical-flow passes frame interpolation runs on, from `sdk/src/backends/vk/shaders/opticalflow/` |
| `LICENSE.txt` | AMD's MIT notice |

No runtime library is needed, and none is included: the passes are compiled from these sources into
`gpu/vk/shaders/fsr3_*_comp_spv.h` by `scripts/fsr3_shaders.py`, which is also what
`tests/test_fsr3_shaders.py` runs to prove the checked-in headers still match. This is the SDK-source
route rather than AMD's FidelityFX API, which would load AMD's own Vulkan runtime and is Windows
only: these passes run on any GPU, on Windows and Linux alike.

Nothing in this folder is edited. A change belongs in a new folder with its own note, per
`CONTRIBUTING.md`.

tree-sha256: 40998920f695589fbb7230198c58ccc501ae958e42875c33a4981dde7f115aac
