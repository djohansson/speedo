#include "resource.h"

size_t ResourceHash::operator()(const IResource& obj) const noexcept
{
	return ankerl::unordered_dense::hash<uuids::uuid>::operator()(obj.GetUuid());
}
