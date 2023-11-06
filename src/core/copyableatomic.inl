template <typename T, typename AtomicT>
constexpr CopyableAtomic<T, AtomicT>::CopyableAtomic(
	CopyableAtomic<T, AtomicT>::value_t val) noexcept
	: AtomicT(val)
{}

template <typename T, typename AtomicT>
CopyableAtomic<T, AtomicT>::CopyableAtomic(const CopyableAtomic& other) noexcept
{
	store(other.load(std::memory_order_acquire), std::memory_order_release);
}

template <typename T, typename AtomicT>
CopyableAtomic<T, AtomicT>&
CopyableAtomic<T, AtomicT>::operator=(const CopyableAtomic& other) noexcept
{
	store(other.load(std::memory_order_acquire), std::memory_order_release);

	return *this;
}
