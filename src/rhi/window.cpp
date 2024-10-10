#include "window.h"

#include <nfd_glfw3.h>

namespace window
{

std::tuple<bool, std::string>
OpenFileDialogue(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList)
{
	nfdu8char_t* openFilePath;
	nfdopendialogu8args_t args{};
	args.filterList = filterList.data();
	args.filterCount = filterList.size();
	args.defaultPath = resourcePathString.c_str();
	NFD_GetNativeWindowFromGLFWWindow(static_cast<GLFWwindow*>(GetCurrentWindow()), &args.parentWindow);

	if (NFD_OpenDialogU8_With(&openFilePath, &args) == NFD_OKAY)
	{
		std::string openFilePathStr;
		openFilePathStr.assign(openFilePath);
		NFD_FreePath(openFilePath);
		return std::make_tuple(true, std::move(openFilePathStr));
	}

	return {false, {}};
}

} // namespace window
