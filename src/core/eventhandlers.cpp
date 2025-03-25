#include "eventhandlers.h"

#include <vector>

namespace core
{

std::vector<std::weak_ptr<MouseEventHandler>> gMouseHandlers;
std::vector<std::weak_ptr<KeyboardEventHandler>> gKeyboardHandlers;

void AddMouseHandler(const std::shared_ptr<MouseEventHandler>& handler)
{
	gMouseHandlers.push_back(handler);
}

void AddKeyboardHandler(const std::shared_ptr<KeyboardEventHandler>& handler)
{
	gKeyboardHandlers.push_back(handler);
}

} // namespace core

void UpdateMouse(const MouseEvent* state)
{
	for (auto& handler : core::gMouseHandlers)
		if (auto h = handler.lock(); h)
			h->OnMouse(*state);
}

void UpdateKeyboard(const KeyboardEvent* state)
{
	for (auto& handler : core::gKeyboardHandlers)
		if (auto h = handler.lock(); h)
			h->OnKeyboard(*state);
}
