#include <rhi/crashreport.h>

namespace rhi
{

template <GraphicsApi G>
const DescriptorSetLayout<G>& PipelineLayout<G>::GetDescriptorSetLayout(uint32_t set) const noexcept
{
	const auto& setLayouts = GetDescriptorSetLayouts();
	auto setLayoutIt = std::lower_bound(
		setLayouts.begin(),
		setLayouts.end(),
		set,
		[](const auto& setLayout, uint32_t set) { return setLayout.first < set; });
	ENSURE(setLayoutIt != setLayouts.end());
	const auto& [_set, setLayout] = *setLayoutIt;
	ENSURE(_set == set);

	return setLayout;
}

// the pipelines' map, their requests and binds, the compute layouts' launch parameters and the layouts by name: the same
// for every backend, which creates the pipelines (InternalCreateGraphicsPipeline, InternalCreateComputePipeline) and binds
// them (BindPipeline)

template <GraphicsApi G>
PipelineHandle<G> Pipeline<G>::InternalGetPipeline(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant)
{
	ZoneScopedN("Pipeline::InternalGetPipeline");

	auto key = InternalCalculateHashKey(variant, computeVariant);
	auto& sync = *myPipelineMapSync;
	std::unique_lock lock(sync.mutex);
	auto [it, inserted] = myPipelineMap.try_emplace(key, PipelineHandle<G>{});
	if (!inserted)
	{
		// created, or being created (here, or by a job from RequestGraphicsPipeline): wait for it
		if (it->second == nullptr)
		{
			ZoneScopedN("Pipeline::InternalGetPipeline::wait");
			CrashBusy busy; // another thread compiles it (see CrashBusy)
			sync.created.wait(lock, [this, key] { return myPipelineMap.at(key) != nullptr; });
		}
		return myPipelineMap.at(key);
	}

	// create it, without holding the lock (others asking for it wait above)
	lock.unlock();
	PipelineHandle<G> handle{};
	{
		ZoneScopedN("Pipeline::InternalGetPipeline::create");
		CrashBusy busy; // inside the driver's compile (see CrashBusy)
		switch (myBindPoint)
		{
		case PipelineBindPoint::kGraphics:
			handle = InternalCreateGraphicsPipeline(key, variant, *InternalSnapshotGraphicsState());
			break;
		case PipelineBindPoint::kCompute:
		{
			const auto layoutIt = InternalGetLayout();
			ENSURE(layoutIt != myPipelineLayouts.end());
			ENSUREF(
				computeVariant.entryPoint < myComputeState.shaderStages.size(),
				"the layout has no compute entry point {} (it has {})",
				computeVariant.entryPoint,
				myComputeState.shaderStages.size());
			handle = InternalCreateComputePipeline(key, computeVariant, myComputeState.shaderStages[computeVariant.entryPoint], *layoutIt);
			break;
		}
		default:
			ASSERTF(false, "Not implemented");
		}
	}
	// the draw thread made progress (CrashWatch): after a shader change, its first frames create the GI's compute pipelines
	// cold, which KosmicKrisp compiles in Metal, over 20 s in all, and the watch reported a hang at startup (here, where the
	// draw thread binds, not in the background jobs of RequestGraphicsPipeline, whose progress isn't the draw thread's)
	CrashHeartbeat();
	lock.lock();
	myPipelineMap.at(key) = handle;
	sync.created.notify_all();
	return handle;
}

template <GraphicsApi G>
typename Pipeline<G>::GraphicsPipelineRequest Pipeline<G>::RequestGraphicsPipeline(GraphicsPipelineVariant variant)
{
	ZoneScopedN("Pipeline::RequestGraphicsPipeline");

	ENSURE(myBindPoint == PipelineBindPoint::kGraphics);
	auto key = InternalCalculateHashKey(variant);
	auto& sync = *myPipelineMapSync;
	{
		std::unique_lock lock(sync.mutex);
		auto [it, inserted] = myPipelineMap.try_emplace(key, PipelineHandle<G>{});
		if (!inserted)
			return {.handle = it->second};
	}

	// the first ask: a task creating it from the state as it is now
	return {
		.handle = {},
		.create = core::CreateTask(
			[this, key, variant, snapshot = InternalSnapshotGraphicsState()]
			{
				auto handle = InternalCreateGraphicsPipeline(key, variant, *snapshot);
				auto& sync = *myPipelineMapSync;
				std::unique_lock lock(sync.mutex);
				myPipelineMap.at(key) = handle;
				sync.created.notify_all();
			}).handle};
}

template <GraphicsApi G>
std::vector<core::TaskCreateInfo<void>> Pipeline<G>::RequestComputePipelines(std::span<const ComputePipelineVariant> variants)
{
	ZoneScopedN("Pipeline::RequestComputePipelines");

	ENSURE(myBindPoint == PipelineBindPoint::kCompute);
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	PipelineLayoutHandle<G> layout = *layoutIt;

	std::vector<core::TaskCreateInfo<void>> tasks;
	auto& sync = *myPipelineMapSync;
	for (const auto& variant : variants)
	{
		ENSUREF(
			variant.entryPoint < myComputeState.shaderStages.size(),
			"the layout has no compute entry point {} (it has {})",
			variant.entryPoint,
			myComputeState.shaderStages.size());
		auto key = InternalCalculateHashKey({}, variant);
		{
			std::unique_lock lock(sync.mutex);
			if (auto [it, inserted] = myPipelineMap.try_emplace(key, PipelineHandle<G>{}); !inserted)
				continue; // created, or being created
		}
		// its entry point's stage as it is now (the module and name are the layout's, which outlives the task)
		tasks.push_back(core::CreateTask(
			[this, key, variant, stage = myComputeState.shaderStages[variant.entryPoint], layout]
			{
				auto handle = InternalCreateComputePipeline(key, variant, stage, layout);
				auto& sync = *myPipelineMapSync;
				std::unique_lock lock(sync.mutex);
				myPipelineMap.at(key) = handle;
				sync.created.notify_all();
			}));
	}
	return tasks;
}

template <GraphicsApi G>
uint32_t Pipeline<G>::GetComputeEntryPointCount() const noexcept
{
	return static_cast<uint32_t>(myComputeState.shaderStages.size());
}

template <GraphicsApi G>
PipelineHandle<G> Pipeline<G>::BindPipelineAuto(CommandBufferHandle<G> cmd, GraphicsPipelineVariant variant)
{
	auto handle = InternalGetPipeline(variant);

	BindPipeline(cmd, myBindPoint, handle);

	return handle;
}

template <GraphicsApi G>
PipelineHandle<G> Pipeline<G>::BindPipelineAuto(CommandBufferHandle<G> cmd, ComputePipelineVariant variant)
{
	ENSURE(myBindPoint == PipelineBindPoint::kCompute);
	auto handle = InternalGetPipeline({}, variant);

	BindPipeline(cmd, myBindPoint, handle);

	return handle;
}

template <GraphicsApi G>
const ComputeLaunchParameters& Pipeline<G>::GetComputeLaunchParameters(ComputePipelineVariant variant) const
{
	ENSURE(myBindPoint == PipelineBindPoint::kCompute && variant.entryPoint < myComputeState.launchParameters.size());
	return myComputeState.launchParameters[variant.entryPoint];
}

template <GraphicsApi G>
PipelineLayoutHandle<G> Pipeline<G>::GetLayout() const noexcept
{
	const auto layoutIt = InternalGetLayout();

	if (layoutIt == myPipelineLayouts.end())
		return {};

	return static_cast<PipelineLayoutHandle<G>>(*layoutIt);
}

template <GraphicsApi G>
PipelineLayoutHandle<G> Pipeline<G>::CreateLayout(std::string_view name, const ShaderSet& shaderSet, std::span<const uint32_t> pushDescriptorSets)
{
	ENSUREF(!myLayoutNames.contains(std::string(name)), "pipeline layout {} exists", name);

	const auto& [layoutIt, wasInserted] = myPipelineLayouts.emplace(
		PipelineLayout<G>(
			PipelineLayoutCreateDesc<G>{
				SuperType::CreateDeviceObjectCreateDesc(name),
			},
			shaderSet,
			pushDescriptorSets));

	auto handle = static_cast<PipelineLayoutHandle<G>>(*layoutIt);
	myLayoutNames.emplace(std::string(name), handle);
	return handle;
}

template <GraphicsApi G>
PipelineLayoutHandle<G> Pipeline<G>::GetLayoutHandle(std::string_view name) const
{
	auto nameIt = myLayoutNames.find(std::string(name));
	ENSUREF(nameIt != myLayoutNames.end(), "no pipeline layout {}", name);
	return nameIt->second;
}

template <GraphicsApi G>
void Pipeline<G>::Swap(Pipeline& rhs) noexcept
{
	DeviceObject<Pipeline<G>>::Swap(rhs);
	std::swap(myDescriptorMap, rhs.myDescriptorMap);
	std::swap(myDescriptorPool, rhs.myDescriptorPool);
	std::swap(myDescriptorPoolMutex, rhs.myDescriptorPoolMutex);
	std::swap(myPipelineMap, rhs.myPipelineMap);
	std::swap(myPipelineMapSync, rhs.myPipelineMapSync);
	std::swap(myCache, rhs.myCache);
	std::swap(myBindPoint, rhs.myBindPoint);
	std::swap(myRenderTarget, rhs.myRenderTarget);
	std::swap(myPipelineLayouts, rhs.myPipelineLayouts);
	std::swap(myLayoutNames, rhs.myLayoutNames);
	std::swap(myCurrentLayoutIt, rhs.myCurrentLayoutIt);
	std::swap(myGraphicsState, rhs.myGraphicsState);
	std::swap(myComputeState, rhs.myComputeState);
	std::swap(myRayTracingState, rhs.myRayTracingState);
}

template <GraphicsApi G>
Pipeline<G>::Pipeline(Pipeline&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
Pipeline<G>& Pipeline<G>::operator=(Pipeline&& other) noexcept
{
	Swap(other);
	return *this;
}

// the descriptor sets' state: the same for every backend, which updates, pushes and binds them (InternalUpdateDescriptorSet,
// InternalPushDescriptorSet, BindDescriptorSet)

template <GraphicsApi G>
void Pipeline<G>::InvalidateDescriptorSets()
{
	for (auto& [setLayout, state] : myDescriptorMap)
	{
		auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = state;
		auto lock = std::lock_guard(mutex);
		std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);
	}
}

template <GraphicsApi G>
std::shared_ptr<void> Pipeline<G>::TakeDescriptorSets(uint32_t set)
{
	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& setLayout = layoutIt->GetDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(setLayout);
	auto lock = std::lock_guard(mutex);
	if (!setOptionalArrayList || setOptionalArrayList->empty())
		return {};
	auto taken = std::make_shared<DescriptorSetArrayList<G>>(std::exchange(*setOptionalArrayList, {}));
	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);
	return taken;
}

template <GraphicsApi G>
void Pipeline<G>::BindDescriptorSetAuto(CommandBufferHandle<G> cmd, uint32_t set, std::optional<uint32_t> bufferOffset)
{
	ZoneScopedN("Pipeline::BindDescriptorSetAuto");

	const auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	const auto& layout = *layoutIt;
	const auto& setLayout = layout.GetDescriptorSetLayout(set);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] = myDescriptorMap.at(setLayout);

	if (setOptionalArrayList)
	{
		mutex.lock_upgrade();

		auto dirtyState = DescriptorSetStatus::kDirty;
		if (std::atomic_ref(setState).compare_exchange_weak(dirtyState, DescriptorSetStatus::kReady, std::memory_order_acq_rel))
		{
			mutex.unlock_upgrade_and_lock();

			InternalUpdateDescriptorSet(setLayout, bindingsData, setTemplate, setOptionalArrayList.value());

			mutex.unlock_and_lock_shared();
		}
		else [[likely]]
		{
			mutex.unlock_upgrade_and_lock_shared();
		}

		auto& setArrayList = setOptionalArrayList.value();
		ENSURE(!setArrayList.empty());
		auto& [setArray, setIndex] = setArrayList.front();
		auto handle = setArray[setIndex];

		BindDescriptorSet(cmd, handle, myBindPoint, static_cast<PipelineLayoutHandle<G>>(layout), set, bufferOffset);
	}
	else // push descriptors (core in vulkan 1.4; the Metal backend's are a dispatch's own argument buffer)
	{
		mutex.lock_shared();

		InternalPushDescriptorSet(cmd, static_cast<PipelineLayoutHandle<G>>(layout), bindingsData, setTemplate);
	}

	mutex.unlock_shared();
}

} // namespace rhi

#include "vulkan/pipeline.inl"
