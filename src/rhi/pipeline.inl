#include <rhi/crashreport.h>

#include <chrono>
#include <cstdlib>
#include <print>
#include <string_view>
#include <type_traits>

// the backends' parts of SetDescriptorData (a neutral binding in the backend's stored form, and how two stored values
// compare), before the generic definitions below: those find them by ordinary lookup at their definition, since the
// backends' structs (vulkan's are global) put rhi::pipeline out of argument-dependent lookup's reach
#include "vulkan/pipeline.inl"
#if defined(__OSX__)
#include "metal/pipeline.inl"
#endif

namespace rhi
{

namespace pipeline
{

// a pipeline's creation, timed (by the backend creating it): one that took a second or more is logged (KosmicKrisp
// compiles each in Metal, which took minutes for a cold uber-kernel), and every one with SPEEDO_LOG_PIPELINES=1
inline void LogCreation(std::string_view kind, std::string_view entryPoint, uint32_t specialization, std::chrono::steady_clock::time_point start)
{
	static const bool kAll = []
	{
		const char* setting = std::getenv("SPEEDO_LOG_PIPELINES");
		return setting != nullptr && std::string_view(setting) == "1";
	}();
	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	if (kAll || seconds >= 1.0)
		std::println(stderr, "pipeline: {} {} (specialization {}) took {:.2f} s to create", kind, entryPoint, specialization, seconds);
}

} // namespace pipeline

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

// PipelineLayout's Swap and moves: the same for every backend

template <GraphicsApi G>
void PipelineLayout<G>::Swap(PipelineLayout& rhs) noexcept
{
	DeviceObject<PipelineLayout<G>>::Swap(rhs);
	std::swap(myShaderModules, rhs.myShaderModules);
	std::swap(myDescriptorSetLayouts, rhs.myDescriptorSetLayouts);
	std::swap(myPushConstantRanges, rhs.myPushConstantRanges);
	std::swap(myLayout, rhs.myLayout);
}

template <GraphicsApi G>
PipelineLayout<G>::PipelineLayout(PipelineLayout&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
PipelineLayout<G>& PipelineLayout<G>::operator=(PipelineLayout&& other) noexcept
{
	Swap(other);
	return *this;
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

namespace pipeline
{

template <typename T>
[[nodiscard]] bool SameBindingValue(const T& lhs, const T& rhs) noexcept // handles, inline uniform blocks, a backend's comparable structs
{
	return lhs == rhs;
}

// the stored form of a descriptor value: neutral bindings become the backend's structs (its ToBackendBinding),
// everything else is stored as is
template <typename T>
[[nodiscard]] decltype(auto) ToBindingValue(T&& value) noexcept
{
	if constexpr (requires { ToBackendBinding(value); })
		return ToBackendBinding(value);
	else
		return std::forward<T>(value);
}

template <typename T>
[[nodiscard]] std::vector<std::remove_cvref_t<decltype(ToBindingValue(std::declval<const T&>()))>> ToBindingValues(
	const std::vector<T>& values)
{
	std::vector<std::remove_cvref_t<decltype(ToBindingValue(std::declval<const T&>()))>> result;
	result.reserve(values.size());
	for (const auto& value : values)
		result.push_back(ToBindingValue(value));
	return result;
}

// used to skip redundant descriptor updates: every update marks the descriptor set dirty, and binding a dirty set
// consumes a fresh descriptor set from the pool
template <GraphicsApi G, typename T>
[[nodiscard]] bool SameBinding(const BindingVariant<G>& existing, const T& data) noexcept
{
	const auto* current = std::get_if<std::remove_cvref_t<T>>(&existing);
	return current != nullptr && SameBindingValue(*current, data);
}

} // namespace pipeline

template <GraphicsApi G>
template <typename T>
void Pipeline<G>::SetDescriptorData(
	uint64_t shaderVariableNameHash, const DescriptorSetLayout<G>& layout, T&& data)
{
	const auto& [binding, descriptorType, descriptorCount] =
		layout.GetShaderVariableBinding(shaderVariableNameHash);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(layout);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, core::RangeSet<uint32_t>{}));

	ENSURE(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ENSURE(count == 0);
		ENSURE(ranges.empty());

		count = 1;
		ranges.insert({0U, 1U});

		if (bindingIt == bindingsMap.begin())
			offset = 0;
		else
			offset = std::get<0>(std::prev(bindingIt)->second) +
					 std::get<1>(std::prev(bindingIt)->second);

		bindingsData.emplace(bindingsData.begin() + offset, std::forward<T>(data));

		// prefix sum offset
		for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
			 it++, prev++)
			std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
	}
	else
	{
		ENSURE(count == 1);

		if (pipeline::SameBinding<G>(bindingsData[offset], data))
			return;

		std::get<std::remove_cvref_t<T>>(bindingsData[offset]) = std::forward<T>(data);
	}

	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <GraphicsApi G>
template <typename T>
void Pipeline<G>::SetDescriptorData(
	std::string_view shaderVariableName, T&& data, uint32_t set)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		pipeline::ToBindingValue(std::forward<T>(data)));
}

template <GraphicsApi G>
template <typename T>
void Pipeline<G>::SetDescriptorData(
	uint64_t shaderVariableNameHash,
	const DescriptorSetLayout<G>& layout,
	const std::vector<T>& data)
{
	const auto& [binding, descriptorType, descriptorCount] =
		layout.GetShaderVariableBinding(shaderVariableNameHash);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(layout);

	ENSURE(data.size() <= descriptorCount);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, core::RangeSet<uint32_t>{}));

	ENSURE(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ENSURE(count == 0);
		ENSURE(ranges.empty());

		count = data.size();
		ranges.insert({0U, count});

		if (bindingIt == bindingsMap.begin())
			offset = 0;
		else
			offset = std::get<0>(std::prev(bindingIt)->second) +
					 std::get<1>(std::prev(bindingIt)->second);

		bindingsData.insert(bindingsData.begin() + offset, data.begin(), data.end());

		// prefix sum offset
		for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
			 it++, prev++)
			std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
	}
	else
	{
		ENSURE(count > 0);

		if (count == data.size())
		{
			if (std::equal(data.begin(), data.end(), bindingsData.begin() + offset,
					[](const T& value, const BindingVariant<G>& existing) { return pipeline::SameBinding<G>(existing, value); }))
				return;

			std::copy(data.begin(), data.end(), bindingsData.begin() + offset);
		}
		else
		{
			auto minCount = std::min(static_cast<uint32_t>(data.size()), count);

			std::copy(data.begin(), data.begin() + minCount, bindingsData.begin() + offset);

			if (count < data.size())
				bindingsData.insert(
					bindingsData.begin() + offset + minCount, data.begin() + minCount, data.end());
			else // must erase: bindingsData is packed, each binding's offset is the prefix sum of the counts before it
				bindingsData.erase(
					bindingsData.begin() + offset + minCount,
					bindingsData.begin() + offset + count);

			count = data.size();
			ranges.clear();
			ranges.insert({0U, count});

			// prefix sum offset
			for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
				 it++, prev++)
				std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
		}
	}

	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <GraphicsApi G>
template <typename T>
void Pipeline<G>::SetDescriptorData(
	std::string_view shaderVariableName, const std::vector<T>& data, uint32_t set)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		pipeline::ToBindingValues(data));
}

template <GraphicsApi G>
template <typename T>
void Pipeline<G>::SetDescriptorData(
	uint64_t shaderVariableNameHash,
	const DescriptorSetLayout<G>& layout,
	T&& data,
	uint32_t index)
{
	const auto& [binding, descriptorType, descriptorCount] =
		layout.GetShaderVariableBinding(shaderVariableNameHash);
	auto& [mutex, setState, bindingsMap, bindingsData, setTemplate, setOptionalArrayList] =
		myDescriptorMap.at(layout);

	auto lock = std::lock_guard(mutex);

	auto [bindingIt, emplaceResult] =
		bindingsMap.emplace(binding, std::make_tuple(0, 0, descriptorType, core::RangeSet<uint32_t>{}));

	ENSURE(bindingIt != bindingsMap.end());

	auto& [offset, count, type, ranges] = bindingIt->second;

	if (emplaceResult)
	{
		ENSURE(count == 0);
		ENSURE(ranges.empty());

		count = 1;
		ranges.insert({index, index + 1});

		if (bindingIt == bindingsMap.begin())
			offset = 0;
		else
			offset = std::get<0>(std::prev(bindingIt)->second) +
					 std::get<1>(std::prev(bindingIt)->second);

		bindingsData.emplace(bindingsData.begin() + offset, std::forward<T>(data));

		// prefix sum offset
		for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
			 it++, prev++)
			std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
	}
	else
	{
		ENSURE(count > 0);

		// the binding's elements are stored in index order (the update template walks ranges in order): the element's
		// position is the number of elements in the ranges before it. a new index goes before the first range above it,
		// not at the end.
		auto rangeIt = ranges.begin();
		uint32_t indexOffset = 0;
		bool found = false;
		for (; rangeIt != ranges.end(); rangeIt++)
		{
			const auto& [low, high] = *rangeIt;

			if (index < low)
				break;

			if (index < high)
			{
				indexOffset += index - low;
				found = true;
				break;
			}

			indexOffset += high - low;
		}

		if (found)
		{
			if (pipeline::SameBinding<G>(bindingsData[offset + indexOffset], data))
				return;

			std::get<std::remove_cvref_t<T>>(bindingsData[offset + indexOffset]) = std::forward<T>(data);
		}
		else
		{
			bindingsData.emplace(
				bindingsData.begin() + offset + indexOffset, std::forward<T>(data));

			count++;
			ranges.insert({index, index + 1});

			// prefix sum offset
			for (auto it = std::next(bindingIt), prev = bindingIt; it != bindingsMap.end();
				 it++, prev++)
				std::get<0>(it->second) = std::get<0>(prev->second) + std::get<1>(prev->second);
		}
	}

	std::atomic_ref(setState).store(DescriptorSetStatus::kDirty, std::memory_order_release);

	InternalUpdateDescriptorSetTemplate(bindingsMap, setTemplate);
}

template <GraphicsApi G>
template <typename T>
void Pipeline<G>::SetDescriptorData(
	std::string_view shaderVariableName, T&& data, uint32_t set, uint32_t index)
{
	auto layoutIt = InternalGetLayout();
	ENSURE(layoutIt != myPipelineLayouts.end());
	SetDescriptorData(
		XXH3_64bits(shaderVariableName.data(), shaderVariableName.size()),
		layoutIt->GetDescriptorSetLayout(set),
		pipeline::ToBindingValue(std::forward<T>(data)),
		index);
}

} // namespace rhi
