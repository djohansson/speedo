#include <cereal/cereal.hpp>
#include <cereal/types/array.hpp>
#include <cereal/types/string.hpp>

#include <mio/mmap_iostream.hpp>

template <class Archive>
void load(Archive& archive, FileInfo& info)
{
    archive(cereal::make_nvp("path", info.path));
    archive(cereal::make_nvp("size", info.size));
    archive(cereal::make_nvp("timeStamp", info.timeStamp));
    archive.loadBinaryValue(info.sha2.data(), info.sha2.size(), "sha2");
}

template <class Archive>
void save(Archive& archive, const FileInfo& info)
{
    archive(cereal::make_nvp("path", info.path));
    archive(cereal::make_nvp("size", info.size));
    archive(cereal::make_nvp("timeStamp", info.timeStamp));
    archive.saveBinaryValue(info.sha2.data(), info.sha2.size(), "sha2");
}

template <typename T, typename Archive>
T loadObject(std::istream& stream, std::string_view name)
{
    Archive archive(stream);
    T outValue = {};
    archive(cereal::make_nvp(name.data(), outValue));
    return outValue;
};

template <typename T, typename Archive>
void saveObject(const T& object, std::ostream& stream, std::string_view name)
{
    Archive json(stream);
    json(cereal::make_nvp(name.data(), object));
};

template <typename T, typename Archive>
std::tuple<std::optional<T>, FileState> loadObject(const std::filesystem::path& filePath, std::string_view name)
{
    auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(std::nullopt, FileState::Missing);

    auto fileStream = mio::mmap_istream(filePath.string());
    
    return std::make_tuple(std::make_optional(loadObject<T, Archive>(fileStream, name)), FileState::Valid);
}

template <typename T, typename Archive>
void saveObject(const T& object, const std::filesystem::path& filePath, std::string_view name)
{
    auto fileStream = mio::mmap_ostream(filePath.string());
	saveObject<T, Archive>(object, fileStream, name);
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::FileObject(
	const std::filesystem::path& filePath,
	std::string_view name,
	T&& defaultObject)
: T(std::get<0>(loadObject<T, InputArchive>(filePath, name)).value_or(std::move(defaultObject)))
, myFilePath(filePath)
, myName(name)
{
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::FileObject(FileObject&& other) noexcept
: T(std::move(other))
, myFilePath(std::exchange(other.myFilePath, {}))
, myName(std::exchange(other.myName, {}))
{
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::~FileObject()
{
	if constexpr(SaveOnClose)
		if (!myFilePath.empty())
			save();
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>& FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::operator=(
    FileObject&& other) noexcept
{
    myFilePath = std::exchange(other.myFilePath, {});
    myName = std::exchange(other.myName, {});
	return *this;
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
void FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::swap(FileObject& rhs) noexcept
{
    std::swap<T>(*this, rhs);
    std::swap(myFilePath, rhs.myFilePath);
    std::swap(myName, rhs.myName);
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
void FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::reload()
{
	static_cast<T&>(*this) = std::get<0>(
		loadObject<T, InputArchive>(myFilePath, myName)).value_or(std::move(static_cast<T&>(*this)));
}

template <typename T, FileAccessMode Mode, typename InputArchive, typename OutputArchive, bool SaveOnClose>
template <FileAccessMode M>
typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::save() const
{
    saveObject<T, OutputArchive>(static_cast<const T&>(*this), myFilePath, myName);
}
