#include "taskgraph.h"

TaskGraph::TaskGraph(TaskGraph&& other) noexcept
{
	*this = std::forward<TaskGraph>(other);
}

TaskGraph& TaskGraph::operator=(TaskGraph&& other) noexcept
{
	myNodes = std::exchange(other.myNodes, {});

	return *this;
}

void TaskGraph::addDependency(TaskNodeHandle a, TaskNodeHandle b)
{
	auto& [taskA, adjacenciesA, dependenciesA] = myNodes[a];
	auto& [taskB, adjacenciesB, dependenciesB] = myNodes[b];

	adjacenciesA.push_back(b);
	++dependenciesB;
}

void TaskGraph::depthFirstSearch(
	size_t v,
	std::vector<bool>& visited,
	std::vector<size_t>& departure,
	std::vector<size_t>& sorted,
	size_t& time) const
{
	visited[v] = true;

	const auto& [task, adjacencies, dependencies] = myNodes[v];

	for (auto u : adjacencies)
		if (!visited[u])
			depthFirstSearch(u, visited, departure, sorted, time);

	sorted.emplace_back(v);
	departure[v] = time++;
};

void TaskGraph::finalize()
{
	ZoneScopedN("TaskGraph::finalize");

	auto n = myNodes.size();

	std::vector<bool> visited(n);
	std::vector<size_t> departure(n);
	std::vector<size_t> sorted;

	sorted.reserve(n);

	size_t time = 0;

	for (size_t i = 0; i < n; i++)
		if (!visited[i])
			depthFirstSearch(i, visited, departure, sorted, time);

	for (size_t u = 0; u < n; u++)
	{
		const auto& [task, adjacencies, dependencies] = myNodes[u];

		for (auto v : adjacencies)
			if (departure[u] <= departure[v])
				throw std::runtime_error("Graph is not a DAG!");
	}

	decltype(myNodes) sortedNodes(n);

	for (size_t i = 0; i < sorted.size(); i++)
		//sortedNodes[i] = std::move(myNodes[sorted[i]]);
		sortedNodes[i] = std::move(myNodes[myNodes.size() - sorted[i] - 1]);

	myNodes = std::move(sortedNodes);

	for (auto& [task, adjacencies, dependencies] : myNodes)
	{
		task.state()->latch.emplace(dependencies + 1); // deps + self

		for (auto adjacent : adjacencies)
			task.state()->adjacencies.emplace_back(&std::get<0>(myNodes[adjacent]));
	}
}
