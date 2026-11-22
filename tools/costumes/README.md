# The PS Vita costumes

The PS Vita version of MK9 (PCSE00023) has costumes the Xbox 360 game does not. mk9-lauta adds the ones made of
things the Xbox 360 game already has: a retro look (mesh, textures, the look's materials) that another character's
costume uses, worn by this character with its own colors. The Vita bakes each costume's colors into its own textures;
the Xbox 360 tints the look's textures with a palette color per costume (`AlternatePaletteColorA`, `SSSColor`,
`GlowColor` on the costume's material instances), so a costume here is the look's packages plus the Vita's colors.

| Costume | Built from | Look |
|---|---|---|
| `scorpion_d`, `subzero_d`, `reptile_d`, `ermac_d` | the character's C costume (MK1 retro) | `RetroNinja_Male_MK2` (Smoke D, Noob D) |
| `mileena_c` | Mileena D (MK3 retro) | `RetroNinja_Female_MK2` (Kitana C, Jade C) |
| `skarlet_c`, `skarlet_d` | Skarlet's own package (she has no retro costume) | `RetroNinja_Female_MK2` / `_MK3` (Kitana and Jade C / D) |

Each comes with its Babality (`NPC_<Name>_<Letter>_Baby`). Not done: the MK3 males (Scorpion, Sub-Zero, Reptile,
Ermac, Smoke, Noob and Rain E), Kenshi B and Skarlet B. Their looks exist only on the Vita (`CHAR_RetroNinja_Male_MK3`:
a 58-bone mesh whose bones all are in the Xbox 360's 143-bone male skeleton, 24-byte vertices, DXT5 textures; their
cloth is PhysX data cooked for the Vita), so they need the Vita's meshes and textures converted: from the player's own
copy of the Vita game, since this project ships no game data.

No game data is in the repository or in the release. The program has, compiled in, `app/costumes/vita_costumes.bin`:
for each costume its INI lines and, for each of its two packages, a patch that rebuilds it from the player's own game
files (copies from their decompressed images, plus the few bytes that are new). At the first start
`app/src/mk9_vita_costumes.cpp` makes the packages in `cache/costumes/Asset`, a `Coalesced.ini` and an `Xbox360TOC.txt`
from the game's own with the costumes' lines, and mounts that folder over the game's as an overlay device: the game
files are not modified.

## How a costume package is made

`build_costume.py` (costumes made from another letter's) and `build_default.py` (Skarlet's, from her own package):

1. The base package becomes the costume's: its seekfree container (`<package>-%08x`, `mk9hash.mk9_hash`), the fighter
   script's symbols (`char_<x>_<letter>_player_data`, `..._audio_override`: FNV-1) and the summary's lists renamed at
   the same length.
2. The look's groups are transplanted (`transplant.py`) from two donor costumes that have it: the references in the
   transplanted objects are found by diffing the two donors (what differs between them points into each package) and
   remapped to this package's objects.
3. The look's package is put in the forced-packages list (in the dropped look's place) and its objects in the
   `CombinedReferencer` (`referencer.py`); without both the game does not find the look's top-level objects.
4. The costume's material instances (`Materials.*` and, in the baby, `CHAR_<costume>.Materials.*`) take the donor's
   (parented to the look's), with the Vita's colors (`vita_mics.py` reads them from the Vita's own packages:
   `MK9_VITA_ASSET` = the Vita game's `Asset` folder).
5. The Babality's script names the baby's mesh in its string table (`NPC_RetroNinja_..._Baby`,
   `Meshes.RetroNinja_..._Baby`): the game takes the mesh from there, not from the INI.
6. Saved with chunks cut at export boundaries (an object split between two chunks makes the loader read in a loop);
   inline bulk offsets fixed, shader caches kept in place.

Skarlet's costumes start from her default packages, whose own mesh and animations at the package's root are renamed
and dropped from the referencer (`build_default.hide_base_groups`; the Vita's `CHAR_Skarlet_C` and `NPC_Skarlet_C_Baby`
have none). The game looks for `Meshes.<character>` (and `Meshes.Reflection`) in a costume's package first: found, it
takes the costume for one with its own mesh, the look of `[CHAR_X_Y] SourceAsset`/`MeshName` is not used and the
Babality's baby is not dressed in the costume's materials (it came out in the look's grey).

The summary's lists after the chunk table (the forced packages, then the script packages) have their sizes in the two
ints before them; `transplant.Model.summary_tail` sets them again (a list grown, or a name renamed at another length).

## Making the bundle

```
python build_costume.py <game>/Asset out/vita_build scorpion_d subzero_d reptile_d ermac_d mileena_c
python build_default.py <game>/Asset out/vita_build skarlet_c skarlet_d
python setup_costumes.py <game> out/vita_root out/vita_build scorpion_d subzero_d reptile_d ermac_d mileena_c skarlet_c skarlet_d
# test with game_data_root = out/vita_root and mk9_vita_costumes = false (versus, each costume, its Babality)
python make_bundle.py <game> out/vita_build out/vita_root app/costumes/vita_costumes.bin
```

`setup_costumes.py` makes a test game folder of hard links with the costumes registered (`make_test_root.py`,
`ini_set.py`, `fix_toc.py`). `make_bundle.py` takes the INI lines from that tested folder and makes each package's
patch (`makepatch.py`; `--apply` checks one). The DLC ids are 20001 on, in the bundle's order.

Patch format (big-endian): `MK9C`, u32 1, u16 sources (u16 path, u64 size, SHA-256), u16 target path, u64 size,
SHA-256, u32 length + zlib of the operations (`1` copy: varint source, offset, length; `2` data: varint length, bytes;
`0` end). The offsets are in the source's decompressed image: the file's bytes up to its first chunk, then the chunks
decompressed at their offsets. Bundle format: `MK9V`, u32 1, u32 costumes; per costume u16 key, u32 DLC id, u16 INI
edits (u8 kind: 0 a line at a section's top, 1 a key set in a section made if missing; u16 section, u16 line), u16
patches (u32 length + patch).

Online, both games must have the same set: the rooms compare `vita-<signature>` in the content they announce (with
Kratos), the session's hello carries a bit for it.
