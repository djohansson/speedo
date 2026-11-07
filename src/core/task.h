#pragma once

#include "std_extra.h"
#include "utils.h"

#include <array>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>

namespace core
{

// the task pool (core/memorypool.h) grows in chunks of 1024 tasks as it needs them, up to about a million, past which it's
// a leak. it was a fixed 16384, which nested ForEachs ran out of on 32 threads (Bistro's cold import: a model's textures
// each compressing on every thread, one worker per thread per loop queued, see TaskExecutor::ForEach)
static constexpr std::size_t kTaskPoolChunkSize = 1024;
static constexpr std::size_t kTaskPoolMaxChunks = 1023;
// an index and a generation, 32 bits (see MemoryPool::Handle, which it is)
using TaskHandle = MinSizeIndex<(std::size_t{1} << 31) - 1>;

// what a task and its futures share: its latch, 1 until it has run, plus one for each task it waits on (AddDependency).
// a future waits for it to reach 0, and a Future<T>'s state holds the value too (Future<T>::FutureState)
struct TaskState
{
	std::atomic_uint32_t latch{1};
};

template <typename T>
class Future
{
public:
	using value_t = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>;
	struct FutureState : TaskState
	{
		value_t value{};
	};

	Future() noexcept = default;
	explicit Future(std::shared_ptr<FutureState>&& state) noexcept : myState(std::move(state)) {}

	[[nodiscard]] bool operator==(const Future& other) const noexcept = default;

	// waits, and returns a copy of the value
	[[nodiscard]] value_t Get();
	[[nodiscard]] bool IsReady() const noexcept;
	[[nodiscard]] bool Valid() const noexcept { return !!myState; }
	void Wait() const;

private:
	std::shared_ptr<FutureState> myState;
};

template <typename T>
struct TaskCreateInfo
{
	TaskHandle handle;
	Future<T> future;
};

// a callable and the arguments it was made with, called once (by a TaskExecutor), with the tasks that wait on it. it was a
// fixed 256 bytes holding the callable (at most 128) and its arguments (at most 80) behind hand-written invoke and delete
// functions, its arguments stored as forwarding references (an lvalue's by reference, which dangled once the task ran
// later), and parameters passed when called (TaskExecutor::Call's, as a tuple behind a void*, whose types nothing
// checked: the draw calls', which are functions of their own queue now, RHI::drawCalls)
class alignas (std::hardware_constructive_interference_size) Task final
{
	friend class TaskExecutor;

	// passkey: the constructor is public so that std::construct_at can call it, but only CreateTask can name the tag
	struct PrivateTag
	{
		explicit PrivateTag() = default;
	};
	template <typename F, typename... Args>
	friend auto CreateTask(F&& callable, Args&&... args) noexcept;

public:
	// the callable and its arguments; it writes a Future<T>'s value into the state it's given (its FutureState)
	using Function = std_extra::move_only_function<void(TaskState&)>;

	Task(PrivateTag, Function&& function, std::shared_ptr<TaskState>&& state) noexcept
		: myFunction(std::move(function))
		, myState(std::move(state))
	{}

	Task(const Task&) = delete;
	Task(Task&&) noexcept = delete;
	~Task() noexcept = default;

	Task& operator=(const Task&) = delete;
	Task& operator=(Task&&) noexcept = delete;

	[[nodiscard]] explicit operator bool() const noexcept { return !!myState; }

	// calls it, once: then its latch is 0, and its futures are ready
	void operator()();

	void AddDependency(Task& other, TaskHandle otherHandle, bool isContinuation = false) noexcept;

private:
	Function myFunction;
	std::shared_ptr<TaskState> myState;
	// the tasks that wait on this one: one in practice (a chain's next), a hard failure past them
	std::array<TaskHandle, 7> myDependents{};
	std::uint8_t myDependentCount = 0;
	// queued without waking a thread once its dependencies are done: the thread that finished them runs it next
	bool myContinuation = false;
};

// makes a task of the callable and its arguments, copied or moved into it (decayed, as std::thread and std::bind take
// them: a reference is passed as std::ref), called once as callable(std::move(args)...)
template <typename F, typename... Args>
[[nodiscard]] auto CreateTask(F&& callable, Args&&... args) noexcept;

// b will start after a has finished
void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation = false) noexcept;

// the task pool's use: tasks allocated now, the most allocated at once, how many its chunks hold, and the most they may
struct TaskPoolStatistics
{
	std::size_t used = 0;
	std::size_t peak = 0;
	std::size_t capacity = 0;
	std::size_t maxCapacity = 0;
};
[[nodiscard]] TaskPoolStatistics GetTaskPoolStatistics() noexcept;

} // namespace core

#include "task.inl"
#include "future.inl"
