#pragma once

#include "future.h"
#include "task.h"
#include "utils.h"

#include <tuple>
#include <type_traits>
#include <vector>

class TaskGraph : public Noncopyable
{
public:
	using TaskNodeHandle = size_t;

	constexpr TaskGraph() noexcept = default;
	TaskGraph(TaskGraph&& other) noexcept;

	TaskGraph& operator=(TaskGraph&& other) noexcept;

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...> std::tuple<TaskNodeHandle, Future<ReturnType>>
	createTask(F&& f, Args&&... args);

	// a happens before b
	void addDependency(TaskNodeHandle a, TaskNodeHandle b);

private:
	friend class TaskExecutor;

	using TaskNodeHandleVector = std::vector<TaskNodeHandle>;
	// todo: use pool allocator for tasks, and just use pointer here
	using TaskNode = std::tuple<Task, TaskNodeHandleVector, size_t>; // task, adjacencies, dependencies
	using TaskNodeVec = std::vector<TaskNode>;

	void depthFirstSearch(
		size_t v,
		std::vector<bool>& discovered,
		std::vector<size_t>& departure,
		std::vector<size_t>& sorted,
		size_t& time) const;

	void finalize(); // will invalidate all task handles allocated from graph

	TaskNodeVec myNodes;
};

#include "taskgraph.inl"