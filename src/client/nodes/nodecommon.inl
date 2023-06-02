#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

template <class Archive>
void serialize(Archive& archive, Attribute& attribute)
{
    archive(
        cereal::make_nvp("id", attribute.id),
        cereal::make_nvp("name", attribute.name));
}

template <class Archive>
void serialize(Archive& archive, Link& link)
{
    archive(
        cereal::make_nvp("fromId", link.fromId),
        cereal::make_nvp("toId", link.toId));
}
