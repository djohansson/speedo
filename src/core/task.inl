#include <core/assert.h>//NOLINT(modernize-deprecated-headers)

#include <utility>

namespace core
{

namespace detail
{

Task* InternalHandleToPtr(TaskHandle handle) noexcept;
TaskHandle InternalAllocate() noexcept;
void InternalFree(TaskHandle handle) noexcept;

} // namespace detail

template <typename F, typename... Args>
auto CreateTask(F&& callable, Args&&... args) noexcept
{
	using C = std::decay_t<F>;
	static_assert(
		std::is_invocable_v<C&, std::decay_t<Args>&&...>,
		"CreateTask: the callable can't be called with its arguments, decayed (pass a reference as std::ref)");
	using R = std::invoke_result_t<C&, std::decay_t<Args>&&...>;
	using State = typename Future<R>::FutureState;

	auto state = std::make_shared<State>();
	auto handle = core::detail::InternalAllocate(); // grows the pool, or traps past its most
	std::construct_at(
		core::detail::InternalHandleToPtr(handle),
		Task::PrivateTag{},
		Task::Function(
			[callable = std::forward<F>(callable), ... args = std::forward<Args>(args)](TaskState& taskState) mutable
			{
				if constexpr (std::is_void_v<R>)
					std::invoke(callable, std::move(args)...);
				else
					static_cast<State&>(taskState).value = std::invoke(callable, std::move(args)...);
			}),
		std::shared_ptr<TaskState>(state));

	return TaskCreateInfo<R>{.handle = handle, .future = Future<R>(std::move(state))};
}

} // namespace core
