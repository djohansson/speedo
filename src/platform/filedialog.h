#pragma once

#include <string>
#include <tuple>
#include <vector>

namespace platform
{

// mirrors nfdu8filteritem_t, so the nfd header stays in the implementation
struct FileFilter
{
	const char* name; // shown in the dialogue
	const char* spec; // comma separated extensions, e.g. "jpg,png"
};

// shows a native open file dialogue over the current window (see GetCurrentWindow), starting in resourcePathString.
// returns whether a file was chosen, and its path. call on the main thread.
[[nodiscard]] std::tuple<bool, std::string> OpenFileDialogue(std::string&& resourcePathString, const std::vector<FileFilter>& filterList);

// shows a native folder picker over the current window, starting in startPathString. returns whether a folder was chosen,
// and its path. call on the main thread.
[[nodiscard]] std::tuple<bool, std::string> OpenFolderDialogue(std::string&& startPathString);

} // namespace platform
