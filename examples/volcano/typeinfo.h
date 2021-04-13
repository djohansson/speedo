#pragma once

#include <string_view>

template <class T>
consteval std::string_view getTypeName()
{
#if defined(__clang__) || defined(__GNUC__)
	const char* strBegin = __PRETTY_FUNCTION__;
	const char* strEnd = strBegin + sizeof(__PRETTY_FUNCTION__);
	const char* strPtr = strBegin;

	while (*strPtr++ != '=');

	for (; *strPtr == ' '; ++strPtr);

	char const* strPtr2 = strPtr;
	for (size_t depth = 1; strPtr2 != strEnd; ++strPtr2)
	{
		switch (*strPtr2)
		{
		case '[':
			++depth;
			break;
		case ']':
			--depth;
			if (!depth)
				return {strPtr, strPtr2};
		}
	}
#else
	static_assert(false, "Please implement me!");
#endif

	return {};
}
