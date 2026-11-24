// mk9-lauta (2026-11-23): the character select's fighters in 3D (mk9_select_preview.h).
//
// The select screen (game state 7) is the Scaleform movie ui_ps_vs over an empty world (empty_map) whose camera stands
// at the origin looking down +X; the movie's background (an unnamed clip under _root.ps) is opaque and the fighters'
// art is _root.ps.characters_team<N>.bodies_1. Per side, with the option on:
//   - which fighter: the art the select script loads into that clip (SetImage, "ui_ps_vs_jpegs.BD_<fighter>.jpeg");
//     which costume: the package the costume menu (START) names when it shows one (the script localizes the costume's
//     package name, "CHAR_Scorpion_B", in section PlayerSelect, then writes the text into _root.ps.AltCostumeSide<N>)
//   - its package loaded as the fight loads it (UObject::LoadPackageAsync), its mesh found as the fighter's setup does
//     (sub_823CC2E8: Meshes.<fighter> in the costume's package, else [<costume>] SourceAsset/MeshName in the game's
//     ini, the retro looks), then shown as the scripts' skeletal actors are (native 824826F8: the MK object 1021,
//     sub_829F69E0, which spawns its own SkeletalMeshActor and has MK's animation player); a retro look wears the
//     costume's own material instances (its colors), drawn with MK's character shader (bUseMKDynamicShader: without
//     it MK9's lights leave a mesh black) and lit by three lights of our own (PointLightMovableSpawnable with MK's
//     AffectCharacters); player 2's side mirrored (Scale3D Y -1, as a fighting game turns its right side)
//   - its stance (the package's "Stance", looped by playing it again at its end; MK's anim data says the length) and,
//     when its pick is made (A on a fighter, or A in the costume menu; not START, which opens that menu: see the
//     sprintf native's hook), a performance at random among all of its own (mk9_select_performances.inc, made from
//     the game's cinematics by tools/select3d/perf_tables.py): one of its Story transitions (it speaks to its rival
//     before or after a Story fight: the line in the game's language, SND_VO_StoryTrans in English, Spanish, French,
//     German or Italian), one of its Story cutscenes' lines (SND_MOV_MK<nnn>, the same dubs) with its EndOfRound_Taunt,
//     its intro as before a fight (its line in English, its effects' sounds, its grunts) or its EndOfRound_Taunt with
//     a line of its voice (its taunt's, its victory's or its laugh). What it says is in the game's language: in another
//     than English, without the English-only lines (the intros', the taunts'). Between its stances, now and then one
//     of its other poses (Stance_Alt01-05, Idle1). The cinematic's package
//     loads while the fighter is shown; its animations and sounds come at their times (played as the select script
//     plays the announcer's), the fighter turned toward its rival meanwhile (the cinematics face their camera, the
//     rival beside it); the stance again at its end
//   - the art's clip and the movie's background hidden while a fighter is shown or loading (GFxMovieView::SetVariable
//     "_visible"); the art comes back for one that cannot be shown
// What it shows stays in the root set (the mesh, the animations, the voices; the lines' packages while the select
// screen is shown) and a garbage collection is asked for when the cursor rests, so browsing does not fill the memory:
// one that takes only the fighters it showed and shows no more, and its cinematics (HoldTheRest). Every collection on
// the select screen (the game's DLC manager's as well) waits for the packages loading to finish (FlushAsyncLoading)
// and keeps what the preview loads or shows (HoldThePreviews). The game deletes its MK objects when the select screen
// ends (the destructor's hook lets ours go); leaving it (or the option turned off) destroys the lights and shows the
// art again. A package the game's file system does not have is never asked for (a missing package ends the game).
// docs/online/engine-map.md, "La selección en 3D", has the addresses and how each was found.

#include "mk9_select_preview.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mk9_guest_call.h"
#include "online/fight_state.h"
#include "online/netplay.h"

REXCVAR_DEFINE_BOOL(mk9_select_preview, false, "MK9",
                    "Character select: each side's fighter in 3D (in the costume being chosen, in its stance; when "
                    "picked, taunting its rival: a Story line in the game's language, its intro or its taunt) instead "
                    "of the 2D art. Offline only");
REXCVAR_DEFINE_STRING(mk9_select_preview_place, "", "MK9",
                      "(tests) the fighters' place: distance,side,height,turn (units, units, units, 1/65536 turn); "
                      "empty: the defaults");
REXCVAR_DEFINE_STRING(mk9_select_preview_perform, "", "MK9",
                      "(tests) the picked fighter's performance: empty, one of its own at random; the package of one "
                      "(\"NIS_MK003_PRE\", \"Scorpion_CharIntro\"), \"taunt\" (its EndOfRound_Taunt) or \"none\"");
REXCVAR_DEFINE_STRING(mk9_select_preview_turn, "", "MK9",
                      "(tests) the performances' turn toward the rival, degrees: story,intro; empty: the defaults");
REXCVAR_DEFINE_STRING(mk9_select_preview_lights, "", "MK9",
                      "(tests) the lights' brightness: key,rim,fill; empty: the defaults");
REXCVAR_DEFINE_STRING(mk9_select_preview_test_pick, "", "MK9",
                      "(tests) a side's pick made at a select screen's frame, the game's own not needed: side:frame, "
                      "comma-separated (\"2:900\")");
REXCVAR_DEFINE_BOOL(mk9_select_preview_test_survey, false, "MK9",
                    "(tests) player 1's fighter plays every performance's body animation in turn and the log says how "
                    "far each one moves its hips (tools/select3d/perf_tables.py leaves out the ones that walk)");
REXCVAR_DEFINE_INT32(mk9_select_preview_test_survey_from, 0, "MK9",
                     "(tests) mk9_select_preview_test_survey: the performance it starts at (kPerformances' index)");
REXCVAR_DEFINE_BOOL(mk9_select_preview_test_gc_report, false, "MK9",
                    "(tests) each garbage collection the preview asks for is followed (30 frames later) by the packages "
                    "it took objects from, and how many");
REXCVAR_DEFINE_STRING(mk9_select_preview_test_toggle, "", "MK9",
                      "(tests) the select screen's frames at which mk9_select_preview is turned over (on to off, off to "
                      "on), comma-separated: the F2 option's checkbox while the select is shown");

namespace mk9::guest_call {
PPCFunc* Resolve(uint32_t address) { return rex::runtime::ResolveIndirectFunction(address); }
}  // namespace mk9::guest_call

namespace mk9::select_preview {
namespace {

using mk9::guest_call::Caller;

// ---------------------------------------------------------------------------------------------------------------------
// The game's memory.

uint32_t L32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LF(const uint8_t* base, uint32_t a) {
  const uint32_t v = L32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void S32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void SF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  S32(base, a, v);
}
// [a, a + n) mapped and readable (the heaps', the image's)
bool Readable(uint32_t a, uint32_t n) {
  if (a < 0x40000000u || a > 0xFFFFFFFFu - n) return false;
  auto* memory = rex::system::kernel_memory();
  auto* heap = memory ? memory->LookupHeap(a) : nullptr;
  return heap && heap->QueryRangeAccess(a, a + n - 1) != rex::memory::PageAccess::kNoAccess;
}
std::string CString(const uint8_t* base, uint32_t a, size_t max = 128) {
  std::string s;
  if (!a) return s;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(base[a + i]);
    if (!c) break;
    s += c;
  }
  return s;
}
std::string FStringAt(const uint8_t* base, uint32_t fs) {  // an FString: its characters are bytes in this game
  const uint32_t data = L32(base, fs), num = L32(base, fs + 4);
  if (!data || num == 0 || num > 512 || !Readable(data, num)) return {};
  return std::string(reinterpret_cast<const char*>(base + data), num - 1);
}
std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
bool StartsNoCase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && EqualNoCase(s.substr(0, prefix.size()), prefix);
}

// ---------------------------------------------------------------------------------------------------------------------
// The engine's objects (UE3). UObject: +0 vtable, +4 index in GObjObjects, +36 outer, +40 name (FName: index, number),
// +48 class. FName::Names' entries hold the text at +16.

constexpr uint32_t kObjects = 0x83416274;           // UObject::GObjObjects (TArray<UObject*>)
constexpr uint32_t kNames = 0x834161DC;             // FName::Names (TArray<FNameEntry*>)
constexpr uint32_t kAsyncPackagesNum = 0x834162A4 + 4;  // UObject::GObjAsyncPackages.Num
constexpr uint32_t kFlushAsyncLoading = 0x8272EC30;       // UObject::FlushAsyncLoading() (ProcessAsyncLoading, no limit)
constexpr uint32_t kGWorld = 0x8341ECF4;            // GWorld
constexpr uint32_t kGConfig = 0x833D9064;           // GConfig (FConfigCacheIni*)
constexpr uint32_t kGGameIni = 0x833DD318;          // GGameIni (the game ini's file name, TCHARs)
constexpr uint32_t kGameState = 0x833848D4;         // the game's state object: +4 the state, +64 asks for a GC
constexpr uint32_t kScriptVm = 0x8342CDF0;          // the scripts' VM: +60 the native's arguments (+4 the first)
constexpr int kStateSelect = 7;                     // GS_PSELECT

// The game's functions (guest addresses)
constexpr uint32_t kLoadPackageAsync = 0x82730F70;  // (const FString& name, callback, data, guid)
constexpr uint32_t kMKAlloc = 0x829DB018;           // MK's objects' allocator (size)
constexpr uint32_t kSkelActorCtor = 0x829F69E0;     // MK object 1021 (this, mesh, parts mask, keep bones, flag, ?)
constexpr uint32_t kSpawnActor = 0x828FEA18;        // UWorld::SpawnActor(world, class, FName, loc*, rot*, ...)
constexpr uint32_t kDestroyActor = 0x828E51A8;      // UWorld::DestroyActor(world, actor, bNetForce, bModifyLevel)
constexpr uint32_t kDetachComponent = 0x8288EFF8;   // AActor::DetachComponent(actor, component)
constexpr uint32_t kAttachComponent = 0x82884EE8;   // AActor::AttachComponent(actor, component)
constexpr uint32_t kClipRoot = 0x82A375A0;          // the movie's root clip wrapper of a clip wrapper
// MK objects' virtual methods (slots)
constexpr uint32_t kSlotDelete = 1;                 // the deleting destructor (flags 1: free)
constexpr uint32_t kSlotPlayAnim = 29;              // (anim, f1 start, f2 blend, f3, flags)
// UMeshComponent's
constexpr uint32_t kSlotSetMaterial = 124;          // SetMaterial(element, material) (execSetMaterial calls it)
// GFxMovieView's
constexpr uint32_t kSlotSetVariable = 16;           // (path, const GFxValue&, SetVarType)

uint32_t ObjectCount(const uint8_t* base) { return L32(base, kObjects + 4); }
uint32_t ObjectAt(const uint8_t* base, uint32_t i) { return L32(base, L32(base, kObjects) + 4 * i); }

std::string NameText(const uint8_t* base, uint32_t index) {
  if (index >= L32(base, kNames + 4)) return {};
  const uint32_t entry = L32(base, L32(base, kNames) + 4 * index);
  return entry ? CString(base, entry + 16, 96) : std::string();
}
std::string ObjName(const uint8_t* base, uint32_t o) {
  std::string n = NameText(base, L32(base, o + 40));
  const uint32_t number = L32(base, o + 44);
  if (number) n += "_" + std::to_string(number - 1);
  return n;
}
std::string ClassName(const uint8_t* base, uint32_t o) {
  const uint32_t c = L32(base, o + 48);
  return c ? ObjName(base, c) : std::string();
}
std::string Path(const uint8_t* base, uint32_t o) {
  std::string p;
  for (int d = 0; o && d < 8; ++d, o = L32(base, o + 36)) p = p.empty() ? ObjName(base, o) : ObjName(base, o) + "." + p;
  return p;
}
uint32_t Outermost(const uint8_t* base, uint32_t o) {
  for (int d = 0; d < 16 && L32(base, o + 36); ++d) o = L32(base, o + 36);
  return o;
}
bool IsObject(const uint8_t* base, uint32_t v) {
  if (!Readable(v, 56)) return false;
  const uint32_t i = L32(base, v + 4);
  return i < ObjectCount(base) && ObjectAt(base, i) == v;
}
// mk9-lauta (2026-11-24): an object a collection let go (UE3 2010's flags, the 64-bit word at +8: RF_Unreachable
// 0x2_00000000, RF_PendingKill 0x20000000_00000000; RF_BeginDestroyed 0x8000, RF_FinishDestroyed 0x10000 in the low
// half, by RF_RootSet 0x4000 and the RF_Native 0x04000000_00000000 the game's collection keeps), or one not loaded yet
// (RF_NeedLoad 0x200_00000000, RF_AsyncLoading 0x400_00000000). A collection destroys what it lets go over the next
// frames (IncrementalPurgeGarbage): until then it is still among GObjObjects and found by its name, and so is its
// package, which LoadPackageAsync then takes as loaded. The preview's lookups and roots take living objects only, and a
// fighter whose package is still going is asked for again (a guard: the tests of 2026-11-24 found none used so; what
// broke the fights was the DLC manager's collection, see sub_826DD740's hook).
constexpr uint32_t kDyingHigh = 0x00000002u | 0x20000000u | 0x00000200u | 0x00000400u;
constexpr uint32_t kDyingLow = 0x00008000u | 0x00010000u;
uint32_t g_dying_seen = 0;
bool Dying(const uint8_t* base, uint32_t o) {
  return (L32(base, o + 8) & kDyingHigh) != 0 || (L32(base, o + 12) & kDyingLow) != 0;
}
// A living object (among GObjObjects, not let go nor still loading); one found dying is said (the first few)
bool Alive(const uint8_t* base, uint32_t o, const char* what = nullptr) {
  if (!IsObject(base, o)) return false;
  if (!Dying(base, o)) return true;
  if (what && g_dying_seen++ < 24)
    REXLOG_WARN("[select3d] {} {}: being destroyed or not loaded (flags {:08X} {:08X}): left alone", what,
                ObjName(base, o), L32(base, o + 8), L32(base, o + 12));
  return false;
}
// A native class's object by its name ("SkeletalMesh"): native classes stay for the whole run, so it is kept
uint32_t ClassObject(const uint8_t* base, const char* name) {
  static std::vector<std::pair<std::string, uint32_t>> known;
  for (const auto& [n, c] : known)
    if (n == name) return c;
  for (uint32_t i = 0, count = ObjectCount(base); i < count; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (o && ObjName(base, o) == name && ClassName(base, o) == "Class") {
      known.emplace_back(name, o);
      return o;
    }
  }
  return 0;
}

// An object by its path ("CHAR_Scorpion.Meshes.Scorpion"; any case) and its class (the class's object; 0: any): a
// living one (g_found_dying: one there but dying)
bool g_found_dying = false;
uint32_t FindObject(const uint8_t* base, const std::string& path, uint32_t cls) {
  const size_t dot = path.rfind('.');
  const std::string leaf = dot == std::string::npos ? path : path.substr(dot + 1);
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o || (cls && L32(base, o + 48) != cls) || !EqualNoCase(ObjName(base, o), leaf)) continue;
    if (!EqualNoCase(Path(base, o), path)) continue;
    if (Alive(base, o, "the object")) return o;
    g_found_dying = true;
  }
  return 0;
}
uint32_t FindObject(const uint8_t* base, const std::string& path, const char* cls) {
  const uint32_t c = cls ? ClassObject(base, cls) : 0;
  return cls && !c ? 0 : FindObject(base, path, c);
}
// An animation by its sequence name among a package's (its seekfree container is "<package>-<hash>")
uint32_t FindAnim(const uint8_t* base, const std::string& package, const char* sequence) {
  const uint32_t cls = ClassObject(base, "AnimSequence");
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o || L32(base, o + 48) != cls || NameText(base, L32(base, o + 0x38)) != sequence) continue;
    const std::string top = ObjName(base, Outermost(base, o));
    if (StartsNoCase(top, package) && (top.size() == package.size() || top[package.size()] == '-') &&
        Alive(base, o, "the animation"))
      return o;
  }
  return 0;
}
// Its length in seconds: MK's anim data (AnimSequence +0xAC) holds it at +24 (and the frames at +20)
float AnimLength(const uint8_t* base, uint32_t anim) {
  const uint32_t data = L32(base, anim + 0xAC);
  if (!Readable(data, 32)) return 0;
  const float s = LF(base, data + 24);
  return std::isfinite(s) && s > 0.05f && s < 60.0f ? s : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The select movie

uint32_t g_view = 0;      // its GFxMovieView (the root clip wrapper's +88), from the art's SetImage
uint32_t g_background = 0;  // the number N of its background clip, _root.ps.instance<N> (0: not found yet)
uint32_t g_background_scan = 0;  // the next N to try
bool g_background_hidden = false;

// clip._visible = visible (SetVarType 0: a clip that is not there is not made); true when the clip is there
bool SetVisible(const PPCContext& ctx, uint8_t* base, const std::string& clip, bool visible) {
  if (!g_view) return false;
  Caller call(ctx, base);
  const uint32_t value = call.Alloc(16);  // GFxValue: no object interface, VT_Boolean (2), the bool
  call.Store32(value + 4, 2);
  base[value + 8] = visible ? 1 : 0;
  return call.Virtual(g_view, kSlotSetVariable, {call.Ascii(clip + "._visible"), value, 0}) != 0;
}

// The background: the first unnamed clip under _root.ps (Scaleform names them instance<N> in the order it makes
// them; the background is the sprite's first). Found by trying names, a few hundred a frame.
void FindBackground(const PPCContext& ctx, uint8_t* base) {
  if (g_background || !g_view) return;
  for (int k = 0; k < 400 && g_background_scan < 30000; ++k, ++g_background_scan) {
    if (SetVisible(ctx, base, "_root.ps.instance" + std::to_string(g_background_scan), true)) {
      g_background = g_background_scan;
      REXLOG_INFO("[select3d] the select movie's background is _root.ps.instance{}", g_background);
      return;
    }
  }
}

void HideBackground(const PPCContext& ctx, uint8_t* base, bool hide) {
  if (!g_background || g_background_hidden == hide) return;
  SetVisible(ctx, base, "_root.ps.instance" + std::to_string(g_background), !hide);
  g_background_hidden = hide;
}

// ---------------------------------------------------------------------------------------------------------------------
// The sides

struct Side {
  int index = 0;  // 0: _root.ps.characters_team1 (player 1's), 1: team2
  // what the select screen shows on this side
  std::string fighter;  // the art's fighter ("scorpion"); empty: none
  std::string costume;  // the costume menu's package ("CHAR_Scorpion_B"); empty: the default
  uint64_t wish_frame = 0;
  bool picked = false;  // this side's pick was made (the taunt to play)
  // the preview
  // kGone: the game deleted its object (the select screen ending): nothing more until the next fighter
  enum class State { kNone, kLoading, kShown, kFailed, kGone } state = State::kNone;
  std::string package;  // shown or loading
  uint64_t load_frame = 0;
  uint32_t retries = 0;   // loads asked for again while its package was still being destroyed
  uint64_t retry_at = 0;  // the frame the next is asked for (0: none waiting)
  uint32_t object = 0, actor = 0, component = 0;
  bool art_hidden = false;
  uint32_t lights[3] = {0, 0, 0};
  // its animations
  uint32_t stance = 0, taunt = 0;
  float stance_length = 0, taunt_length = 0;
  bool taunting = false;
  uint64_t anim_end = 0;  // the frame the current one ends
  // its voice for the pick (an FmodEvent of its own sound packages) and when it starts
  uint32_t voice = 0;
  uint64_t voice_frame = 0;
  // the objects it holds in the root set while shown (a collection would take what only MK's player points at)
  std::vector<uint32_t> rooted;
  // its performance for the pick (kPerformances' index; -1 its EndOfRound_Taunt and a line), its cinematic's package
  // loading while the fighter is shown; ready, the animations found (the body's with their starts from the first's)
  int perf = -1, last_perf = -2;
  bool perf_loading = false, perf_ready = false;
  std::vector<std::pair<uint32_t, float>> perf_body;
  uint64_t pick_wait = 0;  // the frame its pick began waiting for the package (0: not waiting)
  // while it performs: its animations and sounds still to come, and when it is over
  struct Due {
    uint64_t frame;
    uint32_t anim;      // an animation (0: a sound)
    std::string sound;  // "PACKAGE.event" or "@vo_x"
  };
  std::vector<Due> due;
  uint64_t perform_end = 0;
  // its turn from the stance's facing (1/65536 of a turn, toward the rival positive), and where it is going
  int32_t turn_now = 0, turn_to = 0;
  // mk9-lauta (2026-11-24): its other poses while it waits (its package's Stance_Alt01-05 and Idle1, with their
  // lengths), one now and then between its stances, and the frame the next may come
  std::vector<std::pair<uint32_t, float>> idles;
  uint64_t idle_next = 0;
};

Side g_sides[2] = {{0}, {1}};
uint64_t g_frame = 0;
uint64_t g_gc_frame = 0;
bool g_gc_due = false;
// (the collection the preview asks for, see HoldTheRest)
std::vector<std::string> g_loaded_fighters;  // the fighters ("scorpion") the preview loaded on this select screen
bool g_gc_ours = false;                      // the collection under way is the preview's
std::vector<std::string> g_gc_fighters;      // the fighters that collection may take
uint64_t g_load_block_until = 0;  // no load starts before this frame (a collection asked for is under way)
bool g_was_select = false;
uint64_t g_select_frames = 0;  // the frames of this select screen (mk9_select_preview_test_toggle)
// the costume menu: the last costume package localized (section PlayerSelect), and when
std::string g_costume_key;
uint64_t g_costume_frame = 0;
// the select script's trace "Player %d: selected" (A on a fighter, or START opening its costume menu): its player, when
int g_selected_player = -1;
uint64_t g_selected_frame = 0;
uint64_t g_highlight_frame = 0;  // when it last marked a pick made ("SelectionHighlight<N>.gotoAndPlay")

constexpr uint64_t kSettleFrames = 8;    // a wish held this long before it loads (the cursor passing through)
constexpr uint64_t kLoadTimeout = 900;   // 15 s
// PlayAnim's f2, as the bio viewer passes it (0.9) until its meaning is pinned down
constexpr float kStanceBlend = 0.9f, kTauntBlend = 0.9f;
constexpr uint32_t kLoopFlags = 2, kOnceFlags = 2;  // as the bio viewer plays its loop

// The performances for the pick, from the game's cinematics (tools/select3d/perf_tables.py): the Story transitions,
// where a fighter speaks to its rival before or after a Story fight (its line in the game's language: SND_VO_StoryTrans
// has English, Spanish, French, German and Italian), and the fighters' intros as before a fight (their line, in
// English, their effects' sounds and their grunts). A fighter's are all of its own; one of them at random each pick.
struct PerfDef {
  const char* fighter;  // the select's ("scorpion")
  char kind;            // 'S' a Story transition, 'I' an intro
  const char* package;  // the cinematic's (its animations come with it)
  const char* body;     // "sequence@start|...": the body's animations, each from its start (seconds)
  const char* face;     // "sequence@start": the face's (not played: see LogSubskeletons), or ""
  const char* cues;     // "time:PACKAGE.event;time:@vo_x;...": its sounds (@: the fighter's own voice sounds)
};
constexpr PerfDef kPerformances[] = {
#include "mk9_select_performances.inc"
};

struct Timed {
  std::string name;
  float at = 0;
};
// "a@1.5|b@2" (sep '|', mark '@': name, then time) or "1.5:a;2:b" (sep ';', mark ':': time, then name)
std::vector<Timed> ParseTimed(std::string_view list, char sep, char mark, bool time_first) {
  std::vector<Timed> out;
  while (!list.empty()) {
    const size_t end = list.find(sep);
    const std::string_view item = list.substr(0, end);
    list = end == std::string_view::npos ? std::string_view() : list.substr(end + 1);
    const size_t at = time_first ? item.find(mark) : item.rfind(mark);
    if (at == std::string_view::npos) continue;
    const std::string time(time_first ? item.substr(0, at) : item.substr(at + 1));
    out.push_back({std::string(time_first ? item.substr(at + 1) : item.substr(0, at)), std::strtof(time.c_str(), nullptr)});
  }
  return out;
}

std::string ArtClip(const Side& s) { return "_root.ps.characters_team" + std::to_string(s.index + 1) + ".bodies_1"; }

std::string Wanted(const Side& s) {
  if (s.fighter.empty()) return {};
  return s.costume.empty() ? "char_" + s.fighter : s.costume;
}

// The game's file system has the package (the overlay of the PS Vita costumes too): never ask for one it lacks
bool PackageExists(const std::string& package) {
  auto* fs = REX_KERNEL_FS();
  return fs && fs->ResolvePath("game:\\Asset\\" + package + ".xxx") != nullptr;
}

// GConfig->GetString(section, key, value, GGameIni)
std::string IniString(const PPCContext& ctx, uint8_t* base, const std::string& section, const char* key) {
  const uint32_t config = L32(base, kGConfig);
  if (!Readable(config, 4)) return {};
  Caller call(ctx, base);
  const uint32_t value = call.Alloc(16);  // the FString (the game allocates its text; a few bytes left behind)
  if (!call.Virtual(config, 7, {call.Ascii(section), call.Ascii(key), value, kGGameIni})) return {};
  return FStringAt(base, value);
}

// The fighter's mesh as its setup finds it (sub_823CC2E8): the costume's own Meshes.<fighter>, else the look the
// ini names ([<costume>] SourceAsset, MeshName)
// A package (an object with no outer) of that name that a collection let go and its purge has not destroyed yet
bool PackageDying(const uint8_t* base, const std::string& package) {
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (o && !L32(base, o + 36) && EqualNoCase(ObjName(base, o), package)) return Dying(base, o);
  }
  return false;
}

uint32_t MeshFor(const PPCContext& ctx, uint8_t* base, const std::string& package) {
  const std::string own = package.substr(5);  // without "char_"
  if (const uint32_t m = FindObject(base, package + ".Meshes." + own, "SkeletalMesh")) return m;
  const std::string source = IniString(ctx, base, package, "SourceAsset");
  const std::string mesh = IniString(ctx, base, package, "MeshName");
  if (!source.empty() && !mesh.empty())
    if (const uint32_t m = FindObject(base, source + ".Meshes." + mesh, "SkeletalMesh")) return m;
  return 0;
}

// The place: the camera at the origin looking down +X; player 1's fighter on the left facing right, player 2's mirrored
struct Place {
  float distance = 430, side = 120, height = -100, turn = 0;
};
// "a,b,c": the numbers given, in order (the rest keep theirs)
void ReadFloats(const std::string& text, std::initializer_list<float*> out) {
  const char* at = text.c_str();
  for (float* f : out) {
    char* end = nullptr;
    const float v = std::strtof(at, &end);
    if (end == at) return;
    *f = v;
    at = *end == ',' ? end + 1 : end;
  }
}

Place PlaceNow() {
  Place p;
  ReadFloats(REXCVAR_GET(mk9_select_preview_place), {&p.distance, &p.side, &p.height, &p.turn});
  return p;
}
void Where(const Side& s, float out[3], int32_t rot[3]) {
  const Place p = PlaceNow();
  out[0] = p.distance;
  out[1] = s.index == 0 ? -p.side : p.side;
  out[2] = p.height;
  // player 2's is player 1's mirrored across the middle (its component's Scale3D Y -1, as a fighting game turns its
  // right side): the same yaw, mirrored
  rot[0] = 0;
  rot[1] = s.index == 0 ? int32_t(32768 + p.turn) : int32_t(32768 - p.turn);
  rot[2] = 0;
}

uint32_t SpawnActor(const PPCContext& ctx, uint8_t* base, uint32_t cls, const float loc[3], const int32_t rot[3]) {
  if (!cls) return 0;
  Caller call(ctx, base);
  const uint32_t vloc = call.Alloc(16), vrot = call.Alloc(16);
  for (int k = 0; k < 3; ++k) {
    call.StoreFloat(vloc + 4 * k, loc[k]);
    call.Store32(vrot + 4 * k, uint32_t(rot[k]));
  }
  // (world, class, FName None, &location, &rotation, template, bNoCollisionFail, bRemoteOwned; owner, instigator,
  // bNoFail on the stack)
  return call.Call(kSpawnActor, {L32(base, kGWorld), cls, 0, vloc, vrot, 0, 1, 0}, {}, {{84, 0}, {92, 0}, {100, 0}});
}

void DestroyActor(const PPCContext& ctx, uint8_t* base, uint32_t actor) {
  if (!IsObject(base, actor)) return;
  Caller call(ctx, base);
  call.Call(kDestroyActor, {L32(base, kGWorld), actor, 0, 1});
}

void Reattach(const PPCContext& ctx, uint8_t* base, uint32_t actor, uint32_t component) {
  {
    Caller call(ctx, base);
    call.Call(kDetachComponent, {actor, component});
  }
  Caller call(ctx, base);
  call.Call(kAttachComponent, {actor, component});
}

// The component's transform updated after its Rotation changed, as UPrimitiveComponent::execSetRotation does
constexpr uint32_t kUpdateTransform = 0x828CB1E0;

// The fighter turned by its turn_now from the stance's facing, toward the rival (player 2's mirrored: the sign flipped)
void ApplyTurn(const PPCContext& ctx, uint8_t* base, Side& s) {
  if (!s.component) return;
  float at[3];
  int32_t rot[3];
  Where(s, at, rot);
  // (a yaw that grows turns player 1's away from the middle: toward the rival is less)
  S32(base, s.component + 0x210, uint32_t(rot[1] + (s.index == 0 ? -s.turn_now : s.turn_now)));  // Rotation.Yaw
  Caller call(ctx, base);
  call.Call(kUpdateTransform, {s.component});
}

uint32_t g_light_class = 0;

// A light of ours: MK9's lights reach the fighters only with AffectCharacters (LightComponent +0x10C, 0x40000000)
uint32_t SpawnLight(const PPCContext& ctx, uint8_t* base, const float at[3], uint32_t argb, float brightness,
                    float radius) {
  if (!g_light_class) g_light_class = ClassObject(base, "PointLightMovableSpawnable");
  const int32_t rot[3] = {0, 0, 0};
  const uint32_t light = SpawnActor(ctx, base, g_light_class, at, rot);
  const uint32_t lc = light ? L32(base, light + 0x1D0) : 0;  // its PointLightComponent
  if (!IsObject(base, lc)) return light;
  S32(base, lc + 0x10C, L32(base, lc + 0x10C) | 0x40000000u | 0x20000000u);  // AffectCharacters, AffectScene
  SF(base, lc + 0x100, brightness);
  S32(base, lc + 0x104, argb);  // FColor, a word 0xAARRGGBB
  SF(base, lc + 0x194, radius);
  Reattach(ctx, base, light, lc);
  return light;
}

void SpawnLights(const PPCContext& ctx, uint8_t* base, Side& s) {
  if (s.lights[0]) return;
  float key = 1.5f, rim = 3.0f, fill = 0.6f;
  ReadFloats(REXCVAR_GET(mk9_select_preview_lights), {&key, &rim, &fill});
  float at[3];
  int32_t rot[3];
  Where(s, at, rot);
  // the key: toward the camera, high, a little toward the middle (wide: the face and the front); the rim: behind and
  // outside, high, red-orange (the edge against the black); the fill: toward the camera, low, outside, neutral
  const float k[3] = {at[0] - 260, at[1] * 0.4f, at[2] + 240};
  const float r[3] = {at[0] + 110, at[1] * 1.45f, at[2] + 170};
  const float f[3] = {at[0] - 200, at[1] * 1.9f, at[2] + 60};
  s.lights[0] = SpawnLight(ctx, base, k, 0xFFFFF6EEu, key, 900.0f);
  s.lights[1] = SpawnLight(ctx, base, r, 0xFFFF5A28u, rim, 350.0f);
  s.lights[2] = SpawnLight(ctx, base, f, 0xFFC8CCD8u, fill, 700.0f);
}

void DestroyLights(const PPCContext& ctx, uint8_t* base, Side& s) {
  for (uint32_t& l : s.lights) {
    DestroyActor(ctx, base, l);
    l = 0;
  }
}

void PlayAnim(const PPCContext& ctx, uint8_t* base, Side& s, uint32_t anim, float length, float blend, uint32_t flags) {
  if (!s.object || !anim) return;
  Caller call(ctx, base);
  call.Virtual(s.object, kSlotPlayAnim, {anim, 0, 0, 0, flags}, {0.0, double(blend), 0.0});
  s.anim_end = g_frame + uint64_t(std::max(length, 0.5f) * 60.0f);
}

void ShowArt(const PPCContext& ctx, uint8_t* base, Side& s, bool show) {
  if (s.art_hidden == !show) return;
  SetVisible(ctx, base, ArtClip(s), show);
  s.art_hidden = !show;
}

// UObject's flags (+8, a 64-bit word: its low half at +12): RF_RootSet keeps an object (and what it points at, its
// outers too) from a collection
constexpr uint32_t kRootSet = 0x4000;

void Root(uint8_t* base, Side& s, uint32_t o) {
  // (already a root: not ours to let go; dying: going whatever the root set says)
  if (!IsObject(base, o) || Dying(base, o) || (L32(base, o + 12) & kRootSet)) return;
  S32(base, o + 12, L32(base, o + 12) | kRootSet);
  s.rooted.push_back(o);
}

void Unroot(uint8_t* base, Side& s) {
  for (uint32_t o : s.rooted)
    if (IsObject(base, o)) S32(base, o + 12, L32(base, o + 12) & ~kRootSet);
  s.rooted.clear();
}

uint32_t g_deleting = 0;  // the MK object we are deleting (its destructor's hook lets it go)
uint32_t g_hips_name[2] = {0, 0};  // the hips bone's FName (index, number): the mesh's second bone
constexpr int kTauntIndex = -1;  // Side::perf for the fighter's own taunt (kTaunt below)

// The shown fighter goes (its MK object deletes its actor); with art, the art comes back
void Drop(const PPCContext& ctx, uint8_t* base, Side& s, bool art) {
  if (s.object) {
    g_deleting = s.object;
    Caller call(ctx, base);
    call.Virtual(s.object, kSlotDelete, {1});
    g_deleting = 0;
    g_gc_due = true;
  }
  s.object = s.actor = s.component = 0;
  s.stance = s.taunt = 0;
  s.taunting = false;
  s.perf = kTauntIndex;
  s.last_perf = -2;
  s.perf_loading = s.perf_ready = false;
  s.perf_body.clear();
  s.due.clear();
  s.idles.clear();
  s.perform_end = s.pick_wait = 0;
  s.turn_now = s.turn_to = 0;
  Unroot(base, s);
  if (art) ShowArt(ctx, base, s, true);
}

// A costume made of another's look (the retro ones: the mesh is the look package's) wears its own package's material
// instances, the look's with the costume's colors ("<costume>.Materials.<the look's instance's name>"): the fight makes
// its fighter's out of the look's and the costume's colors (the fighter script's native 82497960, sub_823D0B38)
void DressInCostume(const PPCContext& ctx, uint8_t* base, Side& s, uint32_t mesh) {
  if (EqualNoCase(ObjName(base, Outermost(base, mesh)), s.package)) return;  // its own mesh, its own materials
  const uint32_t materials = L32(base, mesh + 0x60), count = L32(base, mesh + 0x64);  // SkeletalMesh.Materials
  for (uint32_t i = 0; i < count && i < 24; ++i) {
    const uint32_t look = L32(base, materials + 4 * i);
    if (!IsObject(base, look)) continue;
    const uint32_t own = FindObject(base, s.package + ".Materials." + ObjName(base, look), L32(base, look + 48));
    if (!own || own == look) continue;
    Caller call(ctx, base);
    call.Virtual(s.component, kSlotSetMaterial, {i, own});
  }
}

// mk9-lauta (2026-11-24): the voices' language, the game's (the user_language it started with: XGetLanguage's 5, 4, 3,
// 6 have the dubbed lines, _spa, _fra, _ger, _ita; the others English): what is said in the select is said in it. The
// fighters' fight voices and the intros' lines exist in English only: in another language they are left out (a
// player asked: "las voces se ponen en inglés cuando deberían estar según lo tengas configurado")
std::string g_lang_suffix;
bool g_lang_known = false;
const std::string& LangSuffix() {
  if (!g_lang_known) {
    g_lang_known = true;
    const std::string lang = rex::cvar::GetFlagByName("user_language");
    g_lang_suffix = lang == "5" ? "_spa" : lang == "4" ? "_fra" : lang == "3" ? "_ger" : lang == "6" ? "_ita" : "";
  }
  return g_lang_suffix;
}
bool EnglishVoices() { return LangSuffix().empty(); }

// Its voice for the pick: an event of its own sound packages (SND_SFX_<fighter>, SND_VO_<fighter>), the first kind
// it has of: its taunt's ("taunt", "taunt01", ...), its victory line, its laugh (every fighter has "vo_laugh"); in
// another language than English its laugh only (the others are said in English)
uint32_t FindVoice(const uint8_t* base, const std::string& fighter) {
  uint32_t best[3] = {0, 0, 0};
  int found[3] = {0, 0, 0};
  const uint32_t cls = ClassObject(base, "FmodEvent");
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o || L32(base, o + 48) != cls) continue;
    const std::string top = ObjName(base, Outermost(base, o));
    if (!EqualNoCase(top, "SND_SFX_" + fighter) && !EqualNoCase(top, "SND_VO_" + fighter)) continue;
    if (Dying(base, o)) continue;
    const std::string name = Lower(ObjName(base, o));
    int kind = name.rfind("taunt", 0) == 0 ? 0 : name == "vo_victory" || name == "vo_vict_laugh" ? 1
               : name == "vo_laugh" ? 2 : -1;
    if (!EnglishVoices()) kind = name == "vo_laugh" || name == "vo_vict_laugh" ? 2 : -1;
    if (kind < 0) continue;
    // one of the kind's, by a draw of our own (not the game's generator)
    ++found[kind];
    if (((g_frame * 2654435761u + i) >> 7) % uint32_t(found[kind]) == 0) best[kind] = o;
  }
  return best[0] ? best[0] : best[1] ? best[1] : best[2];
}

// MK's sound system plays an event, as the select script plays the announcer's names (natives 82470200, 824A4F70):
// the system object ([0x833848DC], sub_82A30268) and its slot 9 with a sound reference {the event's package's name, the
// event's FName, its cache, -1, the name again} (sub_824BFDA8 finds the event by them), a group 0 (the global sounds:
// no fighter's slot), a delay in frames and the script's other values. Its handle (0: none).
constexpr uint32_t kSoundSystem = 0x82A30268;
constexpr uint32_t kSlotPlayBankEvent = 9;

uint32_t PlayEvent(const PPCContext& ctx, uint8_t* base, uint32_t event) {
  if (!IsObject(base, event)) return 0;
  uint32_t system = 0;
  {
    Caller call(ctx, base);
    system = call.Call(kSoundSystem, {});
  }
  if (!system) return 0;
  const std::string package = ObjName(base, Outermost(base, event));
  const uint32_t play = L32(base, L32(base, system) + 4 * kSlotPlayBankEvent);
  Caller call(ctx, base);
  const uint32_t name = call.Ascii(package);
  const uint32_t ref = call.Alloc(32);
  call.Store32(ref, name);
  call.Store32(ref + 4, L32(base, event + 40));  // the event's FName: its index, its number
  call.Store32(ref + 8, L32(base, event + 44));
  call.Store32(ref + 12, 0);
  call.Store32(ref + 16, 0xFFFFFFFFu);
  call.Store32(ref + 20, name);
  return call.Call(play, {system, ref, 0, 0, 0, 0, 0, 0xFFFFFFFFu}, {}, {{84, 1}});
}

void PlayVoice(const PPCContext& ctx, uint8_t* base, Side& s) {
  if (!s.voice) return;
  const uint32_t handle = PlayEvent(ctx, base, s.voice);
  REXLOG_INFO("[select3d] side {}: its voice {}.{}: handle {:X}", s.index + 1, ObjName(base, Outermost(base, s.voice)),
              ObjName(base, s.voice), handle);
}

// An FmodEvent by its package ("SND_SFX_Scorpion": one of its outers, the outermost or one inside it: a cutscene's
// dub, SND_MOV_MK006_spa, holds its lines in SND_MOV_MK006) and its name ("taunt02"), any group's
uint32_t FindEvent(const uint8_t* base, std::string_view package, std::string_view event) {
  const uint32_t cls = ClassObject(base, "FmodEvent");
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o || L32(base, o + 48) != cls || !EqualNoCase(ObjName(base, o), event)) continue;
    for (uint32_t outer = L32(base, o + 36), d = 0; outer && d < 8; outer = L32(base, outer + 36), ++d)
      if (EqualNoCase(ObjName(base, outer), package) && Alive(base, o, "the sound")) return o;
  }
  return 0;
}

// The fighter's own voice sound ("vo_spatk": a cinematic's character sound), of its SND_VO_<fighter> package
uint32_t FindOwnVoice(const uint8_t* base, const std::string& fighter, std::string_view event) {
  const uint32_t cls = ClassObject(base, "FmodEvent");
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o || L32(base, o + 48) != cls || !EqualNoCase(ObjName(base, o), event)) continue;
    const std::string top = Lower(ObjName(base, Outermost(base, o)));
    if (top.rfind("snd_vo_", 0) == 0 && top.find(fighter) != std::string::npos && Alive(base, o, "the sound")) return o;
  }
  return 0;
}

// An animation by its sequence name, any package's (the cinematics' have names of their own)
uint32_t FindSequence(const uint8_t* base, std::string_view sequence) {
  const uint32_t cls = ClassObject(base, "AnimSequence");
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (o && L32(base, o + 48) == cls && EqualNoCase(NameText(base, L32(base, o + 0x38)), sequence) &&
        Alive(base, o, "the animation"))
      return o;
  }
  return 0;
}

// A package asked for as the fight asks for its own (UObject::LoadPackageAsync): asked for even when loaded (a package
// loaded is not loaded again: UE3 finds it), its objects whole once no package is loading (GObjAsyncPackages.Num 0).
// One the game's file system lacks is never asked for (a missing package ends the game).
bool LoadPackage(const PPCContext& ctx, uint8_t* base, const std::string& package) {
  if (!PackageExists(package)) return false;
  Caller call(ctx, base);
  const uint32_t name = call.Ascii(package);
  const uint32_t fs = call.Alloc(16);
  call.Store32(fs, name);
  call.Store32(fs + 4, uint32_t(package.size() + 1));
  call.Store32(fs + 8, uint32_t(package.size() + 1));
  call.Call(kLoadPackageAsync, {fs, 0, 0, 0});
  return true;
}

// The lines' packages: the intros' (SND_VO_CharIntro, English only) and the Story transitions' in the game's language
// (SND_VO_StoryTrans_spa, _fra, _ger, _ita for XGetLanguage's 5, 4, 3, 6, the user_language the game started with;
// English for the others), loaded on the select screen and held in the root set while it is shown
std::string g_story_package;
const std::string& StoryPackage() {
  if (g_story_package.empty()) g_story_package = "SND_VO_StoryTrans" + LangSuffix();
  return g_story_package;
}
constexpr char kIntroLines[] = "SND_VO_CharIntro";

bool g_lines_asked = false, g_lines_held = false;
std::vector<uint32_t> g_lines_rooted;

void HoldLines(const PPCContext& ctx, uint8_t* base) {
  if (!g_lines_asked) {
    g_lines_asked = true;
    if (!PackageExists(StoryPackage())) g_story_package = "SND_VO_StoryTrans";  // game files without that dub
    if (EnglishVoices()) LoadPackage(ctx, base, kIntroLines);  // (the intros' lines: English only)
    LoadPackage(ctx, base, StoryPackage());
    return;
  }
  if (g_lines_held || L32(base, kAsyncPackagesNum) != 0) return;
  g_lines_held = true;
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o) continue;
    const std::string top = ObjName(base, Outermost(base, o));
    if (!EqualNoCase(top, kIntroLines) && !EqualNoCase(top, StoryPackage())) continue;
    if ((L32(base, o + 12) & kRootSet) || Dying(base, o)) continue;
    S32(base, o + 12, L32(base, o + 12) | kRootSet);
    g_lines_rooted.push_back(o);
  }
  REXLOG_INFO("[select3d] the lines' packages held ({}{}: {} objects)", StoryPackage(),
              EnglishVoices() ? std::string(" and ") + kIntroLines : std::string(), g_lines_rooted.size());
}

void ReleaseLines(uint8_t* base) {
  for (uint32_t o : g_lines_rooted)
    if (IsObject(base, o)) S32(base, o + 12, L32(base, o + 12) & ~kRootSet);
  g_lines_rooted.clear();
  g_lines_asked = g_lines_held = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// The performances

constexpr int kTaunt = kTauntIndex;  // its EndOfRound_Taunt and a line of its voice (FindVoice)
constexpr int kNothing = -3;  // (tests) none
constexpr uint32_t kLoadRetries = 6;  // a package still being destroyed: asked for again this many times at most
constexpr uint64_t kPickWaitFrames = 120;  // a pick waits this long for its cinematic's package, then the taunt
constexpr uint64_t kIdleEvery = 420;       // another pose at most every 7 s (and up to 6 s more), the stance between
constexpr float kStoryTurn = 45.0f, kIntroTurn = 135.0f;  // degrees toward the rival (mk9_select_preview_turn)
constexpr int32_t kTurnStep = 65536 / 72;  // 5 degrees a frame

std::mt19937& Rng() {
  static std::mt19937 rng(std::random_device{}());
  return rng;
}

// The next one for the side's fighter: one of its own at random (not the last one again), or its taunt; its package
// then loads (Step) while the fighter is shown. mk9_select_preview_perform picks it (tests).
void Choose(Side& s) {
  s.perf = kTaunt;
  s.perf_loading = s.perf_ready = false;
  s.perf_body.clear();
  std::vector<int> pool;
  for (int i = 0; i < int(std::size(kPerformances)); ++i)
    if (s.fighter == kPerformances[i].fighter) pool.push_back(i);
  if (s.taunt) pool.push_back(kTaunt);
  const std::string& forced = REXCVAR_GET(mk9_select_preview_perform);
  if (!forced.empty()) {
    if (forced == "none") s.perf = kNothing;
    for (int i : pool)
      if (i >= 0 && EqualNoCase(kPerformances[i].package, forced)) s.perf = i;
  } else {
    if (pool.size() > 1) pool.erase(std::remove(pool.begin(), pool.end(), s.last_perf), pool.end());
    if (!pool.empty()) s.perf = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(Rng())];
  }
  s.perf_ready = s.perf < 0;
}

// The package a performance loads: its cinematic's, or a cutscene's line's in the game's language (SND_MOV_MK<nnn>_spa;
// the English one where the game's files lack that dub)
std::string PerfPackage(const PerfDef& d) {
  if (d.kind != 'L' || EnglishVoices()) return d.package;
  const std::string dubbed = d.package + LangSuffix();
  return PackageExists(dubbed) ? dubbed : std::string(d.package);
}

// Its animations, found once its package is whole, held in the root set with the fighter's (a cutscene's line: the
// fighter's EndOfRound_Taunt, and its package's objects, the line's sound with them)
void Resolve(uint8_t* base, Side& s) {
  const PerfDef& d = kPerformances[s.perf];
  s.perf_loading = false;
  if (d.kind == 'L') {
    const std::string file = PerfPackage(d);
    for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
      const uint32_t o = ObjectAt(base, i);
      if (!o) continue;
      const std::string top = ObjName(base, Outermost(base, o));
      if (EqualNoCase(top, d.package) || EqualNoCase(top, file)) Root(base, s, o);
    }
    if (s.taunt || s.stance) s.perf_body.push_back({s.taunt ? s.taunt : s.stance, 0.0f});
    s.perf_ready = true;
    return;
  }
  for (const Timed& t : ParseTimed(d.body, '|', '@', false)) {
    const uint32_t anim = FindSequence(base, t.name);
    if (!anim) continue;
    Root(base, s, anim);
    s.perf_body.push_back({anim, t.at});
  }
  if (s.perf_body.empty()) {
    REXLOG_WARN("[select3d] side {}: {} loaded but its animation {} not found: its taunt instead", s.index + 1,
                d.package, d.body);
    s.perf = kTaunt;
  }
  s.perf_ready = true;
}

// A performance's animation on the fighter, as MK's player plays one (slot 29: its slot 34 on the "FullSkeleton"
// subskeleton), from its start
void PlayOn(const PPCContext& ctx, uint8_t* base, Side& s, uint32_t anim) {
  if (!s.object || !anim) return;
  Caller call(ctx, base);
  call.Virtual(s.object, kSlotPlayAnim, {anim, 0, 0, 0, kOnceFlags}, {0.0, double(kTauntBlend), 0.0});
}

// A cinematic's sound: "PACKAGE.event" (snd_vo_storytrans: the Story lines' package in the game's language) or
// "@vo_x" (the fighter's own voice sound)
void Cue(const PPCContext& ctx, uint8_t* base, Side& s, const std::string& sound) {
  uint32_t event = 0;
  bool line = false;
  if (!sound.empty() && sound[0] == '@') {
    event = FindOwnVoice(base, s.fighter, std::string_view(sound).substr(1));
  } else {
    const size_t dot = sound.find('.');
    if (dot == std::string::npos) return;
    std::string package = sound.substr(0, dot);
    if (EqualNoCase(package, kIntroLines) && !EnglishVoices()) return;  // (said in English only)
    line = EqualNoCase(package, "snd_vo_storytrans") || EqualNoCase(package, kIntroLines) ||
           StartsNoCase(package, "SND_MOV_");
    if (EqualNoCase(package, "snd_vo_storytrans")) package = StoryPackage();
    event = FindEvent(base, package, std::string_view(sound).substr(dot + 1));
  }
  if (!event) {
    if (line) REXLOG_WARN("[select3d] side {}: its line {} not loaded", s.index + 1, sound);
    return;
  }
  const uint32_t handle = PlayEvent(ctx, base, event);
  if (line)
    REXLOG_INFO("[select3d] side {}: its line {}.{}: handle {:X}", s.index + 1, ObjName(base, Outermost(base, event)),
                ObjName(base, event), handle);
}

// The pick's performance: its body's animations, its face's, its sounds, each at its time from the body's start (what
// comes before it at once); the stance again at its end
void Perform(const PPCContext& ctx, uint8_t* base, Side& s) {
  s.due.clear();
  s.last_perf = s.perf;
  if (s.perf == kNothing) return;
  if (s.perf < 0 || s.perf_body.empty()) {
    if (!s.taunt) return;
    REXLOG_INFO("[select3d] side {}: picked: its taunt", s.index + 1);
    PlayAnim(ctx, base, s, s.taunt, s.taunt_length, kTauntBlend, kOnceFlags);
    s.voice_frame = g_frame + 30;  // after the announcer's first word
    s.taunting = true;
    s.perform_end = s.anim_end;
    return;
  }
  const PerfDef& d = kPerformances[s.perf];
  // toward the rival (Animate turns it a little each frame): a Story line is played to the cinematic's camera (the
  // rival beside it), an intro as in a fight (in profile, toward the other side)
  float story = kStoryTurn, intro = kIntroTurn;
  ReadFloats(REXCVAR_GET(mk9_select_preview_turn), {&story, &intro});
  s.turn_to = d.kind == 'L' ? 0 : int32_t((d.kind == 'S' ? story : intro) * 65536.0f / 360.0f);
  const float t0 = s.perf_body[0].second;
  const auto when = [&](float t) { return g_frame + uint64_t(std::max(0.0f, t - t0) * 60.0f + 0.5f); };
  float length = 0;
  for (size_t i = 0; i < s.perf_body.size(); ++i) {
    const auto& [anim, at] = s.perf_body[i];
    if (i == 0)
      PlayOn(ctx, base, s, anim);
    else
      s.due.push_back({when(at), anim, {}});
    length = std::max(length, at - t0 + AnimLength(base, anim));
  }
  size_t sounds = 0;
  for (const Timed& c : ParseTimed(d.cues, ';', ':', true)) {
    s.due.push_back({when(c.at), 0, c.name});
    ++sounds;
  }
  s.perform_end = g_frame + uint64_t(std::max(length, 1.0f) * 60.0f);
  s.anim_end = s.perform_end;
  s.taunting = true;
  REXLOG_INFO("[select3d] side {}: picked: {} {} ({:.1f} s, {} animations, {} sounds)", s.index + 1,
              d.kind == 'S' ? "the Story transition" : d.kind == 'I' ? "its intro" : "a cutscene's line",
              PerfPackage(d), length, s.perf_body.size(), sounds);
}

// The fighters' mesh's subskeletons (MK's mesh data, the component's +656: +168 its entries, 40 bytes each with the
// name's FName first and a 256-bit bone mask, +172 their count, +176 the room; sub_82903458 finds one by name), named
// in the log once. FullSkeleton, ArmCloth, WaistCloth, DamageCloth: no face's. The cinematics play their face tracks
// on the bones named F_* (their "Face Only" subtrack's prefix), a mask the meshes do not have: the performances play
// the body's animations only (their lines' lips do not move; the mesh's bone names are its +0x90, a TArray<FName>).
void LogSubskeletons(const uint8_t* base, uint32_t component) {
  static bool logged = false;
  const uint32_t data = L32(base, component + 656);
  if (logged || !Readable(data, 176)) return;
  logged = true;
  const uint32_t entries = L32(base, data + 168), count = L32(base, data + 172);
  std::string names;
  for (uint32_t i = 0; i < count && i < 16 && Readable(entries + 40 * i, 8); ++i)
    names += (i ? ", " : "") + NameText(base, L32(base, entries + 40 * i));
  REXLOG_INFO("[select3d] the fighters' subskeletons: {}", names);
}

bool Show(const PPCContext& ctx, uint8_t* base, Side& s, uint32_t mesh) {
  // its stance first: without one it would stand in its bind pose (the art stays)
  const uint32_t stance = FindAnim(base, s.package, "Stance");
  if (!stance || AnimLength(base, stance) <= 0) {
    REXLOG_WARN("[select3d] side {}: {} has no stance animation: the art stays", s.index + 1, s.package);
    return false;
  }
  uint32_t object = 0;
  {
    Caller call(ctx, base);
    object = call.Call(kMKAlloc, {464});
  }
  if (!object) return false;
  {
    Caller call(ctx, base);
    call.Call(kSkelActorCtor, {object, mesh, 0x7FFFF, 0, 0, 0});
  }
  s.object = object;
  s.actor = L32(base, object + 8);
  s.component = L32(base, object + 268);
  if (!IsObject(base, s.actor) || !IsObject(base, s.component)) {
    REXLOG_WARN("[select3d] {}: the skeletal actor came out without its actor or component", s.package);
    Drop(ctx, base, s, true);
    return false;
  }
  DressInCostume(ctx, base, s, mesh);
  // its place (its component's, the actor at the origin), MK's character shader, no light environment (as a fighter)
  float at[3];
  int32_t rot[3];
  Where(s, at, rot);
  const uint32_t c = s.component;
  for (int k = 0; k < 3; ++k) {
    SF(base, c + 0x200 + 4 * k, at[k]);              // Translation
    S32(base, c + 0x20C + 4 * k, uint32_t(rot[k]));  // Rotation
    SF(base, c + 0x21C + 4 * k, k == 1 && s.index == 1 ? -1.0f : 1.0f);  // Scale3D
  }
  S32(base, c + 0x114, 0);  // LightEnvironment
  // bUseMKDynamicShader, bUseLightControlVolumes, bReceiveDynamicGobo, bUseForegroundAAEdgeDetection
  S32(base, c + 0x120, L32(base, c + 0x120) | 0x00100000u | 0x400u | 0x4u | 0x2u);
  base[c + 0x130] = 4;  // MKOpaqueDetailCategory, as a fighter's
  Reattach(ctx, base, s.actor, c);
  SpawnLights(ctx, base, s);
  // its stance (and its taunt for the pick)
  s.stance = FindAnim(base, s.package, "Stance");
  s.stance_length = s.stance ? AnimLength(base, s.stance) : 0;
  s.taunt = FindAnim(base, s.package, "EndOfRound_Taunt");
  s.taunt_length = s.taunt ? AnimLength(base, s.taunt) : 0;
  s.voice = FindVoice(base, s.fighter);
  s.voice_frame = 0;
  for (uint32_t o : {mesh, s.stance, s.taunt, s.voice}) Root(base, s, o);
  s.idles.clear();
  for (const char* pose : {"Stance_Alt01", "Stance_Alt02", "Stance_Alt03", "Stance_Alt04", "Stance_Alt05", "Idle1"}) {
    const uint32_t anim = FindAnim(base, s.package, pose);
    const float length = anim ? AnimLength(base, anim) : 0;
    if (length <= 0) continue;
    Root(base, s, anim);
    s.idles.push_back({anim, length});
  }
  s.idle_next = g_frame + kIdleEvery + g_frame % 180;
  PlayAnim(ctx, base, s, s.stance, s.stance_length, kStanceBlend, kLoopFlags);
  s.taunting = false;
  Choose(s);
  LogSubskeletons(base, c);
  if (!g_hips_name[0] && Readable(L32(base, mesh + 0x90), 16) && L32(base, mesh + 0x94) > 1) {
    g_hips_name[0] = L32(base, L32(base, mesh + 0x90) + 8);
    g_hips_name[1] = L32(base, L32(base, mesh + 0x90) + 12);
  }
  ShowArt(ctx, base, s, false);
  REXLOG_INFO("[select3d] side {}: {} shown after {} frames (mesh '{}', stance {:.2f} s, taunt {:.2f} s; {} objects)",
              s.index + 1, s.package, g_frame - s.load_frame, Path(base, mesh), s.stance_length, s.taunt_length,
              ObjectCount(base));
  return true;
}

// USkeletalMeshComponent::GetBoneLocation(out, component, the bone's FName, space 0: the world's), as its native
// (execGetBoneLocation) calls it
constexpr uint32_t kGetBoneLocation = 0x82924108;

// The fighter's hips (the mesh's bone names, a TArray<FName> at its +0x90: Reference, Hips, Spine, ...): where they are
bool BoneAt(const PPCContext& ctx, uint8_t* base, const Side& s, const uint32_t name[2], float out[3]) {
  if (!s.component) return false;
  Caller call(ctx, base);
  const uint32_t v = call.Alloc(16);
  const uint64_t bone = (uint64_t(name[0]) << 32) | name[1];
  const uint32_t r = call.Call(kGetBoneLocation, {v, s.component, bone, 0});
  if (!Readable(r, 12)) return false;
  for (int k = 0; k < 3; ++k) out[k] = LF(base, r + 4 * k);
  return std::isfinite(out[0]) && std::isfinite(out[1]);
}

// (tests) mk9_select_preview_test_survey: each performance's body animation on player 1's fighter, its hips along the
// floor from where they are in the stance (the fighter's place): at its first frames (an intro comes in from out of
// sight), the farthest, and at its end (one that ends away would jump back to the stance), in the log
struct Survey {
  int next = 0;
  bool started = false, asked = false, playing = false;
  uint64_t until = 0;
  float home[2] = {0, 0}, first = 0, farthest = 0, farthest_at = 0, end = 0;
  uint64_t began = 0;
} g_survey;

void SurveyStep(const PPCContext& ctx, uint8_t* base, Side& s) {
  if (s.index != 0 || !s.object || !g_hips_name[0]) return;
  Survey& v = g_survey;
  if (!v.started) {
    v.started = true;
    v.next = std::max(0, int(REXCVAR_GET(mk9_select_preview_test_survey_from)));
  }
  if (v.playing) {
    float at[3];
    if (BoneAt(ctx, base, s, g_hips_name, at)) {
      const float d = std::hypot(at[0] - v.home[0], at[1] - v.home[1]);
      if (g_frame == v.began + 3) v.first = d;
      if (d > v.farthest) v.farthest = d, v.farthest_at = float(g_frame - v.began) / 60.0f;
      v.end = d;
    }
    if (g_frame < v.until) return;
    const PerfDef& d = kPerformances[v.next];
    REXLOG_INFO("[select3d] survey {} {} {}: from its place first {:.0f}, farthest {:.0f} at {:.1f} s, at the end {:.0f} "
                "({:.1f} s)",
                v.next, d.kind, d.package, v.first, v.farthest, v.farthest_at, v.end, float(v.until - v.began) / 60.0f);
    // its animations let go (the fighter's own kept), a collection asked for now and then
    for (const auto& [anim, at0] : s.perf_body) {
      if (IsObject(base, anim)) S32(base, anim + 12, L32(base, anim + 12) & ~kRootSet);
      s.rooted.erase(std::remove(s.rooted.begin(), s.rooted.end(), anim), s.rooted.end());
    }
    s.perf_body.clear();
    v.playing = v.asked = false;
    ++v.next;
    g_gc_due = true;
    PlayAnim(ctx, base, s, s.stance, s.stance_length, kStanceBlend, kLoopFlags);
    return;
  }
  if (v.next >= int(std::size(kPerformances))) return;
  if (!v.asked) {  // its package asked for (Step), its animations found (Resolve)
    v.asked = true;
    s.perf = v.next;
    s.perf_loading = s.perf_ready = false;
    s.perf_body.clear();
    return;
  }
  if (!s.perf_ready) return;
  if (s.perf_body.empty()) {
    REXLOG_INFO("[select3d] survey {} {}: not found", v.next, kPerformances[v.next].package);
    v.asked = false;
    ++v.next;
    return;
  }
  float length = 0;
  for (const auto& [anim, at0] : s.perf_body)
    length = std::max(length, at0 - s.perf_body[0].second + AnimLength(base, anim));
  float home[3];
  if (!BoneAt(ctx, base, s, g_hips_name, home)) return;  // its place: the hips in the stance
  v.home[0] = home[0], v.home[1] = home[1];
  PlayOn(ctx, base, s, s.perf_body[0].first);
  v.playing = true;
  v.began = g_frame;
  v.until = g_frame + uint64_t(std::max(length, 0.5f) * 60.0f);
  v.first = v.farthest = v.farthest_at = v.end = 0;
}

// The shown fighter's frame: its turn, what of its performance is due, its pick, the stance looped
void Animate(const PPCContext& ctx, uint8_t* base, Side& s) {
  if (s.turn_now != s.turn_to) {
    s.turn_now += std::clamp(s.turn_to - s.turn_now, -kTurnStep, kTurnStep);
    ApplyTurn(ctx, base, s);
  }
  if (s.voice_frame && g_frame >= s.voice_frame) {
    s.voice_frame = 0;
    PlayVoice(ctx, base, s);
  }
  for (size_t i = 0; i < s.due.size();) {
    if (g_frame < s.due[i].frame) {
      ++i;
      continue;
    }
    const Side::Due d = s.due[i];
    s.due.erase(s.due.begin() + ptrdiff_t(i));
    if (d.anim)
      PlayOn(ctx, base, s, d.anim);
    else
      Cue(ctx, base, s, d.sound);
  }
  if (s.picked) {
    // its cinematic's package still loading: the pick waits for it a while, then the taunt instead
    const bool waiting = s.perf >= 0 && !s.perf_ready;
    if (waiting && !s.pick_wait) s.pick_wait = g_frame;
    if (!waiting || g_frame - s.pick_wait >= kPickWaitFrames) {
      if (waiting) s.perf = kTaunt;
      s.picked = false;
      s.pick_wait = 0;
      Perform(ctx, base, s);
      return;
    }
  }
  if (g_frame < s.anim_end) return;
  // the stance again at its end (and after a performance: then the next one is chosen, for another pick); now and
  // then one of its other poses instead (not right after a performance)
  if (s.taunting) {
    s.taunting = false;
    s.turn_to = 0;
    s.idle_next = std::max(s.idle_next, g_frame + kIdleEvery);
    Choose(s);
  } else if (!s.idles.empty() && g_frame >= s.idle_next) {
    const auto& [anim, length] = s.idles[std::uniform_int_distribution<size_t>(0, s.idles.size() - 1)(Rng())];
    PlayAnim(ctx, base, s, anim, length, kStanceBlend, kOnceFlags);
    s.idle_next = s.anim_end + kIdleEvery + std::uniform_int_distribution<uint64_t>(0, 360)(Rng());
    return;
  }
  PlayAnim(ctx, base, s, s.stance, s.stance_length, kStanceBlend, kLoopFlags);
}

void Step(const PPCContext& ctx, uint8_t* base, Side& s) {
  const std::string want = Wanted(s);
  if (!EqualNoCase(want, s.package)) {
    if (g_frame - s.wish_frame < kSettleFrames || g_frame < g_load_block_until) return;
    // the one shown goes; while the next loads the side stays empty (the art comes back only for one that cannot
    // be shown: no fighter, no package, a failed load)
    Drop(ctx, base, s, false);
    s.package = want;
    s.state = Side::State::kNone;
    if (want.empty()) {
      ShowArt(ctx, base, s, true);
      return;
    }
    if (!PackageExists(want)) {
      REXLOG_INFO("[select3d] side {}: no package {} in the game's files: the art stays", s.index + 1, want);
      s.state = Side::State::kFailed;
      ShowArt(ctx, base, s, true);
      return;
    }
    ShowArt(ctx, base, s, false);
    s.state = Side::State::kLoading;
    s.load_frame = g_frame;
    s.retries = 0;
    s.retry_at = 0;
    if (const std::string f = Lower(s.fighter);
        !f.empty() && std::find(g_loaded_fighters.begin(), g_loaded_fighters.end(), f) == g_loaded_fighters.end())
      g_loaded_fighters.push_back(f);
    LoadPackage(ctx, base, want);
    return;
  }
  if (s.state == Side::State::kLoading) {
    if (s.retry_at) {  // its package was still being destroyed: asked for again now
      if (g_frame < s.retry_at) return;
      s.retry_at = 0;
      s.load_frame = g_frame;
      LoadPackage(ctx, base, s.package);
      return;
    }
    if (L32(base, kAsyncPackagesNum) != 0) {  // its objects are made over frames: whole once nothing is loading
      if (g_frame - s.load_frame > kLoadTimeout) {
        REXLOG_WARN("[select3d] side {}: {} still loading after {} s: the art stays", s.index + 1, s.package,
                    kLoadTimeout / 60);
        s.state = Side::State::kFailed;
        ShowArt(ctx, base, s, true);
      }
      return;
    }
    g_found_dying = false;
    const uint32_t mesh = MeshFor(ctx, base, s.package);
    // mk9-lauta (2026-11-24): a collection let its package go and the purge is not through (LoadPackageAsync took it
    // as loaded): asked for again in half a second, a few times
    if (!mesh && (g_found_dying || PackageDying(base, s.package)) && s.retries < kLoadRetries) {
      ++s.retries;
      s.retry_at = g_frame + 30;
      REXLOG_INFO("[select3d] side {}: {} still being destroyed by a collection: asked for again", s.index + 1,
                  s.package);
      return;
    }
    if (!mesh) {
      REXLOG_WARN("[select3d] side {}: {} loaded but no mesh found: the art stays", s.index + 1, s.package);
      s.state = Side::State::kFailed;
      ShowArt(ctx, base, s, true);
      return;
    }
    if (Show(ctx, base, s, mesh)) {
      s.state = Side::State::kShown;
    } else {
      s.state = Side::State::kFailed;
      ShowArt(ctx, base, s, true);
    }
    return;
  }
  if (s.state != Side::State::kShown) return;
  if (REXCVAR_GET(mk9_select_preview_test_survey) && s.index == 0) {
    if (s.perf >= 0 && !s.perf_ready) {
      if (!s.perf_loading && g_frame >= g_load_block_until) {
        s.perf_loading = LoadPackage(ctx, base, PerfPackage(kPerformances[s.perf]));
        if (!s.perf_loading) s.perf_ready = true;  // no package: "not found"
      } else if (s.perf_loading && L32(base, kAsyncPackagesNum) == 0) {
        Resolve(base, s);
      }
    }
    SurveyStep(ctx, base, s);
    return;
  }
  // its performance's cinematic: asked for (not while a collection asked for is under way), then its animations
  if (s.perf >= 0 && !s.perf_ready) {
    if (!s.perf_loading && g_frame >= g_load_block_until) {
      s.perf_loading = LoadPackage(ctx, base, PerfPackage(kPerformances[s.perf]));
      if (!s.perf_loading) {
        REXLOG_WARN("[select3d] side {}: no package {}: its taunt instead", s.index + 1,
                    PerfPackage(kPerformances[s.perf]));
        s.perf = kTaunt;
        s.perf_ready = true;
      }
    } else if (s.perf_loading && L32(base, kAsyncPackagesNum) == 0) {
      Resolve(base, s);
    }
  }
  Animate(ctx, base, s);
}

// a side with its art hidden for a fighter of ours (shown or loading): the movie's background goes with it
bool AnyActive() {
  for (const Side& s : g_sides)
    if (s.object || s.state == Side::State::kLoading) return true;
  return false;
}

// Everything of ours goes: leaving the select screen (movie: false, the movie is gone) or the option turned off
void TearDown(const PPCContext& ctx, uint8_t* base, bool movie) {
  bool had = false;
  for (Side& s : g_sides) {
    had = had || s.object || s.lights[0];
    Drop(ctx, base, s, false);
    if (movie) ShowArt(ctx, base, s, true);
    s.art_hidden = false;
    DestroyLights(ctx, base, s);
    s.package.clear();
    s.state = Side::State::kNone;
  }
  if (movie) HideBackground(ctx, base, false);
  g_background_hidden = false;
  ReleaseLines(base);
  if (had) {
    REXLOG_INFO("[select3d] the previews gone ({})", movie ? "the option is off" : "the select screen ended");
    g_gc_due = true;
  }
}

// A garbage collection asked for (the game's own way: the state object's +64, as the Krypt's code asks) a while
// after previews went, so the packages nobody shows any more leave memory
// mk9-lauta (2026-11-24): the collection the preview asks for takes only what the preview loaded and shows no more.
// The game itself collects on the select screen only as the cursor reaches the DLC fighters (its DLC manager, see
// sub_826DD740's hook), with nothing of its own loading; the preview's collection of everything took, besides its
// fighters, what the game had there and keeps (the menus' movies, ui_s_mainmenu, ui_s_options, ui_s_dlcBillboard) and
// what every fighter shares, the effects (FX_Blood, FX_Impact, FX_Global, FX_Character, PFX_Xray; mk9_select_preview_
// test_gc_report's lists), loaded again with the next fighter. Now, for the length of each collection of the
// preview's, everything else is held in the root set: what may go is the packages named after a fighter it showed and
// shows no more on either side (CHAR_Scorpion-<hash>, CHAR_Scorpion, Scorpion_XRAY, FX_Scorpion, SND_SFX_Scorpion,
// Anims_XRay_Scorpion_...) and the cinematics it loaded for performances (NIS_MK..., ANIM_MK..., SND_MOV_MK...), whose
// objects in use are its roots. The rest stays as the game left it, and the shared effects load once.

bool NamedAfter(const std::string& lower_package, const std::string& fighter) {
  return lower_package.rfind(fighter, 0) == 0 || lower_package.find("_" + fighter) != std::string::npos;
}
bool Collectable(const std::string& lower_package) {
  for (const char* prefix : {"nis_mk", "anim_mk", "snd_mov_mk"})
    if (lower_package.rfind(prefix, 0) == 0) return true;
  for (const std::string& f : g_gc_fighters)
    if (NamedAfter(lower_package, f)) return true;
  return false;
}

// The objects whose outermost package 'keep' says to keep, held in the root set for the length of a collection
// (returned, to be let go after); the ones already roots, and the ones dying or still loading, left as they are
template <typename Keep>
std::vector<uint32_t> HoldWhere(uint8_t* base, Keep keep) {
  std::vector<uint32_t> held;
  std::unordered_map<uint32_t, bool> by_package;
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o) continue;
    const uint32_t flags = L32(base, o + 12);
    if ((flags & kRootSet) || Dying(base, o)) continue;
    const uint32_t top = Outermost(base, o);
    auto it = by_package.find(top);
    if (it == by_package.end()) it = by_package.emplace(top, keep(Lower(ObjName(base, top)))).first;
    if (!it->second) continue;
    S32(base, o + 12, flags | kRootSet);
    held.push_back(o);
  }
  return held;
}
// Before the preview's collection: every object a collection of its may not take
std::vector<uint32_t> HoldTheRest(uint8_t* base) {
  return HoldWhere(base, [](const std::string& package) { return !Collectable(package); });
}
// Before the game's own (the DLC manager's, sub_8260C268, as the cursor reaches the DLC fighters): what the preview
// loads or shows (its fighters' packages, its performances' cinematics), which nothing else holds yet
std::vector<uint32_t> HoldThePreviews(uint8_t* base) {
  std::vector<std::string> fighters, cinematics;
  for (const Side& s : g_sides) {
    if (!s.fighter.empty() && !s.package.empty()) fighters.push_back(Lower(s.fighter));
    if (s.perf >= 0 && (s.perf_loading || s.perf_ready)) cinematics.push_back(Lower(PerfPackage(kPerformances[s.perf])));
  }
  if (fighters.empty() && cinematics.empty()) return {};
  return HoldWhere(base, [&](const std::string& package) {
    for (const std::string& f : fighters)
      if (NamedAfter(package, f)) return true;
    for (const std::string& c : cinematics)
      if (package.rfind(c, 0) == 0) return true;
    return false;
  });
}

// (tests, mk9_select_preview_test_gc_report) the objects there when a collection was asked for, and the report of what
// it took 30 frames later, by package
std::vector<std::pair<uint32_t, uint32_t>> g_gc_before;  // (object, its outermost)
std::vector<uint32_t> g_gc_before_class;                 // (its class)
std::unordered_map<uint32_t, std::string> g_gc_outer_names;
uint64_t g_gc_report_at = 0;

void GcSnapshot(const uint8_t* base) {
  g_gc_before.clear();
  g_gc_before_class.clear();
  g_gc_outer_names.clear();
  for (uint32_t i = 0, n = ObjectCount(base); i < n; ++i) {
    const uint32_t o = ObjectAt(base, i);
    if (!o) continue;
    const uint32_t top = Outermost(base, o);
    g_gc_before.emplace_back(o, top);
    g_gc_before_class.push_back(L32(base, o + 48));
    if (const uint32_t c = L32(base, o + 48); c && !g_gc_outer_names.count(c)) g_gc_outer_names[c] = ObjName(base, c);
    if (!g_gc_outer_names.count(top)) g_gc_outer_names[top] = ObjName(base, top);
  }
  g_gc_report_at = g_frame + 30;
}

void GcReport(const uint8_t* base) {
  if (!g_gc_report_at || g_frame < g_gc_report_at) return;
  g_gc_report_at = 0;
  std::unordered_map<std::string, uint32_t> gone, classes;
  uint32_t total = 0;
  for (size_t k = 0; k < g_gc_before.size(); ++k) {
    const auto& [o, top] = g_gc_before[k];
    if (IsObject(base, o) && !Dying(base, o)) continue;
    ++gone[g_gc_outer_names[top]];
    ++classes[g_gc_outer_names[g_gc_before_class[k]]];
    ++total;
  }
  {
    std::vector<std::pair<uint32_t, std::string>> by_class;
    for (const auto& [name, n] : classes) by_class.emplace_back(n, name);
    std::sort(by_class.rbegin(), by_class.rend());
    std::string text;
    for (size_t k = 0; k < by_class.size() && k < 16; ++k)
      text += (k ? ", " : "") + by_class[k].second + " " + std::to_string(by_class[k].first);
    REXLOG_INFO("[select3d] (test) by class: {}", text);
  }
  std::vector<std::pair<uint32_t, std::string>> list;
  for (const auto& [name, n] : gone) list.emplace_back(n, name);
  std::sort(list.rbegin(), list.rend());
  std::string text;
  for (size_t k = 0; k < list.size() && k < 24; ++k)
    text += (k ? ", " : "") + list[k].second + " " + std::to_string(list[k].first);
  REXLOG_INFO("[select3d] (test) the collection took {} objects of {} packages: {}", total, list.size(), text);
  g_gc_before.clear();
}

void MaybeCollect(uint8_t* base) {
  if (REXCVAR_GET(mk9_select_preview_test_gc_report)) GcReport(base);
  if (!g_gc_due || g_frame - g_gc_frame < 180) return;
  // only with nothing loading nor about to (it would take a package nobody holds yet), and no new load for a while
  for (const Side& s : g_sides)
    if (s.state == Side::State::kLoading || s.perf_loading || !EqualNoCase(Wanted(s), s.package)) return;
  if (g_lines_asked && !g_lines_held) return;
  if (L32(base, kAsyncPackagesNum) != 0) return;
  const uint32_t state = L32(base, kGameState);
  if (!Readable(state, 68)) return;
  g_gc_fighters.clear();
  for (const std::string& f : g_loaded_fighters)
    if (!EqualNoCase(f, g_sides[0].fighter) && !EqualNoCase(f, g_sides[1].fighter)) g_gc_fighters.push_back(f);
  g_gc_ours = true;
  if (REXCVAR_GET(mk9_select_preview_test_gc_report)) GcSnapshot(base);
  S32(base, state + 64, 1);
  REXLOG_INFO("[select3d] a garbage collection asked for ({} objects)", ObjectCount(base));
  g_load_block_until = g_frame + 20;
  g_gc_due = false;
  g_gc_frame = g_frame;
}

}  // namespace

void OnFrame(PPCContext& ctx, uint8_t* base) {
  if (++g_frame == 1) StoryPackage();  // the language the game started with (F2 changes it for the next start)
  const bool select = mk9::fight_state::State(base) == kStateSelect;
  if (!select) {
    if (g_was_select) {
      TearDown(ctx, base, false);
      for (Side& s : g_sides) s.fighter.clear(), s.costume.clear(), s.picked = false;
      g_view = 0;
      g_background = 0;
      g_was_select = false;
      g_gc_due = false;  // the fight's load collects as it goes: none asked for outside the select screen
      g_gc_ours = false;
      g_loaded_fighters.clear();
    }
    g_select_frames = 0;
    return;
  }
  g_was_select = true;
  ++g_select_frames;
  if (const std::string& picks = REXCVAR_GET(mk9_select_preview_test_pick); !picks.empty()) {
    for (size_t at = 0; at < picks.size();) {
      const size_t end = std::min(picks.find(',', at), picks.size());
      const std::string item = picks.substr(at, end - at);
      at = end + 1;
      const size_t colon = item.find(':');
      if (colon == std::string::npos) continue;
      const int side = std::atoi(item.c_str()) - 1;
      if ((side == 0 || side == 1) && std::strtoull(item.c_str() + colon + 1, nullptr, 10) == g_select_frames) {
        g_sides[side].picked = true;
        REXLOG_INFO("[select3d] (test) side {} picked at the select's frame {}", side + 1, g_select_frames);
      }
    }
  }
  if (const std::string& toggle = REXCVAR_GET(mk9_select_preview_test_toggle); !toggle.empty()) {
    float at[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    ReadFloats(toggle, {&at[0], &at[1], &at[2], &at[3], &at[4], &at[5], &at[6], &at[7]});
    for (float f : at)
      if (f >= 0 && uint64_t(f) == g_select_frames) {
        const bool now = !REXCVAR_GET(mk9_select_preview);
        rex::cvar::SetFlagByName("mk9_select_preview", now ? "true" : "false");
        REXLOG_INFO("[select3d] (test) the option turned {} at the select's frame {}", now ? "on" : "off",
                    g_select_frames);
      }
  }
  const bool on = REXCVAR_GET(mk9_select_preview) && !mk9::netplay::Active() && !mk9::netplay::SessionGame();
  if (!on) {
    if (AnyActive() || g_sides[0].art_hidden || g_sides[1].art_hidden || g_sides[0].lights[0] || g_sides[1].lights[0])
      TearDown(ctx, base, true);
    return;
  }
  if (!g_view) return;
  FindBackground(ctx, base);
  HoldLines(ctx, base);
  for (Side& s : g_sides) Step(ctx, base, s);
  HideBackground(ctx, base, AnyActive());
  MaybeCollect(base);
}

}  // namespace mk9::select_preview

// ---------------------------------------------------------------------------------------------------------------------
// The select script's hooks (the game thread, inside its natives)

using namespace mk9::select_preview;

namespace {
int SideOfPath(const std::string& path, const char* tag) {  // "..._team1..." / "...Side2..." -> 0 / 1
  const size_t at = path.find(tag);
  if (at == std::string::npos || at + std::strlen(tag) >= path.size()) return -1;
  const char d = path[at + std::strlen(tag)];
  return d == '1' ? 0 : d == '2' ? 1 : -1;
}
}  // namespace

// A clip wrapper's SetImage(wrapper, image path): the select script puts each side's art in its body clip
extern "C" REX_FUNC(__imp__sub_82A3B6C0);
REX_HOOK_RAW(sub_82A3B6C0) {
  const uint32_t wrapper = ctx.r3.u32, image = ctx.r4.u32;
  const std::string path = FStringAt(base, wrapper + 8);  // the clip's full path
  if (path.find("_root.ps.characters_team") == 0 && path.find(".bodies_1.body.image") != std::string::npos) {
    const int side = SideOfPath(path, "characters_team");
    // "ui_ps_vs_jpegs.BD_<fighter>.jpeg" (a few have "_f", the art turned for player 2's side)
    std::string art = Lower(CString(base, image, 96));
    std::string fighter;
    const size_t bd = art.find(".bd_");
    if (bd != std::string::npos) {
      fighter = art.substr(bd + 4);
      fighter = fighter.substr(0, fighter.find('.'));
      if (fighter.size() > 2 && fighter.compare(fighter.size() - 2, 2, "_f") == 0) fighter.resize(fighter.size() - 2);
      if (fighter == "dlc" || fighter == "random" || fighter.find("locked") != std::string::npos) fighter.clear();
    }
    if (side >= 0) {
      Side& s = g_sides[side];
      if (fighter != s.fighter) {
        s.fighter = fighter;
        s.costume.clear();
        s.picked = false;
        s.wish_frame = g_frame;
      }
      // (no call of the game's in an online session: the preview is offline only, and a session's game does only what
      // the other machines' do)
      uint32_t view = 0;
      if (!mk9::netplay::Active() && !mk9::netplay::SessionGame()) {
        Caller call(ctx, base);
        const uint32_t root = call.Call(kClipRoot, {wrapper});
        view = Readable(root, 92) ? L32(base, root + 88) : 0;
      }
      if (view && view != g_view) {
        g_view = view;
        g_background = 0;
        g_background_scan = 1;
        g_background_hidden = false;
      }
    }
  }
  __imp__sub_82A3B6C0(ctx, base);
}

// The scripts' Localize(section, key, default): the costume menu localizes the costume's package name
extern "C" REX_FUNC(__imp__sub_82A235B0);
REX_HOOK_RAW(sub_82A235B0) {
  const uint32_t section = ctx.r3.u32, key = ctx.r4.u32;
  if (section && key && CString(base, section, 16) == "PlayerSelect") {
    const std::string k = CString(base, key, 64);
    if (StartsNoCase(k, "char_")) {
      g_costume_key = k;
      g_costume_frame = g_frame;
    }
  }
  __imp__sub_82A235B0(ctx, base);
}

// A clip wrapper's text set: the costume menu writes the costume's name into _root.ps.AltCostumeSide<N>
extern "C" REX_FUNC(__imp__sub_82A38648);
REX_HOOK_RAW(sub_82A38648) {
  if (g_costume_frame == g_frame && !g_costume_key.empty()) {
    const std::string path = FStringAt(base, ctx.r3.u32 + 8);
    const int side = SideOfPath(path, "AltCostumeSide");
    if (side >= 0) {
      Side& s = g_sides[side];
      // the default costume's key is the fighter's own ("char_scorpion")
      const std::string costume = EqualNoCase(g_costume_key, "char_" + s.fighter) ? std::string() : g_costume_key;
      if (!EqualNoCase(costume, s.costume)) {
        s.costume = costume;
        s.wish_frame = g_frame;
      }
    }
    g_costume_key.clear();
  }
  __imp__sub_82A38648(ctx, base);
}

// The scripts' trace native (printf: the VM's arguments are the format and its values): "Player %d: selected" says
// whose pick it is (0, 1), for A on a fighter and for START opening its costume menu (which is not a pick yet)
extern "C" REX_FUNC(__imp__sub_82479280);
REX_HOOK_RAW(sub_82479280) {
  if (g_was_select) {
    const uint32_t vm = L32(base, kScriptVm);
    const uint32_t args = Readable(vm, 64) ? L32(base, vm + 60) : 0;
    if (Readable(args, 12)) {
      const uint32_t format = L32(base, args + 4);
      if (Readable(format, 24) && CString(base, format, 32) == "Player %d: selected") {
        g_selected_player = int(L32(base, args + 8));
        g_selected_frame = g_frame;
      }
    }
  }
  __imp__sub_82479280(ctx, base);
}

// The scripts' sprintf native (sub_8246E430: its arguments are the text's buffer, its size, the format and its values):
// a pick made, the select script formats "this._parent.SelectionHighlight<N>.gotoAndPlay" (N: a highlight's, not the
// player's) and, in the same frame, before it the trace "Player %d: selected" (A on a fighter) or after it
// "ps.AltCostumeSide<side>.gotoAndPlay" (A in the costume menu, closing it). START, which opens the costume menu, traces
// "selected" with no highlight: the pick is the menu's A.
extern "C" REX_FUNC(__imp__sub_8246E430);
REX_HOOK_RAW(sub_8246E430) {
  uint32_t out = 0;
  if (g_was_select) {
    const uint32_t vm = L32(base, kScriptVm);
    const uint32_t args = Readable(vm, 64) ? L32(base, vm + 60) : 0;
    out = Readable(args, 8) ? L32(base, args + 4) : 0;
  }
  __imp__sub_8246E430(ctx, base);
  if (!out || !Readable(out, 48)) return;
  const std::string text = CString(base, out, 48);
  constexpr std::string_view kHighlight = "this._parent.SelectionHighlight";
  constexpr std::string_view kCostumeMenu = "ps.AltCostumeSide";
  if (text.compare(0, kHighlight.size(), kHighlight) == 0) {
    g_highlight_frame = g_frame;
    if (g_selected_frame == g_frame && (g_selected_player == 0 || g_selected_player == 1))
      g_sides[g_selected_player].picked = true;
  } else if (g_highlight_frame == g_frame && text.size() > kCostumeMenu.size() &&
             text.compare(0, kCostumeMenu.size(), kCostumeMenu) == 0 &&
             text.compare(kCostumeMenu.size() + 1, std::string::npos, ".gotoAndPlay") == 0) {
    const char n = text[kCostumeMenu.size()];
    if (n == '1' || n == '2') g_sides[n - '1'].picked = true;
  }
}

// The skeletal actor's destructor (MK object 1021, sub_829F6D98): the game deletes its objects when the select screen
// ends (before its state changes); one of ours deleted by it is let go, never touched again
extern "C" REX_FUNC(__imp__sub_829F6D98);
REX_HOOK_RAW(sub_829F6D98) {
  const uint32_t object = ctx.r3.u32;
  if (object && object != g_deleting) {
    for (Side& s : g_sides) {
      if (s.object != object) continue;
      REXLOG_INFO("[select3d] side {}: the game deleted the preview's object (the select screen ending)", s.index + 1);
      s.object = s.actor = s.component = 0;
      s.stance = s.taunt = 0;
      Unroot(base, s);
      s.state = Side::State::kGone;
    }
  }
  __imp__sub_829F6D98(ctx, base);
}


// mk9-lauta (2026-11-24, diagnostic): UObject::CollectGarbage (sub_826DD740: KeepFlags, bPerformFullPurge), whoever
// asks for it, logged while the select screen is up: when the game's own run against the preview's loads and picks
extern "C" REX_FUNC(__imp__sub_826DD740);
REX_HOOK_RAW(sub_826DD740) {
  std::vector<uint32_t> held;
  const bool ours = g_gc_ours && g_was_select;
  g_gc_ours = false;
  uint32_t flushed = 0;
  if (g_was_select) {
    // mk9-lauta (2026-11-24): the packages the preview is loading, finished before anything is collected. The game's
    // DLC manager (sub_8260C268) collects as the select screen's cursor reaches a DLC fighter (Skarlet, Kenshi, Rain,
    // Freddy), without UObject::FlushAsyncLoading first as UE3 asks (sub_82700150 does it): on the console nothing is
    // loading then. The preview was, that very fighter: the collection took the package half made ("loaded but no
    // mesh found" in the logs) and the loader went on writing into what it had freed, memory the fight then gave to
    // its textures (the orange blocks over the HUD and the effects, players' screenshots of 1.4.3).
    flushed = L32(base, kAsyncPackagesNum);
    if (flushed) {
      Caller call(ctx, base);
      call.Call(kFlushAsyncLoading, {});
    }
    held = ours ? HoldTheRest(base) : HoldThePreviews(base);
  }
  static uint32_t logged = 0;
  if (g_was_select && logged++ < 64)
    REXLOG_INFO("[select3d] a garbage collection runs ({}; {} objects, {} held for it; {} packages that were loading "
                "finished first)",
                ours ? "the preview's: its fighters shown no more and its cinematics" : "the game's: the preview's held",
                ObjectCount(base), held.size(), flushed);
  __imp__sub_826DD740(ctx, base);
  for (uint32_t o : held)
    if (IsObject(base, o)) S32(base, o + 12, L32(base, o + 12) & ~kRootSet);
}
