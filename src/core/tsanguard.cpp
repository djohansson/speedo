// tsan on macOS: the Itanium C++ ABI's guards of function local statics (__cxa_guard_acquire, release and abort), compiled
// into each of our executables and shared libraries in a tsan build (see SPEEDO_SANITIZE in CMakeLists.txt), so that tsan
// sees a static's initialization published to the threads that read it. Our binaries bound them from the first C++ dylib
// exporting them (libktx's, cpptrace's: each links libc++abi statically, see docs/notes/build.md), not from the system's
// libc++abi, whose functions alone tsan interposes: it reported the threads reading a static racing the one that had
// initialized it (gfx::bvh::Version's, read by parallel model loads), and twice in a quick tier such a report, symbolized
// while a Metal thread held dyld's lock in a dlopen, deadlocked the client.
//
// The compiler checks a guard's first byte inline (an acquire load, which tsan instruments): 1 once the static is
// initialized. The second byte marks an initialization in progress, which other threads wait on. A recursive
// initialization (undefined behaviour, which libc++abi aborts on) waits forever here.

#include <atomic>
#include <cstdint>

namespace
{

constexpr uint64_t kInitialized = 1; // the first byte (little endian), which the compiler reads
constexpr uint64_t kInitializing = uint64_t{1} << 8;

} // namespace

extern "C" int __cxa_guard_acquire(uint64_t* guard)
{
	std::atomic_ref<uint64_t> state(*guard);
	uint64_t value = state.load(std::memory_order_acquire);
	while (true)
	{
		if ((value & kInitialized) != 0)
			return 0;
		if (value == 0)
		{
			if (state.compare_exchange_weak(value, kInitializing, std::memory_order_acquire))
				return 1;
			continue;
		}
		state.wait(value, std::memory_order_acquire);
		value = state.load(std::memory_order_acquire);
	}
}

extern "C" void __cxa_guard_release(uint64_t* guard)
{
	std::atomic_ref<uint64_t> state(*guard);
	state.store(kInitialized, std::memory_order_release);
	state.notify_all();
}

extern "C" void __cxa_guard_abort(uint64_t* guard)
{
	std::atomic_ref<uint64_t> state(*guard);
	state.store(0, std::memory_order_release);
	state.notify_all();
}
