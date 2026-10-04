#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

// rhi's backend neutral vocabulary: what the code above rhi (gfx) uses to describe resources and work, converted to
// the backend's own types inside the backend (see rhi/vulkan/convert.h)

// bitwise operators for a flags enum (one whose enumerators are bits)
#define RHI_FLAGS(E) \
	[[nodiscard]] constexpr E operator|(E lhs, E rhs) noexcept \
	{ \
		return static_cast<E>(static_cast<std::underlying_type_t<E>>(lhs) | static_cast<std::underlying_type_t<E>>(rhs)); \
	} \
	[[nodiscard]] constexpr E operator&(E lhs, E rhs) noexcept \
	{ \
		return static_cast<E>(static_cast<std::underlying_type_t<E>>(lhs) & static_cast<std::underlying_type_t<E>>(rhs)); \
	} \
	constexpr E& operator|=(E& lhs, E rhs) noexcept { return lhs = lhs | rhs; } \
	[[nodiscard]] constexpr bool Any(E flags) noexcept { return static_cast<std::underlying_type_t<E>>(flags) != 0; }

namespace rhi
{

enum class Format : uint8_t
{
	kUndefined,
	kR8G8B8A8Unorm,
	kR8G8B8A8Srgb,
	kB8G8R8A8Unorm,
	kB8G8R8A8Srgb,
	kR8G8B8Unorm,
	kB8G8R8Unorm,
	kA2R10G10B10UnormPack32,
	kA2B10G10R10UnormPack32,
	kR16G16B16A16Sfloat,
	kD32Sfloat,
	kD32SfloatS8Uint,
	kD24UnormS8Uint,
	kBC1RgbUnorm,
	kBC1RgbSrgb,
	kBC3Unorm,
	kBC3Srgb,
	kBC4Unorm,
	kBC5Unorm,
};

enum class ImageLayout : uint8_t
{
	kUndefined,
	kGeneral,
	kColorAttachment,
	kDepthStencilAttachment,
	kShaderReadOnly,
	kTransferSource,
	kTransferDestination,
	kPresent,
};

enum class ImageTiling : uint8_t
{
	kOptimal,
	kLinear,
};

enum class ImageAspect : uint8_t
{
	kNone = 0,
	kColor = 1U << 0U,
	kDepth = 1U << 1U,
	kStencil = 1U << 2U,
};
RHI_FLAGS(ImageAspect)

enum class ImageUsage : uint8_t
{
	kNone = 0,
	kTransferSource = 1U << 0U,
	kTransferDestination = 1U << 1U,
	kSampled = 1U << 2U,
	kStorage = 1U << 3U,
	kColorAttachment = 1U << 4U,
	kDepthStencilAttachment = 1U << 5U,
};
RHI_FLAGS(ImageUsage)

enum class BufferUsage : uint8_t
{
	kNone = 0,
	kTransferSource = 1U << 0U,
	kTransferDestination = 1U << 1U,
	kUniform = 1U << 2U,
	kStorage = 1U << 3U,
	kIndex = 1U << 4U,
	kVertex = 1U << 5U,
};
RHI_FLAGS(BufferUsage)

enum class MemoryProperty : uint8_t
{
	kNone = 0,
	kDeviceLocal = 1U << 0U,
	kHostVisible = 1U << 1U,
	kHostCoherent = 1U << 2U,
};
RHI_FLAGS(MemoryProperty)

enum class FormatFeature : uint8_t
{
	kNone = 0,
	kSampled = 1U << 0U,
	kColorAttachment = 1U << 1U,
	kDepthStencilAttachment = 1U << 2U,
	kTransferSource = 1U << 3U,
	kTransferDestination = 1U << 4U,
};
RHI_FLAGS(FormatFeature)

enum class LoadOp : uint8_t
{
	kLoad,
	kClear,
	kDontCare,
};

enum class StoreOp : uint8_t
{
	kStore,
	kDontCare,
};

// how a render target's commands are recorded: in the command buffer that begins it, or in secondary command buffers
enum class SubpassContents : uint8_t
{
	kInline,
	kSecondaryCommandBuffers,
};

enum class PipelineBindPoint : uint8_t
{
	kGraphics,
	kCompute,
};

enum class PipelineStage : uint16_t
{
	kNone = 0,
	kTopOfPipe = 1U << 0U,
	kTransfer = 1U << 1U,
	kVertexShader = 1U << 2U,
	kFragmentShader = 1U << 3U,
	kComputeShader = 1U << 4U,
	kColorAttachmentOutput = 1U << 5U,
	kAllGraphics = 1U << 6U,
	kAllCommands = 1U << 7U,
	kBottomOfPipe = 1U << 8U,
};
RHI_FLAGS(PipelineStage)

enum class Access : uint8_t
{
	kNone = 0,
	kShaderRead = 1U << 0U,
	kShaderWrite = 1U << 1U,
	kTransferRead = 1U << 2U,
	kTransferWrite = 1U << 3U,
	kHostWrite = 1U << 4U,
};
RHI_FLAGS(Access)

enum class IndexType : uint8_t
{
	kUint16,
	kUint32,
};

enum class SemaphoreType : uint8_t
{
	kBinary,
	kTimeline,
};

enum class Filter : uint8_t
{
	kNearest,
	kLinear,
};

enum class AddressMode : uint8_t
{
	kRepeat,
	kMirroredRepeat,
	kClampToEdge,
	kClampToBorder,
};

struct SamplerDesc
{
	Filter magFilter = Filter::kLinear;
	Filter minFilter = Filter::kLinear;
	Filter mipmapFilter = Filter::kLinear;
	AddressMode addressModeU = AddressMode::kRepeat;
	AddressMode addressModeV = AddressMode::kRepeat;
	AddressMode addressModeW = AddressMode::kRepeat;
	float mipLodBias = 0.0F;
	float maxAnisotropy = 1.0F; // anisotropic filtering if above 1
	float minLod = 0.0F;
	float maxLod = 1000.0F;
};

struct Extent2d
{
	uint32_t width = 0;
	uint32_t height = 0;

	[[nodiscard]] bool operator==(const Extent2d&) const = default;
};

struct Viewport
{
	float x = 0.0F;
	float y = 0.0F;
	float width = 0.0F;
	float height = 0.0F;
	float minDepth = 0.0F;
	float maxDepth = 1.0F;
};

struct Rect
{
	int32_t x = 0;
	int32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

// a color (rgba) or a depth and stencil value, by what the cleared image holds
struct ClearValue
{
	std::array<float, 4> color{};
	float depth = 1.0F;
	uint32_t stencil = 0;
};

// the device limits the code above rhi needs
struct DeviceLimits
{
	uint64_t maxStorageBufferRange = 0; // in bytes
	uint32_t maxPerStageSampledImages = 0;
};

// what presenting a frame says about the swapchain
enum class PresentResult : uint8_t
{
	kSuccess,
	kSuboptimal, // presented, but the swapchain no longer matches the surface exactly
	kOutOfDate, // not presented: the swapchain must be recreated
	kError,
};

} // namespace rhi
