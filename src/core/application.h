#pragma once

#include <string_view>

#include <core/utils.h>

class ApplicationBase : public Noncopyable
{
protected:
	ApplicationBase();
	virtual ~ApplicationBase() = default;

public:
	virtual std::string_view getName() const = 0;
};
