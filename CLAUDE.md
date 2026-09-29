# CLAUDE.md

Notes for working in this codebase, distilled from real build failures.

## rhi: `Object<T>` / `DeviceObject<T>` base classes

Most `rhi` types (`Buffer`, `Fence`, `Queue`, `Pipeline`, `Window`, ...) derive from
`Object<DerivedType>` or `DeviceObject<DerivedType>` (`src/rhi/object.h`,
`src/rhi/deviceobject.h`). This is a CRTP base, not a normal template: `GetInstance()`
and `GetDevice()` are declared in the base but have **no generic definition** — each
concrete type must explicitly specialize them via macros, once, in its own `.cpp`:

```cpp
IMPLEMENT_OBJECT_GETINSTANCE(Foo<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Foo<kVk>);   // only if Foo derives from DeviceObject
```

If a new type deriving from `Object`/`DeviceObject` is added (or one is spotted missing
these), forgetting this produces linker errors like
`undefined symbol: rhi::Object<rhi::Foo<...>>::GetInstance() const` — it is *not* a sign
that something else is broken, just that the macro invocation is missing. These macros
expand to code that calls `RHIApplication::GetRHI<G>()`, so the `.cpp` needs
`#include <rhi/rhiapplication.h>`.

## Move-via-Swap idiom

Types in this hierarchy that need to be movable (e.g. because they live in a
`std::vector`, like `Device` in `RHI::myDevices`) follow a consistent pattern rather than
defaulted move members:

```cpp
void Foo::Swap(Foo& other) noexcept { SuperType::Swap(other); std::swap(myX, other.myX); ... }
Foo::Foo(Foo&& other) noexcept { Swap(other); }          // default-constructs *this*, then swaps
Foo& Foo::operator=(Foo&& other) noexcept { Swap(other); return *this; }
```

After a swap, the moved-from object holds the *default-constructed* state (because the
move constructor default-constructs `this` before swapping). Destructors on these types
must guard on `IsValid()` (from `Object::IsValid()`, true iff `GetDesc().instance` is
set) before doing any GPU-handle cleanup, otherwise a moved-from object double-frees or
operates on null handles.

Not everything in this hierarchy is meant to be movable — e.g. `Instance` explicitly
`= delete`s its move constructor since it's a true singleton. Check how the type is
actually used (is it stored by value in a container that reallocates?) before assuming
it needs Swap/move support added.

## Gotcha: explicit specialization ordering

When a `.cpp` file both defines an explicit template specialization *and* calls that
same specialization earlier in the file, the compiler implicitly instantiates the
primary template at the call site — and then errors with
`explicit specialization of 'X' after instantiation` when it later hits the `template <>`
definition. Two concrete ways this bites in this codebase:

1. **Swap-based move members**: define `Swap()`'s specialization *before* the move
   constructor / move-assignment specializations that call `Swap(other)` in the same
   file, not after.
2. **`IMPLEMENT_OBJECT_GETINSTANCE`/`IMPLEMENT_DEVICEOBJECT_GETDEVICE`**: if the same
   `.cpp` also defines explicit specializations of functions the macro calls internally
   (e.g. `rhiapplication.cpp` defining `RHIApplication::GetRHI<kVk>()`), the macro
   invocation must come *after* those specializations, not before.

## Gotcha: delegating constructors run their arguments before the base exists

Many types create their GPU handle inside the argument list of a delegating constructor:

```cpp
Foo<kVk>::Foo(CreateDescType&& desc)
	: Foo(std::forward<CreateDescType>(desc), [this, &desc] { /* create handle */ }())
{}
```

Those arguments are evaluated *before* the target constructor initializes the
`DeviceObject` base, so inside them `GetDesc()`, `InternalGetDesc()`, members, and the
no-argument `GetDevice()` (which falls back to `GetDesc().device`) all read uninitialized
memory. Read from the incoming `desc` instead: `desc.device` where a `VkDevice` is enough,
`GetDevice(desc.device)` where the `Device` object is needed (e.g. `.GetAllocator()`), and
`CreateDeviceObjectCreateDesc(name, desc.device)` for child objects. `GetInstance()` is fine
since it does not depend on object state. Don't pass references *into* `desc` either (e.g. a
field bound to a `const&` parameter): `desc` is moved into the base before the target's
members are initialized, so pass a copy.

Related: `DeviceObject<T>::GetDevice()` resolves through
`RHIApplication::GetRHI<G>().GetDevice(handle)`, which only finds devices already stored in
`RHI::myDevices`. That's why `Device` creates its queues and pipeline from the `RHI`
constructor, after the devices are registered, rather than from its own constructor. A
failed lookup trips the `ASSERTF` in `RHI::GetDevice()`.

## Application lifecycle: create and destroy while registered in `gApplication`

Device objects find their instance/device through `core::Application::Get()`, which locks the
`core::gApplication` weak_ptr. Two rules follow from that:

- **Create with `core::CreateApplication<T>(args...)`** (`src/core/application.h`). It publishes
  the application in `gApplication` *before* constructing it (objects created during construction
  need `Get()`), and constructs it exactly once in place. Don't recreate the old
  `make_shared_for_overwrite` + `std::construct_at` pattern: it constructs a default object first
  and never destroys it, which leaked a second `TaskExecutor` and its threads. The default
  constructors of `Application`/`RHIApplication`/`Client`/`Server`/`RHI` were removed so that
  pattern no longer compiles.
- **Tear down before releasing the last `shared_ptr`.** Once the strong count hits zero, `Get()`
  returns null even though destructors are still running, and `GetDevice()`/`GetInstance()`
  silently fall back to null objects ("Invalid device" from the loader). So `ClientDestroy` calls
  `RHIApplication::Shutdown()` (joins all tasks, waits for the GPU, flushes timeline callbacks,
  shuts down imgui, destroys the RHI) *before* `reset()`, and `ServerDestroy` joins the executor
  first. `~RHIApplication` only checks that `Shutdown()` ran.

Related gotchas hit while getting shutdown right:

- `TaskExecutor::JoinAll()` waits until nothing is queued *or executing*; call it from a regular
  thread, never from inside a task.
- Destructor bodies run *before* members are destroyed: if a body destroys the parent handle
  (`vkDestroyDevice`, `vmaDestroyAllocator`, `vkDestroyCommandPool`), release the members owning
  children of it first (see `~Device`, `~CommandPool`).
- Don't `myRHI.reset()`: it nulls the pointer before `~RHI` runs, while objects being destroyed
  still resolve devices through `GetRHI()`. `Shutdown()` does `delete myRHI.get()` then
  `release()`.
- Task chains (`Rpc`/`Tick`/`Draw` in client/server) must reach `SetTaskDone` on *every* exit path,
  including errors, and `*Destroy` stops them with `RequestTaskStop`/`WaitTaskStopped` (an
  `exchange`, so an already-ended chain isn't waited on forever).
- The signal handlers re-raise fatal signals with `SIG_DFL`; returning from e.g. SIGSEGV re-runs
  the faulting instruction and loops in the handler forever, which looks like a hang.

## `size_t`-keyed maps expecting hashed names

`Device::GetPipelineLayoutHandles()` (and similar name-keyed lookup tables) key on
`size_t` name hashes, not strings — see `GetPipelineLayoutHandle(std::string_view)` in
`src/rhi/device.h` which does `std::hash<std::string_view>{}(name)`. When inserting into
one of these maps directly (e.g. via `.emplace(...)`), hash the string first; passing a
raw string literal compiles a `pair<size_t, T>` construction attempt that only fails at
template-instantiation depth deep inside `<vector>`/`<unordered_dense.h>`, far from the
actual mistake.
