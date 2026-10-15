#pragma once

#include <core/std_extra.h>

namespace core
{

// whether a long running operation should stop (e.g. the application is exiting), which it checks as it goes and then
// fails. called during the call it is passed to (see std_extra::function_ref): bind a callable that outlives it
using CancelledFn = std_extra::function_ref<bool()>;

// for operations that can't be cancelled (CancelledFn can't be empty)
[[nodiscard]] inline bool NeverCancelled() noexcept { return false; }

} // namespace core
