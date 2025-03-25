#pragma once

#include "capi.h"

#include <memory>

class MouseEventHandler
{
public:
	virtual void OnMouse(const MouseEvent& mouse) = 0;
};

class KeyboardEventHandler
{
public:
	virtual void OnKeyboard(const KeyboardEvent& keyboard) = 0;
};

namespace core
{

void AddMouseHandler(const std::shared_ptr<MouseEventHandler>& handler);
void AddKeyboardHandler(const std::shared_ptr<KeyboardEventHandler>& handler);

} // namespace core
