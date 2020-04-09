#pragma once

#include "gfx-types.h"
#include "utils.h"

#include <any>

template <GraphicsBackend B>
class InstanceContext : Noncopyable
{
public:

    InstanceContext(InstanceCreateDesc<B>&& desc);
    ~InstanceContext();

    const auto& getDesc() const { return myDesc; }
    const auto getInstance() const { return myInstance; }

private:

    const InstanceCreateDesc<B> myDesc;
    InstanceHandle<B> myInstance;
    std::any myUserData;
};
