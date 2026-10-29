#include <core/std_extra.h>

#if !defined(__cpp_lib_thread_attributes)

#include <algorithm>
#include <future>
#include <memory>
#include <string>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#	include <windows.h>
#	include <process.h>
#else
#	include <limits.h>//NOLINT(modernize-deprecated-headers): PTHREAD_STACK_MIN
#	include <pthread.h>
#	include <unistd.h>
#endif

namespace core::std_extra::detail
{

namespace
{

// what a thread of execution starts with: its body and its name, and its id for the constructing thread, which waits for
// it (a promise: its state outlives either side, so the thread may still be in set_value when the other returns)
struct ThreadStart
{
	move_only_function<void()> body;
	std::string name;
	std::promise<std::thread::id> id;
};

void RunThread(ThreadStart* argument)
{
	std::unique_ptr<ThreadStart> start(argument);
	if (!start->name.empty())
	{
#if defined(_WIN32)
		std::wstring name(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, start->name.data(), static_cast<int>(start->name.size()), nullptr, 0)), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, start->name.data(), static_cast<int>(start->name.size()), name.data(), static_cast<int>(name.size()));
		SetThreadDescription(GetCurrentThread(), name.c_str());
#elif defined(__APPLE__)
		pthread_setname_np(start->name.c_str()); // the calling thread's only
#else
		pthread_setname_np(pthread_self(), start->name.substr(0, 15).c_str()); // 16 bytes with the terminator
#endif
	}
	start->id.set_value(std::this_thread::get_id());
	start->body();
}

#if defined(_WIN32)
unsigned __stdcall ThreadMain(void* argument)
{
	RunThread(static_cast<ThreadStart*>(argument));
	return 0;
}
#else
void* ThreadMain(void* argument)
{
	RunThread(static_cast<ThreadStart*>(argument));
	return nullptr;
}
#endif

} // namespace

std::pair<std::thread::native_handle_type, std::thread::id>
StartThread(size_t stackSize, std::string_view name, move_only_function<void()> body)
{
	auto start = std::make_unique<ThreadStart>(ThreadStart{.body = std::move(body), .name = std::string(name), .id = {}});
	auto id = start->id.get_future();
	std::thread::native_handle_type handle{};
#if defined(_WIN32)
	// the size reserved, not committed (CreateThread's flag, which _beginthreadex passes on)
	auto thread = _beginthreadex(nullptr, static_cast<unsigned>(stackSize), ThreadMain, start.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (thread == 0)
		throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "jthread: _beginthreadex");
	handle = reinterpret_cast<std::thread::native_handle_type>(thread);
#else
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	if (stackSize != 0)
	{
		// at least the platform's least, in whole pages (macOS refuses others)
		auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
		pthread_attr_setstacksize(&attributes, std::max<size_t>((stackSize + page - 1) / page * page, PTHREAD_STACK_MIN));
	}
	int error = pthread_create(&handle, &attributes, ThreadMain, start.get());
	pthread_attr_destroy(&attributes);
	if (error != 0)
		throw std::system_error(error, std::generic_category(), "jthread: pthread_create");
#endif
	std::ignore = start.release(); // the thread's now
	return {handle, id.get()};
}

void JoinThread(std::thread::native_handle_type handle)
{
#if defined(_WIN32)
	WaitForSingleObjectEx(handle, INFINITE, FALSE);
	CloseHandle(handle);
#else
	pthread_join(handle, nullptr);
#endif
}

void DetachThread(std::thread::native_handle_type handle)
{
#if defined(_WIN32)
	CloseHandle(handle);
#else
	pthread_detach(handle);
#endif
}

} // namespace core::std_extra::detail

#endif
