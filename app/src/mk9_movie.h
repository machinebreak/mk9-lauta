// mk9-nx: MK9's cutscenes (Movies\*.wmv) decoded by the host's FFmpeg (mk9_movie.cpp). mk9-lauta: skipped with
// START or A (mk9_skip_cutscenes, issue #12); their picture from the PC version's Movies\<name>.bik when it is there
// (mk9_movie_hq, issue #11).

#pragma once

#include <string_view>

namespace mk9::movie {

// A movie decoded here is playing.
bool Playing();

// A file the game opened (the SDK's file-opened hook, mk9_file_check.cpp; any thread): a Movies\<name>.wmv is the
// movie the next codec start plays.
void NoteOpened(std::string_view guest_path);

}  // namespace mk9::movie
