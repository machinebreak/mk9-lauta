// MK9 online: the countries' flags in the online screens and this player's country (mk9-lauta, 2026-11-10).
//
// The flags are one atlas (flags_atlas.png, 40x30 cells: tools/online/make_flags.py, Flagpedia's flags, public
// domain), an ImGui texture of the overlay's own context. The country goes to the lobby with the name (it shows next to
// it in the global chat and the rooms list) and to the rival with the room's messages: the first of mk9_online_country,
// Cloudflare's trace (by the connection's address) and Windows' region.

#pragma once

#include <string>

struct ImDrawList;
struct ImVec2;

namespace mk9::flags {

// The flag of a two-letter code (any case) at the cursor, `height` pixels high (40:30), as an item: false (nothing
// drawn) for an unknown code. The overlay's ImGui context.
bool Draw(const std::string& code, float height);
// The same at a position of a draw list (no layout: the fight's name plates).
bool DrawAt(ImDrawList* list, const std::string& code, const ImVec2& pos, float height);
// Whether a code has a flag.
bool Known(const std::string& code);
// The country's name ("Argentina"), or the code itself when unknown.
std::string Name(const std::string& code);

// An estimate of the round trip between players in two countries (the distance between their centres, at the
// network's usual detours: 30 ms + 2.2 ms per 100 km), ms; 0 when either is unknown. Only a guide: the real one shows
// once the two are connected.
double EstimatedRoundTripMs(const std::string& a, const std::string& b);

// This player's country, two capital letters ("" until known): Detect finds it once, on a thread of its own.
std::string Mine();
void Detect();

}  // namespace mk9::flags
