template <typename Func>
void UpgradableSharedMutex::internalAquireLock(Func lockFn) noexcept
{
	auto result = lockFn();
	auto& [success, value] = result;
	while (!success)
	{
		internalAtomicRef().wait(value);
		result = lockFn();
	}
}

template <typename UpgradableSharedMutex::value_t Expected>
std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::internalTryLock() noexcept
{
	auto result = std::make_tuple(false, Expected);
	auto& [success, value] = result;
	success = internalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
	return result;
}
