// MK9 online: a session's match for the people watching it (mk9-lauta, 2026-11-10). The room's host passes its session
// recording (netplay.cpp's Recorder: the seed, the epochs, every final frame's two inputs and sync hash) to each
// spectator through the lobby's relay (the same "sig" messages as the room's, to the spectator's own id), from the
// start and then as it grows; the spectator's game plays it back from the first frame (netplay.cpp, watching).
//
// The host's upload is the fight's too: the spectators share ~34 KB/s of it (11 times the match's own pace for one,
// 3 times each for kMaxWatchers), a room takes kMaxWatchers at most, and one that stops asking (its "watch", every 2
// s, says how much it has) is dropped after 20 s. A piece lost on the way (a lobby link back up) goes again from what
// the spectator says it has.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mk9::feed {

// --- the host ---
// The recording as it is written (game thread).
void Append(const void* data, size_t size);
constexpr int kMaxWatchers = 4;

// A spectator's word (the room's "watch" message: the lobby's guest id it joined the room with, and the recording's
// bytes it has; UI thread). false: a new one, and the room has kMaxWatchers already.
bool Heard(const std::string& id, uint64_t have);
int Watchers();
// What each spectator lacks, a little at a time (the room's pump, UI thread); ended: the session is over (once).
// budget: the recording's bytes a turn (60 ms), the spectators' together: kBudgetMenus (~34 KB/s); mk9-lauta
// (2026-11-11): in a fight with rollback less (kBudgetFight ~25 KB/s, kBudgetFightSlow ~8.5 KB/s with a long or lossy
// round trip), so a spectator catching up never takes the player's upload from the fight. Each spectator gets 256
// bytes a turn at least (~4 KB/s: the match's own pace is ~3 KB/s).
constexpr size_t kBudgetMenus = 2048, kBudgetFight = 1536, kBudgetFightSlow = 512;
void Pump(const std::string& room, bool ended, size_t budget = kBudgetMenus);

// --- the spectator ---
// What came: the bytes at offset `at` of the recording (UI thread), or its end.
void Received(uint64_t at, const std::string& base64);
void ReceivedEnd();
// What the game thread reads: false when those bytes have not come yet.
bool Read(uint64_t at, void* out, size_t size);
uint64_t Size();
bool Ended();

// --- a replay (mk9-lauta, 2026-11-11) ---
// A session recording read whole from a file (netplay.cpp, mk9_net_replay), as if the relay had brought all of it.
bool LoadFile(const std::wstring& path);

}  // namespace mk9::feed
