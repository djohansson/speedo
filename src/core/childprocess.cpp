#include <core/childprocess.h>

#include <utility>
#include <vector>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <cerrno>
#	include <spawn.h>
#	include <sys/wait.h>
extern char** environ; // NOLINT(readability-redundant-declaration): posix_spawn's environment, the starter's
#endif

namespace core
{

#if defined(_WIN32)

namespace
{

// an argument quoted for CreateProcessW's command line as CommandLineToArgvW (and the C runtime) splits it: in quotes
// where it has a space, a tab or a quote, its quotes escaped, and the backslashes before a quote (or the closing one)
// doubled
std::wstring QuoteArgument(const std::wstring& argument)
{
	if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
		return argument;
	std::wstring quoted = L"\"";
	size_t backslashes = 0;
	for (wchar_t character : argument)
	{
		if (character == L'\\')
		{
			backslashes++;
			continue;
		}
		if (character == L'"')
			quoted.append(backslashes * 2 + 1, L'\\');
		else
			quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted.push_back(character);
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'"');
	return quoted;
}

} // namespace

std::expected<ChildProcess, std::error_code> ChildProcess::Start(
	const std::filesystem::path& executable, std::span<const std::string> arguments) noexcept
{
	std::wstring commandLine = QuoteArgument(executable.wstring());
	for (const auto& argument : arguments)
	{
		commandLine.push_back(L' ');
		commandLine.append(QuoteArgument(std::filesystem::path(argument).wstring()));
	}
	STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
	PROCESS_INFORMATION information{};
	// (its own console none: CREATE_NO_WINDOW; nothing inherited)
	if (CreateProcessW(
			executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information) == 0)
		return std::unexpected(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
	CloseHandle(information.hThread);
	return ChildProcess(static_cast<void*>(information.hProcess));
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : myProcess(std::exchange(other.myProcess, nullptr)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
	if (this != &other)
	{
		(void)Wait();
		myProcess = std::exchange(other.myProcess, nullptr);
	}
	return *this;
}

ChildProcess::~ChildProcess()
{
	(void)Wait();
}

std::expected<int, std::error_code> ChildProcess::Wait() noexcept
{
	if (myProcess == nullptr)
		return std::unexpected(std::make_error_code(std::errc::no_child_process));
	auto* process = static_cast<HANDLE>(std::exchange(myProcess, nullptr));
	DWORD exitCode = 0;
	const bool waited = WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &exitCode) != 0;
	const DWORD error = waited ? 0 : GetLastError();
	CloseHandle(process);
	if (!waited)
		return std::unexpected(std::error_code(static_cast<int>(error), std::system_category()));
	return static_cast<int>(exitCode);
}

#else

std::expected<ChildProcess, std::error_code> ChildProcess::Start(
	const std::filesystem::path& executable, std::span<const std::string> arguments) noexcept
{
	std::string path = executable.string();
	std::vector<std::string> strings{path};
	strings.insert(strings.end(), arguments.begin(), arguments.end());
	std::vector<char*> argv;
	argv.reserve(strings.size() + 1);
	for (auto& string : strings)
		argv.push_back(string.data());
	argv.push_back(nullptr);
	pid_t pid = -1;
	if (int error = posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv.data(), environ); error != 0)
		return std::unexpected(std::error_code(error, std::generic_category()));
	return ChildProcess(static_cast<int>(pid));
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : myPid(std::exchange(other.myPid, -1)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
	if (this != &other)
	{
		(void)Wait();
		myPid = std::exchange(other.myPid, -1);
	}
	return *this;
}

ChildProcess::~ChildProcess()
{
	(void)Wait();
}

std::expected<int, std::error_code> ChildProcess::Wait() noexcept
{
	if (myPid < 0)
		return std::unexpected(std::make_error_code(std::errc::no_child_process));
	const pid_t pid = std::exchange(myPid, -1);
	int status = 0;
	pid_t waited = -1;
	do
		waited = waitpid(pid, &status, 0);
	while (waited < 0 && errno == EINTR);
	if (waited < 0)
		return std::unexpected(std::error_code(errno, std::generic_category()));
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	// (killed by a signal: 128 and its number, as a shell reports it)
	return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

#endif

} // namespace core
