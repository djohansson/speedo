#pragma once

#include "utils.h"
#include "profiling.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
//#include <stop_token>
#include <tuple>
#include <type_traits>
#include <vector>

#include <concurrentqueue.h>

template <typename T>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T>;

template <typename T>
bool is_ready(std::future<T> const& future)
{
	return future.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
}

template <typename T, typename AtomicT = std::atomic<T>>
class CopyableAtomic : public AtomicT
{
public:
	using value_t = T;
	using atomic_t = AtomicT;
	using atomic_t::store;

	constexpr CopyableAtomic() noexcept = default;
	constexpr CopyableAtomic(value_t val) noexcept
		: atomic_t(val)
	{}
	CopyableAtomic(const CopyableAtomic& other) noexcept
	{
		store(other.load(std::memory_order_acquire), std::memory_order_release);
	}
	CopyableAtomic& operator=(const CopyableAtomic& other) noexcept
	{
		store(other.load(std::memory_order_acquire), std::memory_order_release);

		return *this;
	}
};

template <typename ValueT = uint32_t, uint32_t Aligmnent =
#if __cpp_lib_hardware_interference_size >= 201603
	std::hardware_destructive_interference_size>
#else
	64>
#endif
class alignas(Aligmnent) UpgradableSharedMutex
{
	using value_t = ValueT;
	enum : value_t
	{
		Reader = 4,
		Upgraded = 2,
		Writer = 1,
		None = 0
	};

#if __cpp_lib_atomic_ref >= 201806
	value_t myBits = 0;
	inline auto internalAtomicRef() noexcept
	{
		return std::atomic_ref(myBits);
	}
#else
	using atomic_t = CopyableAtomic<value_t>;
	atomic_t myAtomic;
	inline auto& internalAtomicRef() noexcept
	{
		return myAtomic;
	}
#endif

	template <typename Func>
	inline void internalAquireLock(Func lockFn) noexcept
	{
		ZoneScopedN("aquireLock");

		auto result = lockFn();
		auto& [success, value] = result;
		while (!success)
		{
			ZoneScopedN("waitAndTryAgain");

			internalAtomicRef().wait(value);
			result = lockFn();
		}
	}

public:
	// Lockable Concept
	void lock() noexcept
	{
		internalAquireLock([this]() { return try_lock(); });
	}

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	inline void unlock() noexcept
	{
		static_assert(Reader > Writer + Upgraded, "wrong bits!");

		internalAtomicRef().fetch_and(~(Writer | Upgraded), std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// SharedLockable Concept
	void lock_shared() noexcept
	{
		internalAquireLock([this]() { return try_lock_shared(); });
	}

	inline void unlock_shared() noexcept
	{
		internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// Downgrade the lock from writer status to reader status.
	inline void unlock_and_lock_shared() noexcept
	{
		internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);

		unlock();
	}

	// UpgradeLockable Concept
	void lock_upgrade() noexcept
	{
		internalAquireLock([this]() { return try_lock_upgrade(); });
	}

	inline void unlock_upgrade() noexcept
	{
		internalAtomicRef().fetch_add(-Upgraded, std::memory_order_acq_rel);
		internalAtomicRef().notify_all();
	}

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock() noexcept
	{
		// try to unlock upgrade and write lock atomically
		internalAquireLock([this]() { return try_lock<Upgraded>(); });
	}

	// unlock upgrade and read lock atomically
	inline void unlock_upgrade_and_lock_shared() noexcept
	{
		internalAtomicRef().fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
		internalAtomicRef().notify_all();
	}

	// write unlock and upgrade lock atomically
	inline void unlock_and_lock_upgrade() noexcept
	{
		// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
		// the same time when other threads are trying do try_lock_upgrade().

		internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);
		internalAtomicRef().fetch_add(-Writer, std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// Attempt to acquire writer permission. Return false if we didn't get it.
	template <value_t Expected = None>
	inline std::tuple<bool, value_t> try_lock() noexcept
	{
		auto result = std::make_tuple(false, Expected);
		auto& [success, value] = result;
		success = internalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
		return result;
	}

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	inline std::tuple<bool, value_t> try_lock_shared() noexcept
	{
		// fetch_add is considerably (100%) faster than compare_exchange,
		// so here we are optimizing for the common (lock success) case.

		value_t value = internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);
		if (value & (Writer | Upgraded))
		{
			value = internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
			return std::make_tuple(false, value);
		}
		return std::make_tuple(true, value);
	}

	// try to acquire an upgradable lock.
	inline std::tuple<bool, value_t> try_lock_upgrade() noexcept
	{
		value_t value = internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

		// Note: when failed, we cannot flip the Upgraded bit back,
		// as in this case there is either another upgrade lock or a write lock.
		// If it's a write lock, the bit will get cleared up when that lock's done
		// with unlock().
		return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
	}
};

class Task : public Noncopyable
{
	static constexpr size_t kMaxCallableSizeBytes = 56;
	static constexpr size_t kMaxArgsSizeBytes = 24;
	static constexpr size_t kMaxPromiseSizeBytes = 24;

public:

	constexpr Task() {};

	template <typename F, typename dF = std::decay_t<F>, typename... Args, typename ArgsTuple = std::tuple<Args...>, typename R = std::invoke_result_t<dF, Args...>, typename Promise = std::promise<R>>
	Task(F&& f, Args&&... args)
		: myInvokeThunk([](void* callablePtr, void* argsPtr, void* promisePtr)
		{
			auto& callable = *static_cast<dF*>(callablePtr);
			auto& args = *static_cast<ArgsTuple*>(argsPtr);
			auto& promise = *static_cast<Promise*>(promisePtr);
			if constexpr(std::is_void_v<R>)
			{
				std::apply(callable, args);
				promise.set_value();
			}
			else
			{
				promise.set_value(std::apply(callable, args));
		 	}
		}) // todo: args
		, myMoveThunk([](
			void* callablePtr, void* otherCallablePtr,
			void* argsPtr, void* otherArgsPtr,
			void* promisePtr, void* otherPromisePtr)
		{
			auto& otherCallable = *static_cast<dF*>(otherCallablePtr);

			new (callablePtr) dF(std::move(otherCallable));
			
			otherCallable.~dF();

			auto& otherArgs = *static_cast<ArgsTuple*>(otherArgsPtr);

			new (argsPtr) ArgsTuple(std::move(otherArgs));
			
			otherArgs.~ArgsTuple();

			auto& otherPromise = *static_cast<Promise*>(otherPromisePtr);

			new (promisePtr) Promise(std::move(otherPromise));

			otherPromise.~Promise();
		})
		, myDeleteThunk([](void* callablePtr, void* argsPtr, void* promisePtr)
		{
			auto& callable = *static_cast<dF*>(callablePtr);
			callable.~dF();

			auto& args = *static_cast<ArgsTuple*>(argsPtr);
			args.~ArgsTuple();

			auto& promise = *static_cast<Promise*>(promisePtr);
			promise.~Promise();
		})
	{	
		static_assert(sizeof(dF) <= kMaxCallableSizeBytes);

		new (myCallableMemory.data()) dF(std::forward<F>(f));

		static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);

		new (myArgsMemory.data()) ArgsTuple(std::forward<Args>(args)...);

		static_assert(sizeof(Promise) <= kMaxPromiseSizeBytes);

		new (myPromiseMemory.data()) Promise();

		static_assert(sizeof(Task) == 128);
	}

	Task(Task&& other) noexcept	{ *this = std::move(other);	}

	~Task()
	{
		if (myDeleteThunk)
			myDeleteThunk(myCallableMemory.data(), myArgsMemory.data(), myPromiseMemory.data());
	}

	Task& operator=(Task&& other) noexcept
	{
		if (myDeleteThunk)
			myDeleteThunk(myCallableMemory.data(), myArgsMemory.data(), myPromiseMemory.data());
		
		myInvokeThunk = std::exchange(other.myInvokeThunk, nullptr);
		myMoveThunk = std::exchange(other.myMoveThunk, nullptr);
		myDeleteThunk = std::exchange(other.myDeleteThunk, nullptr);

		if (myMoveThunk)
			myMoveThunk(
				myCallableMemory.data(), other.myCallableMemory.data(),
				myArgsMemory.data(), other.myArgsMemory.data(), 
				myPromiseMemory.data(), other.myPromiseMemory.data());

		return *this;
	}

	inline void operator()()
	{
		assert(myInvokeThunk);
		
		myInvokeThunk(myCallableMemory.data(), myArgsMemory.data(), myPromiseMemory.data());
	}

	template <typename R, typename Promise = std::promise<R>>
	inline std::future<R> getFuture()
	{
		static_assert(sizeof(Promise) <= kMaxPromiseSizeBytes);
		// todo: more type checking?

		auto& promise = *static_cast<Promise*>(static_cast<void*>(myPromiseMemory.data()));
		
		return promise.get_future();
	}

private:

	// cache line 1
	void (*myInvokeThunk)(void*, void*, void*) = nullptr;
	alignas(intptr_t) AlignedArray<kMaxCallableSizeBytes> myCallableMemory;
	// cache line 2
	alignas(intptr_t) AlignedArray<kMaxArgsSizeBytes> myArgsMemory;
	alignas(intptr_t) AlignedArray<kMaxPromiseSizeBytes> myPromiseMemory;
	void (*myMoveThunk)(void*, void*, void*, void*, void*, void*) = nullptr;
	void (*myDeleteThunk)(void*, void*, void*) = nullptr;
	//
};

class TaskThreadPool : public Noncopyable
{
	using mutex_t = UpgradableSharedMutex<>;
	//using mutex_t = std::mutex; // todo: compare performance

public:

	TaskThreadPool(uint32_t threadCount)
	{
		ZoneScopedN("TaskThreadPool()");

		assertf(threadCount > 0, "Thread count must be nonzero");

		auto threadMain = [this]
		{
			//auto stopToken = myStopSource.get_token();

			internalThreadMain(/*stopToken*/);
		};
		
		myThreads.reserve(threadCount);
		for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
			myThreads.emplace_back(threadMain);
	}

	~TaskThreadPool()
	{
		//myStopSource.request_stop();
		myStopSource.store(true, std::memory_order_relaxed);
		mySignal.notify_all();

		// workaround for the following problem in msvc implementation of jthread in <mutex>:
		// TRANSITION, ABI: Due to the unsynchronized delivery of notify_all by _Stoken,
		// this implementation cannot tolerate *this destruction while an interruptible wait
		// is outstanding. A future ABI should store both the internal CV and internal mutex
		// in the reference counted block to allow this.
		{
			ZoneScopedN("~TaskThreadPool()::join");

			for (auto& thread : myThreads)
				thread.join();
		}
	}

	template <typename F, typename... Args, typename R = std::invoke_result_t<F, Args...>>
	std::future<R> submit(F&& callable, Args&&... args)
	{
		ZoneScopedN("TaskThreadPool::submit");

		Task task(std::forward<F>(callable), std::forward<Args>(args)...);
		auto future = task.template getFuture<R>();

		{
			ZoneScopedN("TaskThreadPool::submit::enqueue");

			myQueue.enqueue(std::move(task));
		}

		{
			ZoneScopedN("TaskThreadPool::submit::signal");
		
			mySignal.notify_one();
		}

		return future;
	}

	void processQueue()
	{
		ZoneScopedN("TaskThreadPool::processQueue");

		internalProcessQueue();
	}

	template <typename T, typename R = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>>
	std::optional<R> processQueueUntil(std::future<T>&& future)
	{
		ZoneScopedN("TaskThreadPool::processQueueUntil");

		return internalProcessQueue(std::forward<std::future<T>>(future));
	}

private:

	void internalProcessQueue()
	{
		Task task;
		while (myQueue.try_dequeue(task))
			task();
	}

	template <typename T, typename R = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>>
	std::optional<R> internalProcessQueue(std::future<T>&& future)
	{
		if (!future.valid())
			return std::nullopt;

		auto returnFunc = [](auto& future)
		{
			if constexpr(std::is_void_v<T>)
			{
				future.get();
				return std::make_optional<R>();
			}
			else
			{
				return std::make_optional(future.get());
			}
		};

		Task task;
		while (myQueue.try_dequeue(task))
		{
			if (is_ready(future))
				return returnFunc(future);

			task();
		}

		return returnFunc(future);
	}

	void internalThreadMain(/*std::stop_token& stopToken*/)
	{		
		//while (!stopToken.stop_requested())
		while (!myStopSource.load(std::memory_order_relaxed))
		{
			internalProcessQueue();

			auto lock = std::unique_lock(myMutex);

			//mySignal.wait(lock, stopToken, [&queue = myQueue](){ return !queue.empty(); });
			mySignal.wait(lock, [&stopSource = myStopSource, &queue = myQueue](){ return stopSource.load(std::memory_order_relaxed) || queue.size_approx(); });
		}
	}

	mutex_t myMutex;
	//std::vector<std::jthread> myThreads;
	std::vector<std::thread> myThreads;
	std::condition_variable_any mySignal;
	//std::stop_source myStopSource;
	std::atomic_bool myStopSource;
	ConcurrentQueue<Task> myQueue;
};
