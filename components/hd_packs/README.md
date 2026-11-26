## Component: HD art packs (local builds only)

The releases hold no art: the engine upscales the game's own pictures while
it runs. Pre-made HD art packs are still supported, for those who make them
from their own copy of the game with `engine/tools/hd` (see its README):

| Pack | `[graphics] hd_pack` | What |
|---|---|---|
| `ai` | `ai` | AI-upscaled images (Real-ESRGAN x2plus), ground variants, palm repaint |
| `scripted` | `scripted` | the offline version of the engine's own upscale (`enhance_scripted.py`) |
| `mixed` | `mixed` | `ai`, with `scripted` images where the AI does poorly (manifest in `engine/files/ce.dat/hd/mixed/manifest.txt`) |
| `rpu` | (with `mixed`) | the same, made from the Restoration Project's own art (`mod_pack.py`) |

A pack image is only drawn while the game's own picture is the one it was
made from (`hd/<pack>/legacy.txt`, `hd_verify=1`); everything else is
upscaled by the engine as without a pack.

`extra/package.sh <platform> <engine build> <packs dir>` packs them into
`mods/narg_hd.dat` (and `narg_v<VERSION>_rpu.zip` with `mods/narg_hd_rpu.dat`;
`NARG_WITH_SCRIPTED=1` adds `mods/narg_hd_scripted.dat`), and
`narg-install --pack mixed` loads them. The packs are derived from the
game's copyrighted art: archives with them are for your own use, not for
distribution.
