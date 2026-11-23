#!/usr/bin/env python3
"""Local-AI 2x enhancer: Real-ESRGAN (RRDBNet) on PyTorch + MPS.

Per asset:
  1. palette -> RGB; transparent (index 0) and colour-cycling (229..254)
     pixels are holes filled from neighbours (the model never sees the black
     background or the white cycling placeholders).  Floor tiles are wrapped
     with copies of themselves on the isometric lattice (seamless edges).
  2. model upscale (x2plus is native 2x; x4 models are box-downsampled 4x->2x),
     tiled with overlap so large screens fit in memory.
  3. colour-drift correction: the result is box-downsampled back to 1x and
     the low-pass of (source - downsampled) is added back at 2x, so average
     colours match the palette art while the model's detail is kept; 30% (60%
     for tiles) of the high-pass difference is re-added too (--detail,
     --tile-detail), because the model otherwise erases fine regular textures
     (1-px stripes) and treats neighbouring tiles inconsistently.
  4. alpha: separately, crisp nearest-2x of the source silhouette; cycling
     pixels alpha 0 (hdlib.compose_rgba).

Weights are not committed; see MODELS.md (download into tools/hd/models/).

  enhance_ai.py                        # all manifest assets, default model
  enhance_ai.py --model anime6b mainmenu
"""

from __future__ import annotations

import argparse
import json
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

import hdlib

PACK = "ai"

MODELS = {
    # name: (file, scale, num_block)
    "x2plus": ("RealESRGAN_x2plus.pth", 2, 23),
    "x4plus": ("RealESRGAN_x4plus.pth", 4, 23),
    "anime6b": ("RealESRGAN_x4plus_anime_6B.pth", 4, 6),
}
DEFAULT_MODEL = "x2plus"
TILE_PAD = 16


# --- RRDBNet (architecture of basicsr.archs.rrdbnet_arch, same state-dict keys)

class ResidualDenseBlock(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf, gc=32):
        super().__init__()
        self.rdb1 = ResidualDenseBlock(nf, gc)
        self.rdb2 = ResidualDenseBlock(nf, gc)
        self.rdb3 = ResidualDenseBlock(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, scale=4, num_block=23, nf=64, gc=32):
        super().__init__()
        self.scale = scale
        in_ch = 3 * (4 if scale == 2 else 16 if scale == 1 else 1)
        self.conv_first = nn.Conv2d(in_ch, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(num_block)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        if self.scale == 2:
            x = F.pixel_unshuffle(x, 2)
        elif self.scale == 1:
            x = F.pixel_unshuffle(x, 4)
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


def device() -> torch.device:
    return torch.device("mps" if torch.backends.mps.is_available() else "cpu")


def load_model(name: str) -> tuple[RRDBNet, int]:
    file, scale, blocks = MODELS[name]
    path = hdlib.MODELS_DIR / file
    if not path.exists():
        raise SystemExit(f"{path} missing: download it as described in tools/hd/MODELS.md")
    net = RRDBNet(scale=scale, num_block=blocks)
    state = torch.load(path, map_location="cpu", weights_only=True)
    state = state.get("params_ema", state.get("params", state))
    net.load_state_dict(state, strict=True)
    return net.eval().to(device()), scale


@torch.inference_mode()
def run_model(net: RRDBNet, scale: int, rgb: np.ndarray, tile: int = 192, overlap: int = 16) -> np.ndarray:
    """rgb float HxWx3 in 0..255 -> model output (H*scale)x(W*scale)x3."""
    h, w, _ = rgb.shape
    # reflect pad so borders get context and dims are even (x2plus unshuffle)
    ph, pw = 8 + (h % 2), 8 + (w % 2)
    x = np.pad(rgb, ((8, ph), (8, pw), (0, 0)), mode="reflect") / 255.0
    t = torch.from_numpy(x.transpose(2, 0, 1).copy()).float().unsqueeze(0).to(device())
    H, W = t.shape[2:]
    out = torch.zeros((1, 3, H * scale, W * scale), device=device())
    step = tile - 2 * overlap  # all of tile/overlap/H/W are even -> even crops
    for y0 in range(0, H, step):
        for x0 in range(0, W, step):
            ys, xs = max(0, y0 - overlap), max(0, x0 - overlap)
            ye, xe = min(H, y0 + step + overlap), min(W, x0 + step + overlap)
            r = net(t[:, :, ys:ye, xs:xe])
            ch, cw = min(step, H - y0), min(step, W - x0)
            oy, ox = (y0 - ys) * scale, (x0 - xs) * scale
            out[:, :, y0 * scale:(y0 + ch) * scale, x0 * scale:(x0 + cw) * scale] = \
                r[:, :, oy:oy + ch * scale, ox:ox + cw * scale]
    res = out[0].clamp(0, 1).permute(1, 2, 0).cpu().numpy() * 255.0
    return res[8 * scale:(8 + h) * scale, 8 * scale:(8 + w) * scale]


def box_down(a: np.ndarray, f: int) -> np.ndarray:
    h, w = a.shape[0] // f, a.shape[1] // f
    return a[:h * f, :w * f].reshape(h, f, w, f, -1).mean((1, 3))


def gaussian(a: np.ndarray, sigma: float) -> np.ndarray:
    r = max(1, int(3 * sigma))
    k = np.exp(-np.arange(-r, r + 1) ** 2 / (2 * sigma * sigma))
    k /= k.sum()
    p = np.pad(a, ((r, r), (r, r), (0, 0)), mode="edge")
    tmp = sum(k[i] * p[:, i:i + a.shape[1]] for i in range(2 * r + 1))
    return sum(k[i] * tmp[i:i + a.shape[0]] for i in range(2 * r + 1))


def colour_correct(ai2x: np.ndarray, src: np.ndarray, sigma: float = 1.5, detail: float = 0.0) -> np.ndarray:
    """Add back the low-frequency colour difference between the source and
    the model output (measured at source resolution).  `detail` > 0 also
    re-adds that fraction of the high-frequency difference (texture the
    model smoothed away, but also some dither)."""
    from enhance_scripted import bilinear2x
    diff = src - box_down(ai2x, 2)
    low = gaussian(diff, sigma)
    return ai2x + bilinear2x(low + detail * (diff - low))


def enhance_indices(net, scale, indices: np.ndarray, palette: np.ndarray, tile: bool = False,
                    correct: bool = True, detail: float = 0.3) -> np.ndarray:
    if tile:
        work, _ = hdlib.wrap_pad_tile(indices, TILE_PAD)
    else:
        work = indices
    transparent, cycling = hdlib.masks(work)
    rgb = hdlib.fill_holes(palette[work].astype(np.float32), transparent | cycling)
    big = run_model(net, scale, rgb)
    if scale != 2:
        big = box_down(big, scale // 2)
    if correct:
        big = colour_correct(big, rgb, detail=detail)
    if tile:
        p = 2 * TILE_PAD
        big = big[p:p + 2 * indices.shape[0], p:p + 2 * indices.shape[1]]
    return np.clip(big, 0, 255)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="asset names or keys (default: all in manifest)")
    ap.add_argument("--model", choices=sorted(MODELS), default=DEFAULT_MODEL)
    ap.add_argument("--pack", default=PACK, help="output pack name (default: ai)")
    ap.add_argument("--no-colour-correct", action="store_true")
    ap.add_argument("--detail", type=float, default=0.3,
                    help="fraction of source high-frequency detail re-added (default 0.3)")
    ap.add_argument("--tile-detail", type=float, default=0.6,
                    help="same for floor/roof tiles (default 0.6: keeps neighbouring tiles' "
                         "texture consistent, fewer visible tile diamonds)")
    args = ap.parse_args()
    assets = hdlib.select(hdlib.load_manifest(), args.names)
    t0 = time.perf_counter()
    net, scale = load_model(args.model)
    print(f"model {args.model} on {device()} loaded in {time.perf_counter() - t0:.2f}s")
    timings = {}
    for asset in assets:
        t0 = time.perf_counter()
        idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
        big = enhance_indices(net, scale, idx, pal, tile=hdlib.is_tile(asset),
                              correct=not args.no_colour_correct,
                              detail=args.tile_detail if hdlib.is_tile(asset) else args.detail)
        rgba = hdlib.compose_rgba(big, idx)
        dst = hdlib.save_outputs(args.pack, asset, rgba, idx)
        dt = time.perf_counter() - t0
        timings[asset["key"]] = round(dt, 3)
        print(f"{dt:7.2f}s  {dst.relative_to(hdlib.REPO_ROOT)}")
    tfile = hdlib.WORK_DIR / f"timings_{args.pack}.json"
    old = json.loads(tfile.read_text()) if tfile.exists() else {}
    old.update(timings)
    old["_model"] = args.model
    tfile.write_text(json.dumps(old, indent=1))


if __name__ == "__main__":
    main()
