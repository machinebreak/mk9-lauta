// mk9-lauta (2026-11-11): the release's own look: the "MK9 BY LAUTA" logo and Lauta's social networks, for the screens
// (the first start's welcome, F2 > Acerca de, the online profile) and the game's main menu.
//
// The pictures are inside the executable (app/src/brand, made by tools/brand/make_brand_assets.py): the logo at 1024,
// 640, 400 and 240 px wide and the networks' icons (Simple Icons, CC0) at 128, 64 and 32 px, each drawn from the size
// nearest to what it is drawn at (the overlay's textures have no mipmaps). They are textures of the overlay's ImGui,
// decoded the first time they are drawn.
//
// The main menu's corner (Install): "MK9 Online by Lauta" and the networks' buttons at the bottom right of the game's
// main menu, where the game draws nothing; never in an online session's game nor with an overlay window open, and off
// with mk9_brand_menu=false (F2 > Pantalla e idioma). mk9-lauta (2026-11-16): above them, "help keep the server and the
// updates going" and the "Apoyar" button, which opens F2 > Apóyame.

#pragma once

#include <cstddef>

#include "imgui.h"

namespace mk9::brand {

// mk9-lauta (2026-11-16): after the networks, "Apóyame"'s: Mercado Pago (from Argentina: an alias, copied when its
// button is pressed; Cafecito's place until 2026-11-17), Ko-fi (the rest of Latin America and the world), Patreon (every
// month) and the heart of the button that opens F2 > Apóyame (no page of its own).
enum class Social { kDiscord, kWhatsApp, kInstagram, kKick, kYouTube, kX, kMercadoPago, kKofi, kPatreon, kHeart };
constexpr int kSocialCount = 10;

struct Link {
  Social id;
  const char* name;   // "Discord"
  const char* label;  // the button's words, in Spanish (i18n::T has the others): "Unirme al Discord"
  const char* url;    // "" for the heart (its button opens F2 > Apóyame) and Mercado Pago (its button copies `copy`)
  ImU32 color;        // the network's
  const char* copy = nullptr;  // what pressing the button copies to the clipboard instead of opening a page (the alias)
};
const Link& LinkOf(Social social);

// The release's project page and its latest release (the updater's repository).
inline constexpr const char* kProjectUrl = "https://github.com/machinebreak/mk9-lauta";
inline constexpr const char* kDownloadUrl = "https://github.com/machinebreak/mk9-lauta/releases/latest";

// The logo's height for a width (its pictures' proportion).
float LogoHeight(float width);
// The logo at the cursor, `width` px wide, centered in the content region when center (an ImGui item).
void Logo(float width, bool center = true);
// The logo at a position of a draw list.
void LogoAt(ImDrawList* list, const ImVec2& pos, float width, ImU32 tint = IM_COL32_WHITE);
// A network's icon, `size` px square, in `color` (the icons are white: the color tints them).
void IconAt(ImDrawList* list, Social social, const ImVec2& pos, float size, ImU32 color);

// A button as the project's site has them: dark, a thin border, the network's icon in its color and the label (solid:
// the network's color behind, the icon white: Discord's); the mouse over it lights the border in the network's color.
// Pressed: the link opens in the browser. label: nullptr for the network's own words (translated). True when pressed.
bool SocialButton(Social social, const ImVec2& size, bool solid = false, const char* label = nullptr);
// The networks as a block of buttons `width` px wide: Discord's across, the others `columns` a row.
void SocialButtons(float width, int columns, float height);

// mk9-lauta (2026-11-16): F2 > Apóyame: what a donation keeps going (the online's server, the updates), the three ways
// to give (Mercado Pago, Ko-fi, Patreon: who each is for) and, for whoever cannot, the networks to follow. `width` px wide,
// in the current window; `scale`: the overlay's text size (options_ui::UiScale).
void SupportPanel(float width, float scale);

// Once (mk9_capture.cpp, with the other overlay windows): the main menu's corner.
void Install();

}  // namespace mk9::brand
