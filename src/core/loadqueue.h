#pragma once

#include <core/application.h>
#include <core/assert.h>
#include <core/concurrentaccess.h>
#include <core/profiling.h>
#include <core/task.h>
#include <core/taskexecutor.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace core
{

// runs loads in the thread pool, any number of them concurrently, and keeps track of the ones in progress (e.g. to show
// their progress). loads are run with the application's executor, and must have completed before it is destroyed.
class LoadQueue
{
public:
	struct Load
	{
		std::string name;
		std::atomic_uint8_t progress = 0; // 0-255, written by the load
	};

	// runs loadOp(std::atomic_uint8_t& progress) in the thread pool, listed as `name` while it runs. a load that hasn't
	// started when the application exits is skipped, and its future then holds a default constructed result.
	template <typename LoadOp>
	auto Enqueue(std::string name, LoadOp&& loadOp)
	{
		using ResultType = std::invoke_result_t<LoadOp, std::atomic_uint8_t&>;

		auto app = Application::Get();
		ENSURE(app);

		// the name and op are moved into the callable rather than passed as task arguments: CreateTask stores lvalue
		// arguments by reference, and these die when we return
		auto [task, future] = CreateTask(
			[this, name = std::move(name), loadOp = std::forward<LoadOp>(loadOp)]() mutable -> ResultType
			{
				ZoneScopedN("LoadQueue::load");

				if (Application::Get()->IsExitRequested())
					return ResultType();

				auto load = std::make_shared<Load>();
				load->name = std::move(name);
				myLoads.Write().Get().emplace_back(load);

				struct Untrack
				{
					LoadQueue& queue;
					const Load* load;
					~Untrack() { std::erase_if(queue.myLoads.Write().Get(), [this](const auto& other) { return other.get() == load; }); }
				} untrack{*this, load.get()};

				return loadOp(load->progress);
			});

		app->GetExecutor().Submit({&task, 1});

		return future;
	}

	// calls fn(const Load&) for each load in progress, in the order they were started. the loads can't start or finish
	// meanwhile, so keep fn short.
	template <typename Fn>
	void ForEach(Fn&& fn) const
	{
		auto loads = myLoads.Read();
		for (const auto& load : loads.Get())
			fn(std::as_const(*load));
	}

	[[nodiscard]] bool Empty() const { return myLoads.Read().Get().empty(); }

private:
	ConcurrentAccess<std::vector<std::shared_ptr<Load>>> myLoads;
};

} // namespace core
