// mk9-nx (2026-11-08): a player with no gamer profile that drives a menu (player 2's controller choosing ONE VS ONE in
// the 2 PLAYERS menu, the online session's guest) gets the game's question at once: "continue without saving".
//
// MK9's profile and storage manager keeps its state at 0x83394B28 (1 = nothing going on) and its object at 0x834261C8.
// When a controller whose user is not signed in drives a menu (sub_829D6A40 makes it the active one, and the settings'
// saves do the same), sub_829D62E8(user) asks that user ProfilePromptTitle / ContinueWithoutGamerProfile with the
// buttons SignIntoProfile_option and ContWithOutSaving_option (XenonSaveMsgs, built by sub_82AA4720): state 2, and a
// thread pool task (sub_82AA5850) shows XamShowMessageBoxUI and waits for its overlapped. The manager's tick
// (sub_829D7748, state 2) then reads the button: 0 = sign in (state 3, the sign-in UI); 1 = continue without saving:
// the user's flag at object+3156+56*user = 1 (+3152 = 0), state 1, [0x834261A0] = 0, object+3124 &= ~0x200. The menus
// wait for state 1 meanwhile (native 8246edd0, sub_829D5B70: the dragon spinning in a corner).
//
// On PC only user 0 has a profile (the SDK signs in one), so the second answer is the only one that works, and the
// question is the SDK's ImGui popup: two games in an online session would each see it end at its own time, or never
// (nobody answers a popup on a game in the background). Here the answer is given on the game thread, with no
// dialog, as the tick gives it: the same frame on both machines. The original runs when it would not ask anyway (the
// game's own online or Practice's recorder installed: [0x8342D01C]; [0x83424840] set).

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(mk9_profile_prompt_auto, true, "MK9",
                    "A player with no gamer profile (every controller but the first, the online guest) continues "
                    "without saving at once instead of getting the game's sign-in question");

namespace {

constexpr uint32_t kState = 0x83394B28;    // the manager's state (1 = idle, 2 = asking)
constexpr uint32_t kObject = 0x834261C8;   // the manager's object
constexpr uint32_t kRequest = 0x834261A0;  // the state a request waits for (cleared when it is reached)
constexpr uint32_t kRecorder = 0x8342D01C;  // the capture interface's object (the game's own online, Practice)
constexpr uint32_t kQuiet = 0x83424840;     // set: no question asked either

uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

// As a guest store: its page's store card marked (rex/system/xmemory.h: the cards are at the guest window's base; the
// rollback's timeline and Renut go by them).
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
  rex::memory::MarkStoreCardsAt(base, a, 4);
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_829D62E8);
REX_HOOK_RAW(sub_829D62E8) {
  const uint32_t user = ctx.r3.u32;
  if (!REXCVAR_GET(mk9_profile_prompt_auto) || user == 0 || user > 3 || Ld32(base, kRecorder) != 0 ||
      Ld32(base, kQuiet) != 0) {
    __imp__sub_829D62E8(ctx, base);
    return;
  }
  St32(base, kObject + 3156 + 56 * user, 1);
  St32(base, kObject + 3152 + 56 * user, 0);
  St32(base, kState, 1);
  St32(base, kRequest, 0);
  St32(base, kObject + 3124, Ld32(base, kObject + 3124) & ~0x200u);
  ctx.r3.u64 = 0;  // state 1: nothing to wait for
  static uint32_t logged = 0;
  if (!(logged & (1u << user))) {
    logged |= 1u << user;
    REXLOG_INFO("[profile] user {} has no gamer profile: continues without saving (no sign-in question)", user);
  }
}
