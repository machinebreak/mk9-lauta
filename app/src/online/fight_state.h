// MK9 online: what a fight's state says about the match (mk9-lauta, 2026-11-10): the fighters' characters and health,
// for the players' records (who won, with whom).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mk9::fight_state {

// The character player 0 or 1 fights with, as the game names it ("CHAR_SCORPION"; "" when there is none). Game thread.
std::string CharacterName(uint8_t* base, int player);
// That name for people ("Scorpion", "Sub-Zero").
std::string DisplayName(const std::string& game_name);
// The fighter's health, 0-1 (-1: no fighter). Game thread.
float Health(uint8_t* base, int player);

// The game's state (its GS_ names: sub_829B01D8), -1 when there is no game object yet. Game thread.
enum GameState : int { kStateLoadingFight = 1, kStateMainMenu = 3, kStateSelect = 7, kStatePlay = 8 };
int State(uint8_t* base);

// A match decided (the fight's mode over: netplay.cpp), as both games see it.
struct Result {
  int me = 0;                 // this machine's player (0: the host, player 1)
  std::string characters[2];  // DisplayName, by player
  float health[2] = {-1, -1};
  bool tag = false;           // a tag match (two fighters a side): no winner taken
};
// The game thread reports it (at the frame the fight's mode ended); the online screens take it (any thread).
void Report(uint8_t* base, int me);
bool Take(Result* out);

// mk9_diag_fighters: each fighter's character and health in the log.
void Diagnose(uint8_t* base);
// The fighters' state for a fight's sync hash (A2): seed and the fight's state hashed (seed as it is out of a fight).
// The ranges are the ones the two machines always had the same (fighter_diff.py). Game thread, at a frame's boundary;
// key: the frame's (epoch << 32 | frame), whose hashed bytes are kept (the latest 64 frames) for a desync's report.
uint64_t HashFighters(uint8_t* base, uint64_t seed, uint64_t key);
// A desync's report (A3): the kept frames of upto's epoch up to it, with the ranges they come from, to a file
// (tools/online/desync_report.py compares the two players'). Game thread.
bool WriteFighters(const std::filesystem::path& file, uint64_t upto);
// The rollback SyncTest (snapshot.cpp, mk9_synctest): at a frame's boundary at rest, the fighters' whole state (each
// player's block of the match's info, fighter object and actor) after the frame `key` (the game's frame counter): its
// first run kept, a run again compared word by word; the words that came out otherwise are logged ("[synctest]
// fighters") and counted (SyncTestFighterSummary). What a rollback does not bring back the same in the fighters.
void SyncTestFighters(uint8_t* base, uint32_t key);
std::string SyncTestFighterSummary();
// mk9_net_test_desync_frame (tests): right before that game frame runs (each run of it), the guest (player 1) takes
// 5 % of player 1's fighter's health. Host-side write: the timeline does not see its page change (a rollback past it
// does not undo it, a run again takes 5 % more). player: netplay::LocalPlayer().
void TestDesync(uint8_t* base, int player);
// mk9_fighter_dump (tests): at a frame's boundary in a fight, every mk9_fighter_dump_every frames of the game's frame
// counter, each player's block of the match's info and the start of their fighter's object, to a file. Game thread.
void DumpFighters(uint8_t* base);

}  // namespace mk9::fight_state
