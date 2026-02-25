#include "resource.h"

size_t ResourceHash::operator()(const IResource& obj) const noexcept
{
	return Hash<uuids::uuid>::operator()(obj.GetUuid());
}
