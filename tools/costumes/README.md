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

Each comes with its Babality (`NPC_<Name>_<Letter>_Baby`). The others (the MK3 males: Scorpion, Sub-Zero, Reptile,
Ermac, Smoke, Noob and Rain E; Kenshi B and Skarlet B) are made of the Vita's own meshes and textures, converted to the
Xbox 360's formats: see [Costumes made of the Vita's data](#costumes-made-of-the-vitas-data).

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

## Costumes made of the Vita's data

| Costume | Built from (Xbox 360) | What the Vita gives |
|---|---|---|
| `scorpion_e`, `subzero_e`, `reptile_e`, `ermac_e` | the character's C costume (MK1 retro) | the MK3 retro look |
| `smoke_e`, `noob_e` | the character's D costume (MK2 retro) | the MK3 retro look |
| `rain_e` | Rain's default package + the MK2 look (Smoke D, Noob D), as Skarlet's | the MK3 retro look |
| `kenshi_b`, `skarlet_b` | the character's default package | the costume's own mesh |

```
python build_mk3.py <game>/Asset <Vita>/Asset out/vita_mk3 scorpion_e subzero_e reptile_e ermac_e smoke_e noob_e
python build_rain.py <game>/Asset <Vita>/Asset out/vita_mk3
python build_own.py <game>/Asset <Vita>/Asset out/vita_mk3 kenshi_b skarlet_b
# a test folder with them (the Vita's INI sections; the MK3 look: the costume's own package), played: versus, Babality
python make_mk3_pack.py <game> out/vita_mk3 <test folder> out/vita_pack/vita_costumes_pack.bin
```

`<Vita>` is the Vita game's files (PCSE00023). The pack has the bundle format; its new bytes are the Vita's data,
converted, so it is not built into the program: it is hosted apart and downloaded (`mk9_vita_costumes.cpp`, as the
Kratos port is), and `mk9_vita_costumes_pack` reads a local one for the tests. Its DLC ids follow the built-in
bundle's (20008 on).

The conversion (`vita_convert.py`, `xenos.py`, `skelmesh.py`):

- Tagged properties: little-endian to big-endian, names and object references mapped through the objects' paths; the
  arrays whose element type the tags do not tell by their property's name (`ARRAY_ELEMENTS`).
- SkeletalMesh: the same layout on both (472/46 and 501/48) but the GPU vertices: the Vita's 24 bytes (position half4,
  normal ubyte4, UV half2, bone indices and weights) become the Xbox 360's 48 (position float3, TangentX/Y/Z packed as
  the bytes of a big-endian DWORD of (x, y, z, w) with w 0, the UV twice, the bone indices and weights in the reverse
  byte order); the tangents are made from the triangles. The skeleton stays the Vita's (its bones are all in the Xbox
  360's skeletons; the animations go by bone name). After the LODs, the dismemberment data has the grammar of the
  game's reader (`USkeletalMesh::Serialize`, sub_82924928): three object arrays, 18 regions of item groups (25-byte
  items), 19 records `{u8 id; i32 used; TArray<{i32, i32, TArray<u8>, TArray<i32>, TArray<i32>}>; i32 mirror}`, a
  14-byte-element array and four object arrays. The Vita's unused records hold uninitialized memory in `mirror`
  (3516, 1583: the game took it for a record index and read past the array): 0 there, as the Xbox 360's baby has.
  The Vita adds 4868 bytes of tables after it that the Xbox 360 game does not read: left out. `f80` (0xCDCDCDCD on
  the Vita) is 0.
- No `PhysicsAsset` property is added where the Xbox 360 object had none: an explicit None crashed the Babality's baby.
  The look's physics assets, dismemberment states and sockets are the Xbox 360 look's (their bones are in the Vita's
  skeletons; the swords' sockets moved where the MK3 look carries them); the cloth (PhysX data cooked for the Vita) is
  left out (`ClothSet` None) and the Vita's `SphereProxy` too.
- Texture2D: the Vita's DXT5 levels are Morton-swizzled by 4x4 block (y in the even bits); its 1024 level is not there
  (a stripped mip), so the texture is 512. The Xbox 360's: each level tiled (32x32-block tiles, UE Viewer's
  `GetXbox360TiledOffset`), at least one tile big, the levels from 16 texels down packed in one tile at the tail's
  offsets (Xenia's `GetPackedMipOffset`; `MipTailBaseIdx`), the following ones empty (flags 0x21, size and offset -1),
  every word byte-swapped by 16 bits. Checked by decoding the game's own textures with it.

The game draws a fighter with instances made from its look's material instances (`CHAR_<look>.Materials.*`) and
copies only the colors of the costume's own (`Materials.*`, sub_823C5D40: `Materials.Character[_AP]_MIC`): their
textures never replace the look's. The Vita's MK3 costumes set their own textures (`Textures.<Char>_E_*`, baked
colors) in `Materials.*`. So each costume here has its own look package, `CHAR_RetroNinja_Male_MK3_<Char>` (the
INI's `SourceAsset`), whose instances take the costume's values (`build_mk3.costume_into_look`): two MK3 costumes
in one fight do not share them.

The fighter's script names some of the costume's own pieces by its letter (found by diffing the scripts' strings):
Scorpion's and Reptile's masks (`SC_C_Cowl_Sk` -> `SC_E_Cowl_Sk`, `RE_E_Cowl_Sk`) and their FX groups (`fx_scorpion_e`,
`FX_Reptile_E`): renamed, and the Vita's masks converted (`build_mk3.PIECES`). Smoke E keeps the MK2 look's smoke
splines (`FX_Smoke_D`: the Vita's are cooked natively, another format).
