// the D3D12 backend's device: an ID3D12Device10 of the instance's adapter, checked for what the backend relies on (shader
// model 6.8, enhanced barriers, resource binding tier 3: each failing hard, named), its state behind the allocator handle
// (D3D12MA, the host visible pool, the descriptor heaps, see backend.h), three queue families (direct, compute, copy:
// D3D12's queue types, as dedicated as Vulkan's backend takes them), and the pipeline
#include <rhi/d3d12/backend.h>

#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/rhi.h>

#include <core/application.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Device<kD3D12>);

namespace device
{

// a capacity of D3D12's: the shader visible heaps' (resource binding tier 3's resources, the sampler heap's limit)
constexpr uint32_t kShaderVisibleResourceCount = 1'000'000;
constexpr uint32_t kShaderVisibleSamplerCount = 2048;

static std::string AdapterName(IDXGIAdapter4* adapter)
{
	DXGI_ADAPTER_DESC3 desc{};
	adapter->GetDesc3(&desc);
	const int count = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
	std::string name(static_cast<size_t>(std::max(count, 1)) - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), count, nullptr, nullptr);
	return name;
}

// the debug layer's messages: printed, an error or corruption as a validation error (assettest's -Validate fails a client
// on its "Validation Error" lines, as on Vulkan's layer's), which a debugger attached breaks on
static std::atomic<uint32_t> gValidationErrors{0};

static void CALLBACK OnMessage(
	D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/)
{
	if (severity == D3D12_MESSAGE_SEVERITY_INFO || severity == D3D12_MESSAGE_SEVERITY_MESSAGE)
		return;
	// advice, not errors: a clear of an image made without an optimized clear value (rhi's images don't know theirs when
	// they're made)
	if (id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE || id == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
		return;
	// and these: a pipeline state whose pixel shader writes a color target it has none of (a depth only pass's, the
	// writes discarded, as Vulkan's), and a list of barriers alone whose accesses could be none (a hint for drivers)
	if (id == D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET ||
		id == D3D12_MESSAGE_ID_NON_OPTIMAL_BARRIER_ONLY_EXECUTE_COMMAND_LISTS)
		return;
	const bool error = severity == D3D12_MESSAGE_SEVERITY_ERROR || severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
	std::println(stderr, "D3D12 Validation {}: [{}] {}", error ? "Error" : "Warning", static_cast<int>(id), description);
	std::fflush(stderr);
	if (error)
	{
		gValidationErrors++;
		if (IsDebuggerPresent())
			DebugBreak();
	}
}

// what the backend relies on, failing hard naming what the device lacks
static void CheckFeatures(ID3D12Device10* device, std::string_view name)
{
	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{.HighestShaderModel = D3D_SHADER_MODEL_6_8};
	ENSUREF(
		SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) &&
			shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_8,
		"{}: no shader model 6.8 (the shaders' SV_StartInstanceLocation, see docs/backends-plan.md): update the driver, or the D3D12 runtime (the Agility SDK)",
		name);
	D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
	ENSUREF(
		SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) &&
			options12.EnhancedBarriersSupported,
		"{}: no enhanced barriers (the backend's barriers and layouts): update the driver, or the D3D12 runtime (the Agility SDK)",
		name);
	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	ENSUREF(
		SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
			options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3,
		"{}: resource binding tier {} (the descriptor sets' tables, partly bound, need tier 3)",
		name,
		static_cast<int>(options.ResourceBindingTier));
}

} // namespace device

template <>
Device<kD3D12>::Device(CreateDescType&& desc, const Instance<kD3D12>& instance)
	: Object(std::forward<CreateDescType>(desc))
{
	ZoneScopedN("Device()");

	auto* adapter = reinterpret_cast<IDXGIAdapter4*>(GetPhysicalDevice());
	const auto name = device::AdapterName(adapter);
	d3d12::Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&myDevice)), std::format("D3D12CreateDevice ({})", name));
	d3d12::SetName(myDevice, name);
	device::CheckFeatures(myDevice, name);

	if (static_cast<InstanceHandle<kD3D12>>(instance)->validation)
	{
		ID3D12InfoQueue1* infoQueue = nullptr;
		if (SUCCEEDED(myDevice->QueryInterface(IID_PPV_ARGS(&infoQueue))))
		{
			DWORD cookie = 0;
			infoQueue->RegisterMessageCallback(device::OnMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
			d3d12::Release(infoQueue);
		}
	}

	auto* allocator = new d3d12::AllocatorObject{};
	allocator->device = myDevice;
	const D3D12MA::ALLOCATOR_DESC allocatorDesc{
		// (memory zeroed, D3D12's default: not zeroed, an attachment's first use must be a discard or clear in an
		// attachment's layout, which a blit's destination, written in its draw, isn't: the debug layer's error 1422)
		.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED,
		.pDevice = myDevice,
		.PreferredBlockSize = 0,
		.pAllocationCallbacks = nullptr,
		.pAdapter = adapter,
	};
	d3d12::Check(D3D12MA::CreateAllocator(&allocatorDesc, &allocator->allocator), "D3D12MA::CreateAllocator");
	D3D12MA::POOL_DESC hostPoolDesc{};
	hostPoolDesc.HeapProperties = D3D12_HEAP_PROPERTIES{
		.Type = D3D12_HEAP_TYPE_CUSTOM,
		.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
		.MemoryPoolPreference = D3D12_MEMORY_POOL_L0,
		.CreationNodeMask = 0,
		.VisibleNodeMask = 0,
	};
	hostPoolDesc.HeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	d3d12::Check(allocator->allocator->CreatePool(&hostPoolDesc, &allocator->hostPool), "D3D12MA::CreatePool (host visible)");
	allocator->views.Init(myDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	allocator->samplers.Init(myDevice, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	allocator->renderTargets.Init(myDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	allocator->depthStencils.Init(myDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	allocator->pool.resources.Init(
		myDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, device::kShaderVisibleResourceCount, "Shader Visible Resources");
	allocator->pool.samplers.Init(myDevice, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, device::kShaderVisibleSamplerCount, "Shader Visible Samplers");
	const D3D12_INDIRECT_ARGUMENT_DESC dispatch{.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
	const D3D12_COMMAND_SIGNATURE_DESC dispatchDesc{
		.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS), .NumArgumentDescs = 1, .pArgumentDescs = &dispatch, .NodeMask = 0};
	d3d12::Check(
		myDevice->CreateCommandSignature(&dispatchDesc, nullptr, IID_PPV_ARGS(&allocator->dispatchSignature)),
		"CreateCommandSignature (dispatch)");
	d3d12::RegisterAllocator(myDevice, allocator);
	myAllocator = allocator;

	// the timestamps' period: a direct queue's frequency (ticks a second), from a queue made to ask
	{
		const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
		ID3D12CommandQueue* queue = nullptr;
		d3d12::Check(myDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue (timestamp frequency)");
		UINT64 frequency = 0;
		d3d12::Check(queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency");
		d3d12::Release(queue);
		ENSURE(frequency > 0);
		allocator->timestampPeriod = static_cast<float>(1e9 / static_cast<double>(frequency));
	}

	// a family each queue type: direct (graphics, compute, copies), compute, copy, a queue each. timestamps on the direct
	// and compute queues (a copy queue's are a heap type of their own, which not every device has: the copy queue isn't
	// profiled)
	for (auto [flags, timestampBits] : std::array<std::pair<uint32_t, uint32_t>, 3>{
			 std::pair{kQueueFamilyFlagBitsGraphics | kQueueFamilyFlagBitsCompute | kQueueFamilyFlagBitsTransfer, 64U},
			 std::pair{kQueueFamilyFlagBitsCompute | kQueueFamilyFlagBitsTransfer, 64U},
			 std::pair{kQueueFamilyFlagBitsTransfer, 0U}})
		myQueueFamilyDescs.push_back(QueueFamilyDesc<kD3D12>{.queueCount = 1, .flags = flags, .timestampValidBits = timestampBits});
	std::println("\"{}\" is selected as primary graphics device (D3D12)", name);
	SetCrashReportDevice(std::format("{} (D3D12)", name));
}

template <>
void Device<kD3D12>::Swap(Device& other) noexcept
{
	SuperType::Swap(other);
	std::swap(myDevice, other.myDevice);
	std::swap(myAllocator, other.myAllocator);
	std::swap(myQueueFamilyDescs, other.myQueueFamilyDescs);
	std::swap(myQueues, other.myQueues);
	std::swap(myPipeline, other.myPipeline);
	std::swap(myResources, other.myResources);
}

template <>
Device<kD3D12>::Device(Device&& other) noexcept
{
	Swap(other);
}

template <>
Device<kD3D12>& Device<kD3D12>::operator=(Device<kD3D12>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Device<kD3D12>::~Device()
{
	ZoneScopedN("~Device()");

	if (!IsValid())
		return;

	myResources.clear();
	myPipeline = Pipeline<kD3D12>{};
	myQueues.clear();
	auto* allocator = myAllocator;
	allocator->pool.resources.Destroy();
	allocator->pool.samplers.Destroy();
	allocator->views.Destroy();
	allocator->samplers.Destroy();
	allocator->renderTargets.Destroy();
	allocator->depthStencils.Destroy();
	d3d12::Release(allocator->dispatchSignature);
	for (auto& [stride, signature] : allocator->drawIndexedSignatures)
		d3d12::Release(signature);
	for (auto& [format, pipeline] : allocator->blits.pipelines)
		d3d12::Release(pipeline);
	d3d12::Release(allocator->blits.rootSignature);
	d3d12::Release(allocator->hostPool);
	d3d12::Release(allocator->allocator);
	delete allocator;
	if (device::gValidationErrors > 0)
		std::println(stderr, "D3D12: {} validation errors", device::gValidationErrors.load());
	myDevice->Release();
}

template <>
DeviceLimits Device<kD3D12>::GetLimits() const
{
	// D3D12's: a buffer's views reach 2^31 bytes; resource binding tier 3's tables any number of textures (the shader
	// visible heap's million); 16384 texels a 2D image's side, 2048 a 3D one's; indirect draws with a count
	// (ExecuteIndirect's count buffer); timestamps in the direct queue's ticks. no ray queries yet (step 6)
	constexpr uint64_t kMaxBufferViewByteCount = 1ULL << 31U;
	constexpr uint32_t kMaxDrawIndirectCount = 1U << 24U;
	return DeviceLimits{
		.maxStorageBufferRange = kMaxBufferViewByteCount,
		.maxPerStageSampledImages = device::kShaderVisibleResourceCount,
		.maxDrawIndirectCount = kMaxDrawIndirectCount,
		.maxImageDimension2d = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
		.maxImageDimension3d = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION,
		.timestampPeriod = myAllocator->timestampPeriod,
		.drawIndirectCount = true,
		.drawIndirectCountEmulated = false,
		.rayQuery = false,
		.accelerationStructureScratchAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT};
}

template <>
Format Device<kD3D12>::FindSupportedFormat(std::span<const Format> candidates, ImageTiling /*tiling*/, FormatFeature features) const
{
	// the first DXGI has (no 24 bit color) whose support covers what features ask
	for (auto candidate : candidates)
	{
		if (candidate == Format::kUndefined || candidate == Format::kR8G8B8Unorm || candidate == Format::kB8G8R8Unorm ||
			candidate == Format::kA2R10G10B10UnormPack32)
			continue;
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = d3d12::ToD3D12(candidate)};
		if (FAILED(myDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
			continue;
		auto has = [&support](D3D12_FORMAT_SUPPORT1 flag) { return (support.Support1 & flag) == flag; };
		if (Any(features & FormatFeature::kSampled) && !has(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
			continue;
		if (Any(features & FormatFeature::kColorAttachment) && !has(D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
			continue;
		if (Any(features & FormatFeature::kDepthStencilAttachment) && !has(D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL))
			continue;
		return candidate;
	}
	return Format::kUndefined;
}

template <>
std::string_view Device<kD3D12>::GetName() const
{
	thread_local std::string name;
	name = device::AdapterName(reinterpret_cast<IDXGIAdapter4*>(GetPhysicalDevice()));
	return name;
}

template <>
void Device<kD3D12>::WaitIdle() const
{
	ZoneScopedN("Device::WaitIdle");

	for (const auto& [type, queue] : myQueues)
	{
		auto access = queue->Read();
		for (const auto& [deviceQueue, syncInfo] : access->queues)
			deviceQueue.WaitIdle();
	}
}

template <>
void Device<kD3D12>::InternalCreateQueues()
{
	ZoneScopedN("Device::InternalCreateQueues");

	// a queue of its own each queue type, in its family (see the constructor), with its own timeline: a direct, a compute
	// and a copy command queue (D3D12's copy queues are the gpus' copy engines, where they have them)
	constexpr std::array kQueueTypes{
		std::pair{kQueueTypeGraphics, "Graphics"}, std::pair{kQueueTypeCompute, "Compute"}, std::pair{kQueueTypeTransfer, "Transfer"}};
	auto& queues = GetQueues();
	for (uint32_t familyIt = 0; familyIt < kQueueTypes.size(); familyIt++)
	{
		const auto [type, name] = kQueueTypes[familyIt];
		auto context = std::make_shared<QueueTimelineContext<kD3D12>>(std::make_shared<QueueTimelineContextData<kD3D12>>(
			Semaphore<kD3D12>{SemaphoreCreateDesc<kD3D12>{
				CreateDeviceObjectCreateDesc(std::format("{} Queue Timeline Semaphore", name)), SemaphoreType::kTimeline}},
			uint64_t{},
			uint32_t{},
			core::CircularContainer<QueueContext<kD3D12>>{}));
		{
			auto data = context->Write();
			data->queues = std::vector<QueueContext<kD3D12>>(1);
			data->queueFamilyIndex = familyIt;
			auto& [queue, syncInfo] = data->queues.FetchAdd();
			queue = Queue<kD3D12>(QueueCreateDesc<kD3D12>{
				CreateDeviceObjectCreateDesc(std::format("{} Queue 0", name)),
				0,
				familyIt,
				static_cast<uint32_t>(GetQueueFamilies()[familyIt].timestampValidBits > 0)});
			// the direct queue, which swapchains present from
			if (type == kQueueTypeGraphics)
				myAllocator->directQueue = queue;
		}
		queues.emplace(type, context);
	}
}

template <>
void Device<kD3D12>::InternalCreatePipeline(std::vector<DescriptorPoolSize>&& descriptorPoolSizes)
{
	ZoneScopedN("Device::InternalCreatePipeline");

	myPipeline = Pipeline<kD3D12>(PipelineCreateDesc<kD3D12>{
		CreateDeviceObjectCreateDesc("Pipeline"),
		(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "pipeline.d3d12.cache").string(),
		std::move(descriptorPoolSizes)});
}

} // namespace rhi
