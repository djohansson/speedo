template <typename Func>
void UpgradableSharedMutex::InternalAquireLock(Func lockFn) noexcept
{
	auto result = lockFn();
	auto& [success, value] = result;
	while (!success)
	{
		InternalAtomicRef().wait(value);
		result = lockFn();
	}
}

template <typename UpgradableSharedMutex::value_t Expected>
std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::InternalTryLock() noexcept
{
	auto result = std::make_tuple(false, Expected);
	auto& [success, value] = result;
	success = InternalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
	return result;
}
