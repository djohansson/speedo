#pragma once

#include "nodecommon.h"

class InputOutputNode : public INode
{
public:
	InputOutputNode() noexcept = default;
	InputOutputNode(int id, std::string&& name);
	InputOutputNode(const InputOutputNode&) = default;
	InputOutputNode(InputOutputNode&&) noexcept = default;
	~InputOutputNode() = default;

	InputOutputNode& operator=(const InputOutputNode&) = default;
	InputOutputNode& operator=(InputOutputNode&&) noexcept = default;

	void Swap(InputOutputNode& rhs) noexcept;
	friend void Swap(InputOutputNode& lhs, InputOutputNode& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] int& Id() final;
	[[nodiscard]] std::optional<int>& Selected() final;
	[[nodiscard]] std::string& GetName() final;

	[[nodiscard]] std::vector<Attribute>& InputAttributes();
	[[nodiscard]] std::vector<Attribute>& OutputAttributes();

private:
	int myId = 0;
	std::optional<int> mySelected;
	std::string myName;
	std::vector<Attribute> myInputAttributes;
	std::vector<Attribute> myOutputAttributes;
};
