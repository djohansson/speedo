// measures the task system's overheads (core/task.h, core/taskexecutor.h), and how full the task pool gets:
//
// - submit: create n empty tasks, submit them at once, and wait for them all (JoinAll): the time per task;
// - call: create a task and call it inline on the calling thread (TaskExecutor::Call): what a task costs itself;
// - chain: a continuation chain of n tasks, each creating and depending on the next as the client's tick and draw chains
//   do (AddDependency(..., true)): the time per link;
// - foreach: ForEach over the executor's thread count of indices with a trivial body, n times: the time per call;
// - nested: the shape of a model's texture loads, a ForEach over <outer> textures, each a ForEach per <levels> mip level over
//   <inner> rows of about a microsecond's work (gfx::image's CompressLevel): the time, and the pool's peak use, which on
//   many threads ran out (Bistro's cold import on 32 threads: "Ensure failed: myAvailable > 0" in the task pool).
//
// usage: taskbench [--threads <n>] [--iterations <n>] [--tasks <n>] [--nested <outer>,<levels>,<inner>]
//                  [submit|call|chain|foreach|nested ...]
//
// each benchmark runs --iterations times (default 5), its median printed; the pool's peak is over the whole run.

#include <core/task.h>
#include <core/taskexecutor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

struct Options
{
	uint32_t threads = std::max(1U, std::thread::hardware_concurrency());
	uint32_t iterations = 5;
	uint32_t tasks = 4096;
	uint32_t outer = 64;
	uint32_t levels = 10;
	uint32_t inner = 64;
	std::vector<std::string> benchmarks;
};

// about a microsecond of work the compiler can't drop
uint64_t Spin(uint64_t seed)
{
	for (int it = 0; it < 400; it++)
		seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
	return seed;
}

std::atomic_uint64_t gSink = 0;

template <typename Fn>
double MedianSeconds(uint32_t iterations, Fn&& run)
{
	std::vector<double> seconds;
	for (uint32_t it = 0; it < iterations; it++)
	{
		auto start = Clock::now();
		run();
		seconds.push_back(std::chrono::duration<double>(Clock::now() - start).count());
	}
	std::ranges::sort(seconds);
	return seconds[seconds.size() / 2];
}

void Submit(core::TaskExecutor& executor, const Options& options)
{
	auto seconds = MedianSeconds(options.iterations, [&]
	{
		std::vector<core::TaskHandle> handles;
		handles.reserve(options.tasks);
		for (uint32_t it = 0; it < options.tasks; it++)
			handles.push_back(core::CreateTask([] { gSink.fetch_add(1, std::memory_order_relaxed); }).handle);
		executor.Submit(handles);
		executor.JoinAll();
	});
	std::println("submit: {} tasks, {:.0f} ns a task", options.tasks, seconds * 1e9 / options.tasks);
}

// create a task and call it inline (TaskExecutor::Call): no queue or thread, what a task costs itself
void CallBenchmark(core::TaskExecutor& executor, const Options& options)
{
	auto seconds = MedianSeconds(options.iterations, [&]
	{
		for (uint32_t it = 0; it < options.tasks; it++)
			executor.Call(core::CreateTask([] { gSink.fetch_add(1, std::memory_order_relaxed); }).handle);
		executor.JoinAll();
	});
	std::println("call: {} tasks, {:.0f} ns a task", options.tasks, seconds * 1e9 / options.tasks);
}

struct Chain
{
	core::TaskHandle current;
	uint32_t left = 0;
	std::atomic_bool done = false;
};

void ChainLink(Chain* chain)
{
	if (--chain->left == 0)
	{
		chain->done.store(true, std::memory_order_release);
		chain->done.notify_all();
		return;
	}
	auto next = core::CreateTask([chain] { ChainLink(chain); });
	core::AddDependency(chain->current, next.handle, true);
	chain->current = next.handle;
}

void ChainBenchmark(core::TaskExecutor& executor, const Options& options)
{
	auto seconds = MedianSeconds(options.iterations, [&]
	{
		Chain chain;
		chain.left = options.tasks;
		auto first = core::CreateTask([chain = &chain] { ChainLink(chain); });
		chain.current = first.handle;
		executor.Submit({&first.handle, 1});
		chain.done.wait(false, std::memory_order_acquire);
		executor.JoinAll();
	});
	std::println("chain: {} links, {:.0f} ns a link", options.tasks, seconds * 1e9 / options.tasks);
}

void ForEach(core::TaskExecutor& executor, const Options& options)
{
	auto calls = std::max(1U, options.tasks / options.threads);
	auto seconds = MedianSeconds(options.iterations, [&]
	{
		for (uint32_t call = 0; call < calls; call++)
			executor.ForEach(options.threads, [](size_t index) { gSink.fetch_add(index, std::memory_order_relaxed); });
		executor.JoinAll();
	});
	std::println("foreach: {} calls over {} indices, {:.2f} us a call", calls, options.threads, seconds * 1e6 / calls);
}

void Nested(core::TaskExecutor& executor, const Options& options)
{
	auto seconds = MedianSeconds(options.iterations, [&]
	{
		executor.ForEach(options.outer, [&](size_t texture)
		{
			for (uint32_t level = 0; level < options.levels; level++)
				executor.ForEach(options.inner, [&](size_t row)
				{
					gSink.fetch_add(Spin(texture * 7919 + level * 131 + row + 1), std::memory_order_relaxed);
				});
		});
		executor.JoinAll();
	});
	auto work = uint64_t{options.outer} * options.levels * options.inner;
	std::println("nested: {} x {} x {} ({} rows of about 1 us), {:.1f} ms, {:.2f} us a row", options.outer, options.levels,
		options.inner, work, seconds * 1e3, seconds * 1e6 / work);
}

bool ParseNested(std::string_view text, Options& options)
{
	uint32_t values[3]{};//NOLINT(modernize-avoid-c-arrays)
	for (uint32_t& value : values)
	{
		auto comma = text.find(',');
		value = static_cast<uint32_t>(std::strtoul(std::string(text.substr(0, comma)).c_str(), nullptr, 10));
		if (value == 0)
			return false;
		text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
	}
	options.outer = values[0];
	options.levels = values[1];
	options.inner = values[2];
	return true;
}

} // namespace

int main(int argc, char* argv[])
{
	Options options;
	for (int argIt = 1; argIt < argc; argIt++)
	{
		std::string_view arg = argv[argIt];
		auto value = [&] { return argIt + 1 < argc ? static_cast<uint32_t>(std::strtoul(argv[++argIt], nullptr, 10)) : 0U; };
		if (arg == "--threads")
			options.threads = std::max(1U, value());
		else if (arg == "--iterations")
			options.iterations = std::max(1U, value());
		else if (arg == "--tasks")
			options.tasks = std::max(1U, value());
		else if (arg == "--nested" && argIt + 1 < argc)
		{
			if (!ParseNested(argv[++argIt], options))
			{
				std::println(stderr, "--nested takes <outer>,<levels>,<inner>");
				return EXIT_FAILURE;
			}
		}
		else if (arg == "submit" || arg == "call" || arg == "chain" || arg == "foreach" || arg == "nested")
			options.benchmarks.emplace_back(arg);
		else
		{
			std::println(stderr, "usage: taskbench [--threads <n>] [--iterations <n>] [--tasks <n>] "
				"[--nested <outer>,<levels>,<inner>] [submit|call|chain|foreach|nested ...]");
			return EXIT_FAILURE;
		}
	}
	if (options.benchmarks.empty())
		options.benchmarks = {"submit", "call", "chain", "foreach", "nested"};

	std::println("{} threads, median of {} iterations, a task {} bytes", options.threads, options.iterations, sizeof(core::Task));
	{
		core::TaskExecutor executor(options.threads);
		for (const auto& benchmark : options.benchmarks)
		{
			if (benchmark == "submit")
				Submit(executor, options);
			else if (benchmark == "call")
				CallBenchmark(executor, options);
			else if (benchmark == "chain")
				ChainBenchmark(executor, options);
			else if (benchmark == "foreach")
				ForEach(executor, options);
			else
				Nested(executor, options);
		}
		executor.JoinAll();
	}
	auto pool = core::GetTaskPoolStatistics();
	std::println("task pool: peak {} of {} ({} in use at exit)", pool.peak, pool.capacity, pool.used);
	return gSink.load() == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
