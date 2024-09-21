#pragma once

#include <core/utils.h>

#include <stduuid/uuid.h>

struct IResource
{
	virtual ~IResource() = default;

	[[nodiscard]] virtual uuids::uuid GetUUID() const noexcept = 0;
};

struct ResourceHash : ankerl::unordered_dense::hash<uuids::uuid>
{
	[[nodiscard]] size_t operator()(const IResource& obj) const noexcept;
	
	using is_transparent = std::true_type;
};

using ResourceTable = UnorderedSet<IResource, ResourceHash>;
