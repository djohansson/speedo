#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace core
{

// a process started from an executable with arguments (CreateProcessW, posix_spawn), sharing the starter's environment
// and working directory, which its owner waits for (Wait: its exit code). for work that can't run in this process: the
// shader compiler's workers (slang isn't thread safe, so compiles run in parallel as processes: see gfx::ShaderLoader).
// destroying one that wasn't waited for waits for it (no zombie, nor a process left running unseen)
class ChildProcess final
{
public:
	[[nodiscard]] static std::expected<ChildProcess, std::error_code> Start(
		const std::filesystem::path& executable, std::span<const std::string> arguments) noexcept;

	ChildProcess(ChildProcess&& other) noexcept;
	ChildProcess& operator=(ChildProcess&& other) noexcept;
	ChildProcess(const ChildProcess&) = delete;
	ChildProcess& operator=(const ChildProcess&) = delete;
	~ChildProcess();

	// waits for it to end: its exit code (or the error waiting gave). once
	[[nodiscard]] std::expected<int, std::error_code> Wait() noexcept;

private:
#if defined(_WIN32)
	explicit ChildProcess(void* process) noexcept : myProcess(process) {}
	void* myProcess = nullptr; // its HANDLE, null once waited for
#else
	explicit ChildProcess(int pid) noexcept : myPid(pid) {}
	int myPid = -1; // -1 once waited for
#endif
};

} // namespace core
