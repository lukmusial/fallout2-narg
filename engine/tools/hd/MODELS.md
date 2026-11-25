# Models used by `enhance_ai.py`

Weights are **not** committed (`tools/hd/models/` is gitignored). Download them
from the official Real-ESRGAN GitHub releases (xinntao/Real-ESRGAN, BSD-3-Clause):

```sh
mkdir -p tools/hd/models && cd tools/hd/models
curl -LO https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth
# optional alternatives (--model x4plus / --model anime6b)
curl -LO https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth
curl -LO https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth
shasum -a 256 *.pth
```

| `--model` | File | Release | Arch | Native scale | SHA-256 |
|---|---|---|---|---|---|
| `x2plus` (default) | RealESRGAN_x2plus.pth (67 MB) | v0.2.1 | RRDBNet, 23 blocks, pixel-unshuffle input | 2x | `49fafd45f8fd7aa8d31ab2a22d14d91b536c34494a5cfe31eb5d89c2fa266abb` |
| `x4plus` | RealESRGAN_x4plus.pth (67 MB) | v0.1.0 | RRDBNet, 23 blocks | 4x, box-downsampled to 2x | `4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1` |
| `anime6b` | RealESRGAN_x4plus_anime_6B.pth (18 MB) | v0.2.2.4 | RRDBNet, 6 blocks | 4x, box-downsampled to 2x | `f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da` |

Runtime: PyTorch 2.15.0 (`cp315` macOS arm64 wheel from PyPI) on the MPS
backend, float32. The RRDBNet architecture is re-implemented in
`enhance_ai.py` with the same state-dict keys (`params_ema`), loaded with
`strict=True`; `basicsr`/`realesrgan` pip packages are not needed.

Why `x2plus`: it is the only official model with a native 2x output, so no
resampling step is needed. In a side-by-side check on `mainmenu`, `car1` and
cave tiles it kept the most texture. `x4plus` looks similar but is about 1.6x
slower. `anime6b` gives the crispest lines but flattens the painted textures
into a cartoon look.

Not used: `realesrgan-ncnn-vulkan` (the prebuilt binary needs MoltenVK, and its
output can't be colour-corrected in-process) and ONNX/CoreML conversion (not
needed while torch wheels exist for Python 3.15).
