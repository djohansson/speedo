#pragma once

#include "command.h"
#include "device.h"
#include "instance.h"
#include "types.h"

#include <memory>
#include <vector>

template <GraphicsBackend B>
struct ApplicationContext
{
    std::shared_ptr<InstanceContext<B>> instance;
	std::shared_ptr<DeviceContext<B>> device;
    std::shared_ptr<CommandContext<B>> computeCommands;
	std::shared_ptr<CommandContext<B>> transferCommands;
};
