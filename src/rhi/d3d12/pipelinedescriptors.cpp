// the D3D12 backend's descriptor sets in the pipeline: their state per layout, written into a fresh set's ranges of the
// shader visible heaps when their values changed (or, for a pushed set, into ranges the command buffer takes at each
// bind, freed when its commands are done), and bound for the next dispatch or draw (CommandBufferObject::PrepareDispatch).
// an image view's or a sampler's descriptor is copied from its own (the cpu heaps'); a buffer's is made in place, its
// view the binding's (a constant buffer, a structured buffer of the code's stride, or a raw one)
#include <rhi/d3d12/backend.h>

#include <rhi/pipeline.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <cstring>
#include <variant>

namespace rhi
{

namespace pipeline
{

// a buffer's view at dst, as its binding takes it
static void WriteBufferDescriptor(
	ID3D12Device* device, const d3d12::DescriptorSetLayoutObject::Binding& binding, const DescriptorBufferInfo<kD3D12>& buffer, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
	ENSUREF(buffer.buffer != nullptr, "binding {} has no buffer", binding.binding);
	const uint64_t size = buffer.buffer->GetDesc().Width;
	ENSUREF(buffer.offset <= size, "binding {}'s offset {} is past its buffer's {} bytes", binding.binding, buffer.offset, size);
	const uint64_t range = buffer.range == kWholeSize ? size - buffer.offset : buffer.range;
	if (binding.type == DescriptorType::kUniformBuffer)
	{
		ENSUREF(
			buffer.offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0,
			"binding {}: a constant buffer's offset {} isn't a multiple of 256 bytes",
			binding.binding,
			buffer.offset);
		const D3D12_CONSTANT_BUFFER_VIEW_DESC desc{
			.BufferLocation = buffer.buffer->GetGPUVirtualAddress() + buffer.offset,
			.SizeInBytes = static_cast<UINT>((range + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t{D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1})};
		device->CreateConstantBufferView(&desc, dst);
		return;
	}
	ENSUREF(binding.type == DescriptorType::kStorageBuffer, "binding {}: a buffer bound to a binding of type {}", binding.binding, std::to_underlying(binding.type));
	// a structured buffer of the code's element (its stride), else a raw one of 32 bit values
	const uint32_t stride = binding.stride != 0 ? binding.stride : 4;
	ENSUREF(
		buffer.offset % stride == 0, "binding {}: a buffer's offset {} isn't a multiple of its elements' {} bytes", binding.binding, buffer.offset, stride);
	const auto first = buffer.offset / stride;
	const auto count = static_cast<UINT>(range / stride);
	if (binding.writable)
	{
		const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
			.Format = binding.stride != 0 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = first,
				.NumElements = count,
				.StructureByteStride = binding.stride,
				.CounterOffsetInBytes = 0,
				.Flags = binding.stride != 0 ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW}};
		device->CreateUnorderedAccessView(buffer.buffer, nullptr, &desc, dst);
	}
	else
	{
		const D3D12_SHADER_RESOURCE_VIEW_DESC desc{
			.Format = binding.stride != 0 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.Buffer = {
				.FirstElement = first,
				.NumElements = count,
				.StructureByteStride = binding.stride,
				.Flags = binding.stride != 0 ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW}};
		device->CreateShaderResourceView(buffer.buffer, &desc, dst);
	}
}

// a set's values written into its resources' table (an offset in the shader visible heap), and its samplers into
// samplers (their descriptions, the table's content: DescriptorPoolObject::AcquireSamplerTable), by the template's
// entries (see DescriptorSetLayoutObject)
static void WriteDescriptorSet(
	d3d12::AllocatorObject& device,
	const d3d12::DescriptorSetLayoutObject& layout,
	const BindingsData<kD3D12>& bindingsData,
	const std::vector<DescriptorUpdateTemplateEntry<kD3D12>>& entries,
	uint32_t resources,
	std::vector<D3D12_SAMPLER_DESC>& samplers)
{
	samplers.assign(layout.samplerCount, D3D12_SAMPLER_DESC{});
	ZoneScopedN("Pipeline::WriteDescriptorSet");

	for (const auto& entry : entries)
	{
		const auto& binding = layout.Find(entry.dstBinding);
		ENSUREF(
			entry.dstArrayElement + entry.descriptorCount <= binding.count,
			"binding {}'s elements {} to {} are past its count {}",
			entry.dstBinding,
			entry.dstArrayElement,
			entry.dstArrayElement + entry.descriptorCount,
			binding.count);
		for (uint32_t elementIt = 0; elementIt < entry.descriptorCount; elementIt++)
		{
			const auto& value = bindingsData[(entry.offset + (elementIt * entry.stride)) / sizeof(BindingVariant<kD3D12>)];
			const uint32_t index = binding.offset + entry.dstArrayElement + elementIt;
			if (const auto* buffer = std::get_if<DescriptorBufferInfo<kD3D12>>(&value))
			{
				WriteBufferDescriptor(device.device, binding, *buffer, device.pool.resources.Cpu(resources + index));
			}
			else if (const auto* image = std::get_if<DescriptorImageInfo<kD3D12>>(&value))
			{
				if (binding.sampler)
				{
					ENSUREF(image->sampler != nullptr, "binding {} has no sampler", entry.dstBinding);
					samplers.at(index) = image->sampler->desc;
					continue;
				}
				ENSUREF(image->imageView != nullptr, "binding {} has no image view", entry.dstBinding);
				const auto source =
					binding.type == DescriptorType::kStorageImage ? image->imageView->unorderedAccess : image->imageView->shaderResource;
				ENSUREF(
					source.ptr != 0,
					"binding {} element {}: {}'s view has no {} view (its image's usage)",
					entry.dstBinding,
					entry.dstArrayElement + elementIt,
					d3d12::GetName(image->imageView->image->resource),
					binding.type == DescriptorType::kStorageImage ? "unordered access" : "shader resource");
				device.device->CopyDescriptorsSimple(1, device.pool.resources.Cpu(resources + index), source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			else if (const auto* structure = std::get_if<AccelerationStructureHandle<kD3D12>>(&value))
			{
				// a shader resource view of its address (no resource: D3D12's for an acceleration structure)
				ENSUREF(*structure != nullptr, "binding {} has no acceleration structure", entry.dstBinding);
				const D3D12_SHADER_RESOURCE_VIEW_DESC desc{
					.Format = DXGI_FORMAT_UNKNOWN,
					.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
					.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
					.RaytracingAccelerationStructure = {.Location = (*structure)->address}};
				device.device->CreateShaderResourceView(nullptr, &desc, device.pool.resources.Cpu(resources + index));
			}
			else
			{
				ENSUREF(false, "the D3D12 backend's sets hold no texel buffers or inline uniform blocks (binding {})", entry.dstBinding);
			}
		}
	}
}

} // namespace pipeline

template <>
void Pipeline<kD3D12>::InternalPrepareDescriptorSets()
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;

	for (const auto& [set, setLayout] : layout.GetDescriptorSetLayouts())
	{
		auto* setLayoutHandle = static_cast<DescriptorSetLayoutHandle<kD3D12>>(setLayout);
		if (myDescriptorMap.contains(setLayoutHandle))
			continue;

		const bool push = (setLayout.GetDesc().flags & d3d12::kDescriptorSetLayoutPush) != 0U;
		auto [insertIt, insertResult] = myDescriptorMap.emplace(
			setLayoutHandle,
			std::make_tuple(
				core::UpgradableSharedMutex{},
				DescriptorSetStatus::kReady,
				BindingsMap<kD3D12>{},
				BindingsData<kD3D12>{},
				DescriptorUpdateTemplate<kD3D12>{DescriptorUpdateTemplateCreateDesc<kD3D12>{
					GetDevice().CreateDeviceObjectCreateDesc("DescriptorUpdateTemplate"),
					push ? d3d12::DescriptorUpdateTemplateType::kPushDescriptors : d3d12::DescriptorUpdateTemplateType::kDescriptorSet,
					setLayoutHandle,
					myBindPoint,
					static_cast<PipelineLayoutHandle<kD3D12>>(layout),
					set,
				}},
				push ? std::nullopt : std::make_optional(DescriptorSetArrayList<kD3D12>{})));
		ENSURE(insertResult);
		auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = insertIt->second;

		if (setOptionalArrayList)
			setOptionalArrayList->emplace_front(
				DescriptorSetArray<kD3D12>(
					DescriptorSetArrayCreateDesc<kD3D12>{
						GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"),
						myDescriptorPool,
						myDescriptorPoolMutex,
					},
					setLayout),
				0);
	}
}

template <>
void Pipeline<kD3D12>::InternalResetDescriptorPool()
{
	// no pool: every set takes its ranges from the device's shader visible heaps (see descriptorset.cpp)
}

template <>
void Pipeline<kD3D12>::InternalUpdateDescriptorSet(
	const DescriptorSetLayout<kD3D12>& setLayout,
	const BindingsData<kD3D12>& bindingsData,
	const DescriptorUpdateTemplate<kD3D12>& setTemplate,
	DescriptorSetArrayList<kD3D12>& setArrayList) const
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSet");

	// a fresh set (one the gpu may read is never written again), from a new array when the front one is used up
	bool frontArrayIsFull = !setArrayList.empty() && std::get<1>(setArrayList.front()) == (DescriptorSetArray<kD3D12>::Capacity() - 1);
	if (setArrayList.empty() || frontArrayIsFull)
		setArrayList.emplace_front(std::make_tuple(
			DescriptorSetArray<kD3D12>(
				DescriptorSetArrayCreateDesc<kD3D12>{
					GetDevice().CreateDeviceObjectCreateDesc("DescriptorSetArray"), myDescriptorPool, myDescriptorPoolMutex},
				setLayout),
			~0));

	auto& [setArray, setIndex] = setArrayList.front();
	++setIndex;
	ENSURE(setIndex < setArray.Capacity());
	auto* set = setArray[setIndex];

	// its samplers' table the one of theirs (shared), its last one's given back
	auto& device = d3d12::AllocatorObject::ForDevice(GetDesc().device);
	std::vector<D3D12_SAMPLER_DESC> samplers;
	pipeline::WriteDescriptorSet(
		device, *static_cast<DescriptorSetLayoutHandle<kD3D12>>(setLayout), bindingsData, setTemplate.GetEntries(), set->resources, samplers);
	const uint32_t table = device.pool.AcquireSamplerTable(device.device, samplers);
	device.pool.ReleaseSamplerTable(set->samplers);
	set->samplers = table;
}

template <>
void Pipeline<kD3D12>::InternalPushDescriptorSet(
	CommandBufferHandle<kD3D12> cmd,
	PipelineLayoutHandle<kD3D12> /*layoutHandle*/,
	const BindingsData<kD3D12>& bindingsData,
	const DescriptorUpdateTemplate<kD3D12>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalPushDescriptorSet");

	// the set's resources in a range the command buffer takes, and its samplers' table (shared): given back when its
	// commands are done (see FreeTransients)
	const auto& layout = *setTemplate.GetDesc().descriptorSetLayout;
	auto& pool = cmd->device->pool;
	const uint32_t resources = pool.resources.Allocate(layout.resourceCount);
	if (resources != d3d12::GpuDescriptorHeap::kNone)
		cmd->pushedResources.emplace_back(resources, layout.resourceCount);
	std::vector<D3D12_SAMPLER_DESC> samplerContent;
	pipeline::WriteDescriptorSet(*cmd->device, layout, bindingsData, setTemplate.GetEntries(), resources, samplerContent);
	const uint32_t samplers = pool.AcquireSamplerTable(cmd->device->device, samplerContent);
	if (samplers != d3d12::GpuDescriptorHeap::kNone)
		cmd->pushedSamplers.push_back(samplers);

	const uint32_t set = setTemplate.GetDesc().set;
	if (cmd->sets.size() <= set)
		cmd->sets.resize(set + 1);
	cmd->sets[set] = {
		.resources = resources != d3d12::GpuDescriptorHeap::kNone ? pool.resources.Gpu(resources) : D3D12_GPU_DESCRIPTOR_HANDLE{},
		.samplers = samplers != d3d12::GpuDescriptorHeap::kNone ? pool.samplers.Gpu(samplers) : D3D12_GPU_DESCRIPTOR_HANDLE{}};
}

template <>
void Pipeline<kD3D12>::InternalUpdateDescriptorSetTemplate(
	const BindingsMap<kD3D12>& bindingsMap, DescriptorUpdateTemplate<kD3D12>& setTemplate)
{
	ZoneScopedN("Pipeline::InternalUpdateDescriptorSetTemplate");

	std::vector<DescriptorUpdateTemplateEntry<kD3D12>> entries;
	entries.reserve(bindingsMap.size());

	for (const auto& [index, binding] : bindingsMap)
	{
		const auto& [offset, count, type, ranges] = binding;

		uint32_t rangeOffset = 0;
		for (const auto& [low, high] : ranges)
		{
			entries.push_back(DescriptorUpdateTemplateEntry<kD3D12>{
				.dstBinding = index,
				.dstArrayElement = low,
				.descriptorCount = high - low,
				.descriptorType = type,
				.offset = (offset + rangeOffset) * sizeof(BindingVariant<kD3D12>),
				.stride = sizeof(BindingVariant<kD3D12>)});
			rangeOffset += high - low;
		}
	}

	setTemplate.SetEntries(std::move(entries));
}

template <>
void Pipeline<kD3D12>::BindDescriptorSet(
	CommandBufferHandle<kD3D12> cmd,
	DescriptorSetHandle<kD3D12> handle,
	PipelineBindPoint /*bindPoint*/,
	PipelineLayoutHandle<kD3D12> /*layoutHandle*/,
	uint32_t set,
	std::optional<uint32_t> bufferOffset) const
{
	ZoneScopedN("Pipeline::BindDescriptorSet");

	ENSUREF(!bufferOffset, "the D3D12 backend's sets take no dynamic offsets yet");
	const auto& pool = cmd->device->pool;
	if (cmd->sets.size() <= set)
		cmd->sets.resize(set + 1);
	cmd->sets[set] = {
		.resources = handle->resources != d3d12::GpuDescriptorHeap::kNone ? pool.resources.Gpu(handle->resources) : D3D12_GPU_DESCRIPTOR_HANDLE{},
		.samplers = handle->samplers != d3d12::GpuDescriptorHeap::kNone ? pool.samplers.Gpu(handle->samplers) : D3D12_GPU_DESCRIPTOR_HANDLE{}};
}

// (InvalidateDescriptorSets, TakeDescriptorSets and BindDescriptorSetAuto are every backend's, in pipeline.inl)

} // namespace rhi
