// MK9 online: what a fight's state says about the match (mk9-lauta, 2026-11-10): the fighters' characters and health,
// for the players' records (who won, with whom).

#pragma once

#include <cstdint>
#include <string>

namespace mk9::fight_state {

// The character player 0 or 1 fights with, as the game names it ("CHAR_SCORPION"; "" when there is none). Game thread.
std::string CharacterName(uint8_t* base, int player);
// That name for people ("Scorpion", "Sub-Zero").
std::string DisplayName(const std::string& game_name);
// The fighter's health, 0-1 (-1: no fighter). Game thread.
float Health(uint8_t* base, int player);

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

}  // namespace mk9::fight_state
