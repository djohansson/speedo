#include "eventhandlers.h"

#include <vector>

namespace core
{

namespace detail
{

std::vector<std::weak_ptr<MouseEventHandler>> gMouseHandlers;
std::vector<std::weak_ptr<KeyboardEventHandler>> gKeyboardHandlers;

} // namespace detail

void AddMouseHandler(const std::shared_ptr<MouseEventHandler>& handler)
{
	using namespace detail;
	
	gMouseHandlers.push_back(handler);
}

void AddKeyboardHandler(const std::shared_ptr<KeyboardEventHandler>& handler)
{
	using namespace detail;

	gKeyboardHandlers.push_back(handler);
}

} // namespace core

void UpdateMouse(const MouseEvent* state)
{
	for (auto& handler : core::detail::gMouseHandlers)
		if (auto h = handler.lock(); h)
			h->OnMouse(*state);
}

void UpdateKeyboard(const KeyboardEvent* state)
{
	for (auto& handler : core::detail::gKeyboardHandlers)
		if (auto h = handler.lock(); h)
			h->OnKeyboard(*state);
}
