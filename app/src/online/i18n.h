// MK9 online: the online screens in the player's language (mk9-lauta, 2026-11-10): Spanish (the texts' own), English
// and Portuguese, after the game's language (F2 > Idioma del juego: English, Português; French, German and Italian
// get English), or mk9_online_language.

#pragma once

namespace mk9::i18n {

// The text in the player's language. es: the Spanish text, which is also its key (formats keep their placeholders,
// "%s" or "{}", in every language); a text the table lacks stays in Spanish.
const char* T(const char* es);

// mk9-lauta (2026-11-11, F3): the game's language just chosen (the first start's steps), as the online screens'
// language ("es", "en", "pt"): they follow it at once, unless mk9_online_language says another than auto.
void SetLanguage(const char* code);

}  // namespace mk9::i18n
