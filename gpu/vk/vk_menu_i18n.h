// The Insert menu's languages (vk_menu_i18n.cpp): its English texts as written in vk_menu.cpp, shown in the language the Menu
// language row picks (Auto: the game's own language, language.txt). The tables are menu_i18n.h's. ui::text, ui::para,
// ui::size_of and ui::spaced (vk_ui.h) look every text up, so a row, a value or a help line is drawn translated without its
// own call; a text the table does not have is drawn as it is.
#pragma once

#include <string>

namespace mkwvk::i18n {

/// The language now: an index into menu_i18n::kLangs (0 English)
int lang_now();
/// Its code ("pt-BR", "ja", "en")
const char* lang_code();
/// The plural form a count takes in it: 0 one, 1 more than one, 2 many (Russian and Polish only: 5 and up, 11 to 14, ...)
int plural(int n);
/// The Menu language row's choices: 0 Auto, then each of menu_i18n::kLangs by its own name
int lang_count();
const char* lang_name(int index);
/// The language the game runs in, e.g. "pt-BR", read once as host/nn_applet.c reads it: HOS_LANGUAGE over language.txt (its
/// first line that is not blank or a comment and names a language the game has), any case, "en-US" when neither does. The
/// menu's Auto follows it until the next start
const std::string& game_language();

/// The game's languages, as nx2/make_game.py LANGUAGES lists them (language.txt's codes), each by its own name
int game_lang_count();
const char* game_lang_code(int index);
const char* game_lang_name(int index);
/// The index of language.txt's language as last saved (the game's next start; the one it runs in until a change), 0 (English
/// (US)) when it is none of them
int game_lang_index();
/// language.txt's code line set to that language (its comments kept): the game reads it at its next start. False when the
/// file could not be written (it is then as it was). The menu's Auto stays on game_language() until the restart
bool set_game_language(int index);

/// A text as the menu shows it: s itself in English or when the table lacks it, else its translation. A text made from one
/// of the table's printf formats ("%d of %d") is translated through it. The pointer stays good for the process's life
const char* tr(const char* s);
std::string tr_text(const std::string& s);

}  // namespace mkwvk::i18n
