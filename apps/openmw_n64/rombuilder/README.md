# ROM builder tools

The ROM builder converts a player's own Morrowind files for the N64
(PLAN_ALL_ON_N64.md §4.3, §4.7). These are its first pieces.

## census: will it fit in 64 MB?

Measures what Morrowind (no expansions) would take on a 64 MB cartridge,
from your `Data Files` folder, for three quality tiers:

```bash
make -C nifstat                        # once: OpenMW's NIF reader, built for your PC
cd census
npx tsx census.ts "/path/to/Morrowind/Data Files" --json census.json
npx tsx census.ts "/path/to/Morrowind/Data Files" --texture-sample 10   # quicker
```

It reads Morrowind.esm, Morrowind.bsa and any loose files, and for each
category does the conversion (or a measured estimate of it):

| Category | How it is measured |
|---|---|
| Records, dialogue, books, scripts, cells | the ESM's records, compressed in 16 KB chunks (so each can be read on its own) |
| Terrain | each cell's LAND record without its normals (rebuilt on the console), compressed |
| Textures, icons, book art | decoded and encoded as CHROMA64's C64T at the tier's size cap and quality |
| Meshes, animation | vertices, triangles and keys counted by `nifstat`, priced per item (constants at the top of `census.ts`) |
| Voice, music, sound effects | durations from the files, priced at the tier's Opus bitrate |
| Code | the linked-size measurement from PLAN §3; not from your files |

It needs a CHROMA64 checkout next to this repository (`--chroma64 DIR`
otherwise) for the C64T encoder and the DDS/TGA readers. Nothing is written
except the optional JSON report; your files are only read.

Tested so far on generated Morrowind-format data (`../tools/make_test_data.py`)
and OpenMW's example-suite, not on Morrowind itself. The first real run is
milestone S1b.

## nifstat

`nifstat Morrowind.bsa "Data Files"` prints, per NIF (JSON Lines), the
drawn shapes, vertices and triangles, collision geometry, particle vertices,
animation keys, skin weights and the textures it names. It is OpenMW's own
`components/nif` and `components/bsa` built for the PC (with `-DOPENMW_N64`,
which leaves out Bullet), so it reads every file exactly as the game does.
