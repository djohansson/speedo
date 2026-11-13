// the client's C api (client/capi.h), the one part of the client compiled once: gfx and the client are compiled once per
// graphics api the platform has, each copy in an inline namespace of its own (gfx/graphicsapi.h), and this runs the copy
// SPEEDO_GRAPHICS_API names: vulkan (the default), or metal (macOS's)
#include <client/capi.h>

#include <core/assert.h>

#include <cstdlib>
#include <string_view>

// each copy's entry points (client/client.h's, which this can't include: its declarations are a copy's)
namespace client::inline vk
{
void Create(const PathConfig* paths);
bool Main();
void Destroy();
} // namespace client::inline vk

#if defined(__APPLE__)
namespace client::inline metal
{
void Create(const PathConfig* paths);
bool Main();
void Destroy();
} // namespace client::inline metal
#endif

namespace client::capi
{
namespace
{

struct Copy
{
	void (*create)(const PathConfig*);
	bool (*main)();
	void (*destroy)();
};

constexpr Copy kVulkan{.create = vk::Create, .main = vk::Main, .destroy = vk::Destroy};
#if defined(__APPLE__)
constexpr Copy kMetal{.create = metal::Create, .main = metal::Main, .destroy = metal::Destroy};
#endif

const Copy* gCopy = nullptr; // the one running, from ClientCreate to ClientDestroy

// the copy SPEEDO_GRAPHICS_API names: vulkan unless it says metal, which only macOS has (a hard failure elsewhere, and
// for any other name)
const Copy& Chosen()
{
	const char* value = std::getenv("SPEEDO_GRAPHICS_API");
	const std::string_view api = value != nullptr ? value : "";
	if (api.empty() || api == "vulkan")
		return kVulkan;
#if defined(__APPLE__)
	if (api == "metal")
		return kMetal;
	ENSUREF(false, "SPEEDO_GRAPHICS_API={}: vulkan or metal", api);
#else
	ENSUREF(false, "SPEEDO_GRAPHICS_API={}: vulkan (metal is macOS's)", api);
#endif
	return kVulkan;
}

} // namespace
} // namespace client::capi

void ClientCreate(const PathConfig* paths)
{
	using namespace client::capi;

	ENSURE(gCopy == nullptr);
	gCopy = &Chosen();
	gCopy->create(paths);
}

bool ClientMain()
{
	using namespace client::capi;

	return gCopy->main();
}

void ClientDestroy()
{
	using namespace client::capi;

	gCopy->destroy();
	gCopy = nullptr;
}
