#pragma once

#include <core/capi.h>

#include <memory>

namespace core
{

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

void AddMouseHandler(const std::shared_ptr<MouseEventHandler>& handler);
void AddKeyboardHandler(const std::shared_ptr<KeyboardEventHandler>& handler);

} // namespace core
