#pragma once

#include <rhi/buffer.h>
#include <rhi/deviceobject.h>
#include <rhi/enums.h>
#include <rhi/types.h>

#include <array>
#include <cstdint>
#include <span>
#include <variant>

namespace rhi
{

template <GraphicsApi G>
class Device;

template <GraphicsApi G>
class AccelerationStructure;

// what ray queries traverse (DeviceLimits::rayQuery): a bottom level structure holds triangles, a top level one instances of
// bottom level ones
enum class AccelerationStructureType : uint8_t
{
	kBottomLevel,
	kTopLevel,
};

// a bottom level structure's triangles, in buffers with BufferUsage::kAccelerationStructureInput and kDeviceAddress: their
// vertices from vertexAddress, a position of three floats at the start of each, vertexStride bytes apart, vertexCount of
// them; their indices (uint32) from indexAddress, three a triangle (0: none, the vertices three a triangle); and a
// transform placing them (3x4 floats, row major, at transformAddress; 0: none). an opaque geometry's triangles are hit
// wherever a ray meets them; another's are candidates, which the query's shader accepts or not
struct AccelerationStructureTriangles
{
	uint64_t vertexAddress = 0;
	uint32_t vertexStride = 0;
	uint32_t vertexCount = 0;
	uint64_t indexAddress = 0;
	uint64_t transformAddress = 0;
	uint32_t triangleCount = 0;
	bool opaque = true;
};

// a top level structure's instances (AccelerationStructureInstance each) in a buffer, as the triangles above
struct AccelerationStructureInstances
{
	uint64_t address = 0;
	uint32_t count = 0;
};

using AccelerationStructureGeometry = std::variant<AccelerationStructureTriangles, AccelerationStructureInstances>;

// an instance of a bottom level structure in a top level one, as the device reads it (Vulkan's
// VkAccelerationStructureInstanceKHR): the transform's rows (3x4), the index the shader reads (24 bits, its
// CommittedInstanceID) and the mask a query's must share a bit with (8), and the structure's address
// (AccelerationStructure::GetDeviceAddress; 0: none, the instance is inactive)
struct AccelerationStructureInstance
{
	std::array<float, 12> transform{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
	uint32_t customIndexAndMask = 0; // the index in the low 24 bits, the mask in the top 8
	uint32_t shaderBindingTableOffsetAndFlags = 0;
	uint64_t structureAddress = 0;
};
static_assert(sizeof(AccelerationStructureInstance) == 64);

enum class AccelerationStructureBuildFlags : uint8_t
{
	kNone = 0,
	kPreferFastTrace = 1U << 0U,
	kPreferFastBuild = 1U << 1U,
	kAllowUpdate = 1U << 2U, // later builds may update it (refit it to moved triangles of the same count)
	kAllowCompaction = 1U << 3U, // its compacted size may be queried, and it copied into one of that size (see CommandEncoder)
};
RHI_FLAGS(AccelerationStructureBuildFlags)

// what a structure of some geometries takes: its own storage, and a build's and an update's scratch memory
struct AccelerationStructureSizes
{
	uint64_t structureSize = 0;
	uint64_t buildScratchSize = 0;
	uint64_t updateScratchSize = 0;
};

template <GraphicsApi G>
struct AccelerationStructureCreateDesc final : DeviceObjectCreateDesc<G>
{
	AccelerationStructureType type{};
	uint64_t size = 0; // AccelerationStructureSizes::structureSize, of what it will be built of
};

template <GraphicsApi G>
struct ObjectTraits<AccelerationStructure<G>>
{
	using CreateDescType = AccelerationStructureCreateDesc<G>;
};

// an acceleration structure (see AccelerationStructureType), in a device local buffer of its own; built and updated by
// commands (CommandEncoder::BuildAccelerationStructure), bound to a ray query's shader as its handle
template <GraphicsApi G>
class AccelerationStructure final : public DeviceObject<AccelerationStructure<G>>
{
public:
	using SuperType = DeviceObject<AccelerationStructure<G>>;
	using CreateDescType = ObjectTraits<AccelerationStructure<G>>::CreateDescType;

	constexpr AccelerationStructure() noexcept = default;
	explicit AccelerationStructure(CreateDescType&& desc);
	AccelerationStructure(AccelerationStructure&& other) noexcept;
	~AccelerationStructure();

	[[maybe_unused]] AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myHandle; } //NOLINT(google-explicit-constructor)

	void Swap(AccelerationStructure& rhs) noexcept;
	friend void Swap(AccelerationStructure& lhs, AccelerationStructure& rhs) noexcept { lhs.Swap(rhs); }

	// what a top level instance names it by (AccelerationStructureInstance::structureAddress)
	[[nodiscard]] uint64_t GetDeviceAddress() const noexcept { return myAddress; }

	// what a structure of type built (flags) of geometries takes, all triangles (bottom level) or one set of instances (top)
	[[nodiscard]] static AccelerationStructureSizes GetSizes(
		const Device<G>& device,
		AccelerationStructureType type,
		std::span<const AccelerationStructureGeometry> geometries,
		AccelerationStructureBuildFlags flags);

private:
	Buffer<G> myBuffer;
	AccelerationStructureHandle<G> myHandle{};
	uint64_t myAddress = 0;
};

} // namespace rhi
