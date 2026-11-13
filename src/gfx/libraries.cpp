// the header only libraries gfx uses, their implementations compiled once: gfx itself is compiled once per graphics api
// (CMakeLists.txt's speedo_graphics_api_libraries), and two copies of a library's functions in one executable don't link.
// each is configured as the gfx sources that include it configure it (tinybvh by gfx/tinybvh.h; the others by nothing)

// first: it includes cgltf.h, whose implementation's section, below, has no include guard of its own
#include <gfx/libraries.h>

#define TINYBVH_IMPLEMENTATION
#include <gfx/tinybvh.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace gfx::libraries
{

std::vector<std::vector<std::string>> AnimationPointers(const cgltf_data& data)
{
	std::vector<std::vector<std::string>> result(data.animations_count);
	for (cgltf_size animationIt = 0; animationIt < data.animations_count; animationIt++)
		result[animationIt].resize(data.animations[animationIt].channels_count);
	if (data.json == nullptr || data.json_size == 0)
		return result;

	jsmn_parser parser;
	jsmn_init(&parser);
	auto count = jsmn_parse(&parser, data.json, data.json_size, nullptr, 0);
	if (count <= 0)
		return result;
	std::vector<jsmntok_t> tokens(static_cast<size_t>(count));
	jsmn_init(&parser);
	if (jsmn_parse(&parser, data.json, data.json_size, tokens.data(), tokens.size()) < 0)
		return result;

	std::string_view json(data.json, data.json_size);
	auto text = [&](int token) { return json.substr(tokens[token].start, tokens[token].end - tokens[token].start); };
	// the token after token's subtree (an object's keys have their value as their one child)
	auto skip = [&](int token)
	{
		for (int pending = 1; pending > 0 && std::cmp_less(token, tokens.size()); token++)
			pending += tokens[token].size - 1;
		return token;
	};
	auto member = [&](int object, std::string_view key) -> int
	{
		if (object < 0 || tokens[object].type != JSMN_OBJECT)
			return -1;
		auto token = object + 1;
		for (int keyIt = 0; keyIt < tokens[object].size && std::cmp_less(token + 1, tokens.size()); keyIt++)
		{
			if (tokens[token].type == JSMN_STRING && text(token) == key)
				return token + 1;
			token = skip(token);
		}
		return -1;
	};
	auto elements = [&](int array)
	{
		std::vector<int> result;
		if (array < 0 || tokens[array].type != JSMN_ARRAY)
			return result;
		for (int elementIt = 0, token = array + 1; elementIt < tokens[array].size; elementIt++, token = skip(token))
			result.push_back(token);
		return result;
	};

	auto animations = elements(member(0, "animations"));
	for (size_t animationIt = 0; animationIt < std::min(animations.size(), result.size()); animationIt++)
	{
		auto channels = elements(member(animations[animationIt], "channels"));
		for (size_t channelIt = 0; channelIt < std::min(channels.size(), result[animationIt].size()); channelIt++)
		{
			auto pointer = member(member(member(member(channels[channelIt], "target"), "extensions"), "KHR_animation_pointer"), "pointer");
			if (pointer >= 0 && tokens[pointer].type == JSMN_STRING)
				result[animationIt][channelIt] = std::string(text(pointer));
		}
	}
	return result;
}

} // namespace gfx::libraries
