#include "window.h"

#include <nfd.h>
#include <nfd_glfw3.h>

namespace rhi
{

namespace window
{

std::tuple<bool, std::string>
OpenFileDialogue(std::string&& resourcePathString, const std::vector<FileFilter>& filterList)
{
	std::vector<nfdu8filteritem_t> nfdFilterList;
	nfdFilterList.reserve(filterList.size());
	for (const auto& [name, spec] : filterList)
		nfdFilterList.push_back(nfdu8filteritem_t{.name = name, .spec = spec});

	nfdu8char_t* openFilePath;
	nfdopendialogu8args_t args{};
	args.filterList = nfdFilterList.data();
	args.filterCount = nfdFilterList.size();
	args.defaultPath = resourcePathString.c_str();
	NFD_GetNativeWindowFromGLFWWindow(reinterpret_cast<GLFWwindow*>(GetCurrentWindow()), &args.parentWindow); // NOLINT(performance-no-int-to-ptr)

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

} // namespace rhi
