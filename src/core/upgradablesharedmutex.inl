template <typename Func>
void UpgradableSharedMutex::InternalAquireLock(Func lockFn) noexcept
{
	auto atomic = InternalAtomicRef();
	auto result = lockFn();
	auto& [success, value] = result;
	while (!success)
	{
		atomic.wait(value);
		result = lockFn();
	}
}

template <typename UpgradableSharedMutex::value_t Expected>
std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::InternalTryLock() noexcept
{
	auto atomic = InternalAtomicRef();
	auto result = std::make_tuple(false, Expected);
	auto& [success, value] = result;
	success = atomic.compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
	return result;
}
