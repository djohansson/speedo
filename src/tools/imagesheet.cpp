// composes images and labels into one, for the scripts' review sheets and charts (scripts/researchcompare.ps1), on every
// platform: the scripts lay out, this draws.
//
// usage: imagesheet size <image>                 prints "<width> <height>"
//        imagesheet bmp <image> <out.bmp>         writes an image as a bmp (capture::Save's, 32 bit, top down)
//        imagesheet compose <width> <height> [items] --out <file> [--out <file>] [--quality <1-100>]
// items, drawn in order on black:
//   --font <file.ttf>                            the font of the texts after it (stb_truetype, antialiased; without one,
//                                                stb_easy_font's segment glyphs)
//   --image <file> <x> <y> <w> <h>               the image scaled to fit the rectangle, its shape kept, centered in it
//   --text <x> <y> <w> <size> <text>             white text from (x, y), its top, wrapped at words to <w> pixels, <size>
//                                                pixels high (the font's ascent to descent)
// an image is a bmp (24 or 32 bit) or a png; an output is a png, a jpg (at --quality, default 88) or a bmp, by its
// extension. exit 0, 2 on an error.

#include "capture.h"

#include <core/file.h>

// this executable's own copies, static (it links no gfx, whose copies are in gfx/libraries.cpp)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include <stb_image_resize2.h>
#include <stb_easy_font.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

// a bmp (capture::Load), or a png decoded from its mapped bytes (its alpha dropped)
[[nodiscard]] std::optional<capture::Image> LoadImage(const std::filesystem::path& path)
{
	auto file = core::file::Map(path);
	constexpr std::array<uint8_t, 8> kPngSignature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	if (!file || file->size() < kPngSignature.size() ||
		!std::equal(kPngSignature.begin(), kPngSignature.end(), reinterpret_cast<const uint8_t*>(file->data())))
		return capture::Load(path);
	int width = 0;
	int height = 0;
	int channels = 0;
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgb(
		stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file->data()), static_cast<int>(file->size()), &width, &height, &channels, 3), &stbi_image_free);
	if (!rgb)
	{
		std::println(stderr, "{}: can't decode it: {}", path.string(), stbi_failure_reason());
		return std::nullopt;
	}
	capture::Image image{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 3)};
	for (size_t pixelIt = 0; pixelIt < static_cast<size_t>(width) * height; pixelIt++)
		for (size_t channelIt = 0; channelIt < 3; channelIt++)
			image.bgr[(pixelIt * 3) + channelIt] = rgb.get()[(pixelIt * 3) + 2 - channelIt];
	return image;
}

[[nodiscard]] bool SaveImage(const std::filesystem::path& path, const capture::Image& image, int quality)
{
	std::string extension = path.extension().string();
	std::ranges::transform(extension, extension.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
	if (extension == ".bmp")
		return capture::Save(path, image);
	std::vector<uint8_t> rgb(image.bgr.size());
	for (size_t pixelIt = 0; pixelIt < image.bgr.size() / 3; pixelIt++)
		for (size_t channelIt = 0; channelIt < 3; channelIt++)
			rgb[(pixelIt * 3) + channelIt] = image.bgr[(pixelIt * 3) + 2 - channelIt];
	std::vector<std::byte> bytes;
	auto append = [](void* context, void* data, int size)
	{
		auto* out = static_cast<std::vector<std::byte>*>(context);
		auto span = std::span(static_cast<const std::byte*>(data), static_cast<size_t>(size));
		out->insert(out->end(), span.begin(), span.end());
	};
	int written = 0;
	if (extension == ".png")
		written = stbi_write_png_to_func(append, &bytes, image.width, image.height, 3, rgb.data(), image.width * 3);
	else if (extension == ".jpg" || extension == ".jpeg")
		written = stbi_write_jpg_to_func(append, &bytes, image.width, image.height, 3, rgb.data(), quality);
	else
	{
		std::println(stderr, "{}: not a png, jpg or bmp", path.string());
		return false;
	}
	if (written == 0 || !core::file::Write(path, bytes))
	{
		std::println(stderr, "{}: can't write", path.string());
		return false;
	}
	return true;
}

// the image scaled into the rectangle, its shape kept and centered (stb_image_resize2, filtered in linear light)
void DrawImage(capture::Image& canvas, const capture::Image& image, int x, int y, int width, int height)
{
	double fit = std::min(static_cast<double>(width) / image.width, static_cast<double>(height) / image.height);
	int fitWidth = std::max(1, static_cast<int>(image.width * fit));
	int fitHeight = std::max(1, static_cast<int>(image.height * fit));
	std::vector<uint8_t> scaled(static_cast<size_t>(fitWidth) * fitHeight * 3);
	stbir_resize_uint8_srgb(image.bgr.data(), image.width, image.height, image.width * 3, scaled.data(), fitWidth, fitHeight, fitWidth * 3, STBIR_BGR);
	int left = x + ((width - fitWidth) / 2);
	int top = y + ((height - fitHeight) / 2);
	for (int row = 0; row < fitHeight; row++)
	{
		int canvasY = top + row;
		if (canvasY < 0 || canvasY >= canvas.height)
			continue;
		for (int column = 0; column < fitWidth; column++)
		{
			int canvasX = left + column;
			if (canvasX < 0 || canvasX >= canvas.width)
				continue;
			for (size_t channelIt = 0; channelIt < 3; channelIt++)
				canvas.bgr[((static_cast<size_t>(canvasY) * canvas.width + canvasX) * 3) + channelIt] =
					scaled[((static_cast<size_t>(row) * fitWidth + column) * 3) + channelIt];
		}
	}
}

// a white rectangle, its edges' pixels blended by how much of each it covers
void FillRectangle(capture::Image& canvas, float x0, float y0, float x1, float y1)
{
	int firstX = std::max(0, static_cast<int>(std::floor(x0)));
	int lastX = std::min(canvas.width - 1, static_cast<int>(std::ceil(x1)) - 1);
	int firstY = std::max(0, static_cast<int>(std::floor(y0)));
	int lastY = std::min(canvas.height - 1, static_cast<int>(std::ceil(y1)) - 1);
	for (int py = firstY; py <= lastY; py++)
	{
		float coverY = std::min(y1, static_cast<float>(py + 1)) - std::max(y0, static_cast<float>(py));
		for (int px = firstX; px <= lastX; px++)
		{
			float cover = std::clamp(coverY * (std::min(x1, static_cast<float>(px + 1)) - std::max(x0, static_cast<float>(px))), 0.0F, 1.0F);
			for (size_t channelIt = 0; channelIt < 3; channelIt++)
			{
				uint8_t& value = canvas.bgr[((static_cast<size_t>(py) * canvas.width + px) * 3) + channelIt];
				value = static_cast<uint8_t>(std::lround((value * (1.0F - cover)) + (255.0F * cover)));
			}
		}
	}
}

// without a font: the text wrapped at words to the width, in stb_easy_font's segment glyphs (quads, each four vertices of
// 16 bytes: x, y, z, color), size pixels high (its glyphs are 7 at scale 1)
void DrawSegmentText(capture::Image& canvas, float x, float y, float width, float size, std::string_view text)
{
	float scale = size / 7.0F;
	std::string wrapped;
	std::string line;
	auto widthOf = [](std::string value) { return static_cast<float>(stb_easy_font_width(value.data())); };
	size_t at = 0;
	while (at <= text.size())
	{
		size_t end = std::min(text.find(' ', at), text.size());
		std::string word(text.substr(at, end - at));
		std::string candidate = line.empty() ? word : line + ' ' + word;
		if (!line.empty() && widthOf(candidate) * scale > width)
		{
			wrapped += line + '\n';
			line = word;
		}
		else
			line = candidate;
		at = end + 1;
	}
	wrapped += line;
	std::vector<std::byte> vertices(16 * 4 * (wrapped.size() * 64 + 64));
	int quadCount = stb_easy_font_print(0.0F, 0.0F, wrapped.data(), nullptr, vertices.data(), static_cast<int>(vertices.size()));
	for (int quadIt = 0; quadIt < quadCount; quadIt++)
	{
		std::array<float, 8> corners{}; // x, y of the four vertices
		for (size_t vertexIt = 0; vertexIt < 4; vertexIt++)
			std::memcpy(&corners[vertexIt * 2], vertices.data() + (((static_cast<size_t>(quadIt) * 4) + vertexIt) * 16), 2 * sizeof(float));
		float minX = std::min({corners[0], corners[2], corners[4], corners[6]});
		float maxX = std::max({corners[0], corners[2], corners[4], corners[6]});
		float minY = std::min({corners[1], corners[3], corners[5], corners[7]});
		float maxY = std::max({corners[1], corners[3], corners[5], corners[7]});
		FillRectangle(canvas, x + (minX * scale), y + (minY * scale), x + (maxX * scale), y + (maxY * scale));
	}
}

// a TrueType font, its file mapped for as long as stb_truetype reads it
struct Font
{
	core::file::MappedFile file;
	stbtt_fontinfo info{};
};

[[nodiscard]] std::unique_ptr<Font> LoadFont(const std::filesystem::path& path)
{
	auto file = core::file::Map(path);
	if (!file)
	{
		std::println(stderr, "{}: can't read", path.string());
		return nullptr;
	}
	auto font = std::make_unique<Font>(Font{.file = std::move(*file)});
	const auto* bytes = reinterpret_cast<const unsigned char*>(font->file.data());
	if (stbtt_InitFont(&font->info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0)) == 0)
	{
		std::println(stderr, "{}: not a TrueType font", path.string());
		return nullptr;
	}
	return font;
}

// the code points of UTF-8 text (a byte that starts no sequence taken as itself)
[[nodiscard]] std::vector<int> CodePoints(std::string_view text)
{
	std::vector<int> points;
	for (size_t at = 0; at < text.size();)
	{
		auto lead = static_cast<unsigned char>(text[at]);
		size_t length = lead < 0x80U ? 1 : (lead >> 5U) == 0x6U ? 2 : (lead >> 4U) == 0xEU ? 3 : (lead >> 3U) == 0x1EU ? 4 : 1;
		if (at + length > text.size())
			length = 1;
		int point = length == 1 ? lead : static_cast<int>(lead & (0x7FU >> length));
		for (size_t byteIt = 1; byteIt < length; byteIt++)
			point = (point << 6) | (static_cast<unsigned char>(text[at + byteIt]) & 0x3F);
		points.push_back(point);
		at += length;
	}
	return points;
}

// white text in the font from (x, y), the top of its first line, wrapped at spaces to the width, size pixels from its
// ascent to its descent: each glyph rasterized antialiased at its subpixel position and blended over the canvas
void DrawText(capture::Image& canvas, const Font& font, float x, float y, float width, float size, std::string_view text)
{
	float scale = stbtt_ScaleForPixelHeight(&font.info, size);
	int ascent = 0;
	int descent = 0;
	int lineGap = 0;
	stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &lineGap);
	float lineHeight = static_cast<float>(ascent - descent + lineGap) * scale;
	auto advance = [&](int point, int next)
	{
		int advanceWidth = 0;
		int leftBearing = 0;
		stbtt_GetCodepointHMetrics(&font.info, point, &advanceWidth, &leftBearing);
		return static_cast<float>(advanceWidth + (next != 0 ? stbtt_GetCodepointKernAdvance(&font.info, point, next) : 0)) * scale;
	};
	auto widthOf = [&](const std::vector<int>& points)
	{
		float sum = 0.0F;
		for (size_t pointIt = 0; pointIt < points.size(); pointIt++)
			sum += advance(points[pointIt], pointIt + 1 < points.size() ? points[pointIt + 1] : 0);
		return sum;
	};
	// the lines: words joined while they fit
	std::vector<int> all = CodePoints(text);
	std::vector<std::vector<int>> lines;
	std::vector<int> line;
	for (size_t at = 0; at <= all.size();)
	{
		size_t end = std::min(static_cast<size_t>(std::find(all.begin() + static_cast<ptrdiff_t>(at), all.end(), ' ') - all.begin()), all.size());
		std::vector<int> candidate = line;
		if (!candidate.empty())
			candidate.push_back(' ');
		candidate.insert(candidate.end(), all.begin() + static_cast<ptrdiff_t>(at), all.begin() + static_cast<ptrdiff_t>(end));
		if (!line.empty() && widthOf(candidate) > width)
		{
			lines.push_back(line);
			line.assign(all.begin() + static_cast<ptrdiff_t>(at), all.begin() + static_cast<ptrdiff_t>(end));
		}
		else
			line = std::move(candidate);
		at = end + 1;
	}
	lines.push_back(line);
	std::vector<unsigned char> glyph;
	for (size_t lineIt = 0; lineIt < lines.size(); lineIt++)
	{
		float baseline = std::round(y + (static_cast<float>(ascent) * scale) + (static_cast<float>(lineIt) * lineHeight));
		float pen = x;
		const auto& points = lines[lineIt];
		for (size_t pointIt = 0; pointIt < points.size(); pointIt++)
		{
			float shift = pen - std::floor(pen);
			int x0 = 0;
			int y0 = 0;
			int x1 = 0;
			int y1 = 0;
			stbtt_GetCodepointBitmapBoxSubpixel(&font.info, points[pointIt], scale, scale, shift, 0.0F, &x0, &y0, &x1, &y1);
			int glyphWidth = x1 - x0;
			int glyphHeight = y1 - y0;
			if (glyphWidth > 0 && glyphHeight > 0)
			{
				glyph.assign(static_cast<size_t>(glyphWidth) * glyphHeight, 0);
				stbtt_MakeCodepointBitmapSubpixel(&font.info, glyph.data(), glyphWidth, glyphHeight, glyphWidth, scale, scale, shift, 0.0F, points[pointIt]);
				int left = static_cast<int>(std::floor(pen)) + x0;
				int top = static_cast<int>(baseline) + y0;
				for (int row = 0; row < glyphHeight; row++)
				{
					int canvasY = top + row;
					if (canvasY < 0 || canvasY >= canvas.height)
						continue;
					for (int column = 0; column < glyphWidth; column++)
					{
						int canvasX = left + column;
						if (canvasX < 0 || canvasX >= canvas.width)
							continue;
						float cover = static_cast<float>(glyph[(static_cast<size_t>(row) * glyphWidth) + column]) / 255.0F;
						for (size_t channelIt = 0; channelIt < 3; channelIt++)
						{
							uint8_t& value = canvas.bgr[((static_cast<size_t>(canvasY) * canvas.width + canvasX) * 3) + channelIt];
							value = static_cast<uint8_t>(std::lround((value * (1.0F - cover)) + (255.0F * cover)));
						}
					}
				}
			}
			pen += advance(points[pointIt], pointIt + 1 < points.size() ? points[pointIt + 1] : 0);
		}
	}
}

template <typename T>
[[nodiscard]] std::optional<T> ParseNumber(std::string_view text)
{
	T value{};
	auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size())
		return std::nullopt;
	return value;
}

int Usage()
{
	std::println(stderr, "usage: imagesheet size <image> | bmp <image> <out.bmp> | compose <width> <height> "
						 "[--font <file.ttf>] [--image <file> <x> <y> <w> <h>] [--text <x> <y> <w> <size> <text>] --out <file> [--quality <1-100>]");
	return 2;
}

} // namespace

int main(int argc, char* argv[])
{
	std::vector<std::string_view> args(argv + 1, argv + argc);
	if (args.empty())
		return Usage();
	if (args[0] == "size" && args.size() == 2)
	{
		auto image = LoadImage(args[1]);
		if (!image)
			return 2;
		std::println("{} {}", image->width, image->height);
		return 0;
	}
	if (args[0] == "bmp" && args.size() == 3)
	{
		auto image = LoadImage(args[1]);
		return image && capture::Save(args[2], *image) ? 0 : 2;
	}
	if (args[0] != "compose" || args.size() < 3)
		return Usage();
	auto width = ParseNumber<int>(args[1]);
	auto height = ParseNumber<int>(args[2]);
	if (!width || !height || *width <= 0 || *height <= 0)
		return Usage();
	capture::Image canvas{*width, *height, std::vector<uint8_t>(static_cast<size_t>(*width) * *height * 3, 0)};
	std::vector<std::filesystem::path> outs;
	std::unique_ptr<Font> font;
	int quality = 88;
	for (size_t argIt = 3; argIt < args.size();)
	{
		if (args[argIt] == "--image" && argIt + 5 < args.size())
		{
			auto x = ParseNumber<int>(args[argIt + 2]);
			auto y = ParseNumber<int>(args[argIt + 3]);
			auto w = ParseNumber<int>(args[argIt + 4]);
			auto h = ParseNumber<int>(args[argIt + 5]);
			if (!x || !y || !w || !h)
				return Usage();
			auto image = LoadImage(args[argIt + 1]);
			if (!image)
				return 2;
			DrawImage(canvas, *image, *x, *y, *w, *h);
			argIt += 6;
		}
		else if (args[argIt] == "--text" && argIt + 5 < args.size())
		{
			auto x = ParseNumber<float>(args[argIt + 1]);
			auto y = ParseNumber<float>(args[argIt + 2]);
			auto w = ParseNumber<float>(args[argIt + 3]);
			auto size = ParseNumber<float>(args[argIt + 4]);
			if (!x || !y || !w || !size)
				return Usage();
			if (font)
				DrawText(canvas, *font, *x, *y, *w, *size, args[argIt + 5]);
			else
				DrawSegmentText(canvas, *x, *y, *w, *size, args[argIt + 5]);
			argIt += 6;
		}
		else if (args[argIt] == "--font" && argIt + 1 < args.size())
		{
			font = LoadFont(args[argIt + 1]);
			if (!font)
				return 2;
			argIt += 2;
		}
		else if (args[argIt] == "--out" && argIt + 1 < args.size())
		{
			outs.emplace_back(args[argIt + 1]);
			argIt += 2;
		}
		else if (args[argIt] == "--quality" && argIt + 1 < args.size())
		{
			auto value = ParseNumber<int>(args[argIt + 1]);
			if (!value || *value < 1 || *value > 100)
				return Usage();
			quality = *value;
			argIt += 2;
		}
		else
			return Usage();
	}
	if (outs.empty())
		return Usage();
	for (const auto& out : outs)
		if (!SaveImage(out, canvas, quality))
			return 2;
	return 0;
}
