#include "environmentkernelsbridge.h"

#include <slang-cpp-prelude.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <numeric>
#include <optional>

// the kernels (environmentkernels.generated.cpp): each runs its groups from varyingInput's start to its end
extern "C"
{
void EnvironmentSky(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
void EnvironmentDownsample(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
void EnvironmentCopy(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
void EnvironmentSpecular(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
void EnvironmentSheen(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
void EnvironmentIrradiance(ComputeVaryingInput* varyingInput, void* entryPointParams, void* globalParams);
}

// the prelude only declares it: the kernels pass it back to the textures, whose one sampler it is (linear, around in u,
// clamped in v, as the gpu's)
struct ISamplerState
{
};

namespace environmentkernels::bridge
{

namespace detail
{

using Texel = Vector<float, 4>;

// an image of float texels with its mips (each half the one before, at least minimum wide)
struct CpuImage
{
	struct Level
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<Texel> texels;
	};
	std::vector<Level> levels;

	CpuImage(uint32_t width, uint32_t levelCount, uint32_t minimum)
	{
		for (uint32_t level = 0; level < levelCount; level++)
		{
			uint32_t levelWidth = std::max(width >> level, minimum);
			uint32_t levelHeight = std::max(levelWidth / 2, 1U);
			levels.push_back({levelWidth, levelHeight, std::vector<Texel>(static_cast<size_t>(levelWidth) * levelHeight)});
		}
	}

	CpuImage(uint32_t width, const float* rgb)
		: CpuImage(width, 1, 1)
	{
		auto& texels = levels.front().texels;
		for (size_t texel = 0; texel < texels.size(); texel++)
			texels[texel] = Texel{rgb[texel * 3], rgb[(texel * 3) + 1], rgb[(texel * 3) + 2], 1.0F};
	}
};

// a view of some of an image's mips, as a kernel sees a texture: loaded by texel, or sampled as the gpu's sampler does
class CpuView final : public IRWTexture
{
public:
	CpuView(CpuImage& image, uint32_t base, uint32_t count)
		: myImage(image)
		, myBase(base)
		, myCount(count)
	{}

	TextureDimensions GetDimensions(int mipLevel) override
	{
		const auto& level = Level(mipLevel < 0 ? 0 : static_cast<uint32_t>(mipLevel));
		TextureDimensions dimensions;
		dimensions.reset();
		dimensions.shape = SLANG_TEXTURE_2D;
		dimensions.width = level.width;
		dimensions.height = level.height;
		dimensions.numberOfLevels = myCount;
		return dimensions;
	}

	void Load(const int32_t* v, void* outData, size_t dataSize) override
	{
		const auto& level = Level(static_cast<uint32_t>(v[2]));
		auto x = std::clamp<int32_t>(v[0], 0, static_cast<int32_t>(level.width) - 1);
		auto y = std::clamp<int32_t>(v[1], 0, static_cast<int32_t>(level.height) - 1);
		std::memcpy(outData, &level.texels[(static_cast<size_t>(y) * level.width) + x], std::min(dataSize, sizeof(Texel)));
	}

	void Sample(SamplerState samplerState, const float* loc, void* outData, size_t dataSize) override
	{
		SampleLevel(samplerState, loc, 0.0F, outData, dataSize);
	}

	// trilinear: bilinear in each of the two mips around level (around in u, clamped in v), mixed
	void SampleLevel(SamplerState /*samplerState*/, const float* loc, float level, void* outData, size_t dataSize) override
	{
		level = std::clamp(level, 0.0F, static_cast<float>(myCount - 1));
		auto below = static_cast<uint32_t>(std::floor(level));
		float fraction = level - static_cast<float>(below);
		auto color = Bilinear(below, loc[0], loc[1]);
		if (fraction > 0.0F && below + 1 < myCount)
		{
			auto above = Bilinear(below + 1, loc[0], loc[1]);
			for (int c = 0; c < 4; c++)
				color[c] += (above[c] - color[c]) * fraction;
		}
		Texel texel{color[0], color[1], color[2], color[3]};
		std::memcpy(outData, &texel, std::min(dataSize, sizeof(Texel)));
	}

	void* refAt(const uint32_t* loc) override
	{
		auto& level = Level(0);
		return &level.texels[(static_cast<size_t>(loc[1]) * level.width) + loc[0]];
	}

private:
	CpuImage::Level& Level(uint32_t mip) { return myImage.levels[myBase + std::min(mip, myCount - 1)]; }

	std::array<float, 4> Bilinear(uint32_t mip, float u, float v)
	{
		const auto& level = Level(mip);
		float x = (u * static_cast<float>(level.width)) - 0.5F;
		float y = std::clamp((v * static_cast<float>(level.height)) - 0.5F, 0.0F, static_cast<float>(level.height - 1));
		auto x0 = static_cast<int64_t>(std::floor(x));
		auto y0 = static_cast<int64_t>(std::floor(y));
		float fx = x - static_cast<float>(x0);
		float fy = y - static_cast<float>(y0);
		auto w = static_cast<int64_t>(level.width);
		auto texel = [&level, w](int64_t tx, int64_t ty)
		{
			tx = ((tx % w) + w) % w;
			ty = std::clamp<int64_t>(ty, 0, level.height - 1);
			return level.texels[static_cast<size_t>((ty * w) + tx)];
		};
		auto a = texel(x0, y0);
		auto b = texel(x0 + 1, y0);
		auto c = texel(x0, y0 + 1);
		auto d = texel(x0 + 1, y0 + 1);
		std::array<float, 4> out{};
		for (int i = 0; i < 4; i++)
		{
			float top = a[i] + ((b[i] - a[i]) * fx);
			float bottom = c[i] + ((d[i] - c[i]) * fx);
			out[i] = top + ((bottom - top) * fy);
		}
		return out;
	}

	CpuImage& myImage;
	uint32_t myBase;
	uint32_t myCount;
};

// the kernels' global parameters: as environment.slang declares them, in its order
struct Globals
{
	Texture2D<Texel> source;
	SamplerState sampler;
	Texture2D<Texel> input;
	RWTexture2D<Texel> output;
	RWStructuredBuffer<EnvironmentData> environment;
	EnvironmentFilterConstants* filter;
};

using Kernel = void (*)(ComputeVaryingInput*, void*, void*);

// a kernel of 8x8 threads per group over an output level: the rows of groups spread over the cpu's threads (libc++ runs
// std::execution::par serially)
void Run(Kernel kernel, Globals globals, uint32_t width, uint32_t height)
{
	constexpr uint32_t kGroupSize = 8;
	uint32_t groupsX = (width + kGroupSize - 1) / kGroupSize;
	uint32_t groupsY = (height + kGroupSize - 1) / kGroupSize;
	std::atomic_uint32_t nextRow = 0;
	auto work = [&]
	{
		for (uint32_t row = nextRow++; row < groupsY; row = nextRow++)
		{
			ComputeVaryingInput input{};
			input.startGroupID = {0, row, 0};
			input.endGroupID = {groupsX, row + 1, 1};
			auto rowGlobals = globals;
			kernel(&input, nullptr, &rowGlobals);
		}
	};
	std::vector<std::thread> threads(std::min<uint32_t>(std::max(std::thread::hardware_concurrency(), 1U), groupsY));
	for (auto& thread : threads)
		thread = std::thread(work);
	for (auto& thread : threads)
		thread.join();
}

} // namespace detail

Output Prefilter(const Input& input, uint32_t levelCount, EnvironmentData& environment)
{
	using namespace detail;

	ISamplerState samplerState;

	// as EnvironmentFilter::Record: the source's pyramid, down to 8 wide, the levels and the sheen levels
	uint32_t width = input.lighting != nullptr ? input.width : input.sourceWidth;
	uint32_t sourceLevelCount = 1;
	while ((width >> sourceLevelCount) >= 8)
		sourceLevelCount++;
	CpuImage sourceImage(width, sourceLevelCount, 8);
	CpuImage levels(width, levelCount, 2);
	CpuImage sheenLevels(width / 8, levelCount, 2);
	std::optional<CpuImage> original;
	std::optional<CpuImage> lighting;
	if (input.lighting != nullptr)
	{
		original.emplace(input.width, input.original);
		lighting.emplace(input.width, input.lighting);
	}

	EnvironmentFilterConstants constants{.roughness = 0.0F, .sourceLevelCount = sourceLevelCount};
	CpuView sourceView(sourceImage, 0, sourceLevelCount);
	Globals globals{};
	globals.sampler.state = &samplerState;
	globals.environment = {&environment, 1};
	globals.filter = &constants;
	auto dispatch = [&](Kernel kernel, CpuView* in, CpuImage& image, uint32_t level, float roughness)
	{
		CpuView output(image, level, 1);
		globals.input.texture = in;
		globals.output.texture = &output;
		constants.roughness = roughness;
		const auto& extent = image.levels[level];
		Run(kernel, globals, extent.width, extent.height);
	};

	// the source's level 0 and its pyramid
	std::optional<CpuView> lightingView;
	if (lighting)
	{
		lightingView.emplace(*lighting, 0, 1);
		dispatch(EnvironmentCopy, &*lightingView, sourceImage, 0, 0.0F);
	}
	else
		dispatch(EnvironmentSky, nullptr, sourceImage, 0, 0.0F);
	for (uint32_t level = 1; level < sourceLevelCount; level++)
	{
		CpuView above(sourceImage, level - 1, 1);
		dispatch(EnvironmentDownsample, &above, sourceImage, level, 0.0F);
	}

	// the levels
	globals.source.texture = &sourceView;
	auto roughnessOf = [levelCount](uint32_t level) { return static_cast<float>(level) / static_cast<float>(levelCount - 1); };
	std::optional<CpuView> originalView;
	CpuView sourceLevel0(sourceImage, 0, 1);
	if (original)
		originalView.emplace(*original, 0, 1);
	dispatch(EnvironmentCopy, originalView ? &*originalView : &sourceLevel0, levels, 0, 0.0F);
	for (uint32_t level = 1; level < levelCount; level++)
		dispatch(EnvironmentSpecular, nullptr, levels, level, roughnessOf(level));
	for (uint32_t level = 0; level < levelCount; level++)
		dispatch(EnvironmentSheen, nullptr, sheenLevels, level, roughnessOf(level));

	// the irradiance: one group, from the first mip at most 128 wide
	uint32_t irradianceLevel = 0;
	while (irradianceLevel + 1 < sourceLevelCount && sourceImage.levels[irradianceLevel].width > 128)
		irradianceLevel++;
	CpuView irradianceInput(sourceImage, irradianceLevel, 1);
	globals.input.texture = &irradianceInput;
	ComputeVaryingInput one{};
	one.endGroupID = {1, 1, 1};
	EnvironmentIrradiance(&one, nullptr, &globals);

	Output output;
	for (auto [image, out] : {std::pair{&levels, &output.levels}, std::pair{&sheenLevels, &output.sheenLevels}})
		for (const auto& level : image->levels)
		{
			Level copy{level.width, level.height, std::vector<std::array<float, 4>>(level.texels.size())};
			for (size_t texel = 0; texel < level.texels.size(); texel++)
				for (int c = 0; c < 4; c++)
					copy.texels[texel][static_cast<size_t>(c)] = level.texels[texel][c];
			out->push_back(std::move(copy));
		}
	return output;
}

} // namespace environmentkernels::bridge
