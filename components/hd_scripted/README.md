## Component: scripted HD pack

`mods/narg_hd_scripted.dat`: the whole scripted (deterministic, numpy) HD pack,
`hd/scripted/`, made by `engine/tools/hd/enhance_scripted.py`. The default
`mixed` pack only needs the few scripted images `mods/narg_hd.dat` already
holds; this pack is for `--pack scripted` (`[graphics] hd_pack=scripted`).

The images are derived from the game's art and are not kept in this
repository: `extra/package.sh` packs them from the locally generated packs.
