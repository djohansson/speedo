#include "profiling.h"
#include "typeinfo.h"

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
	archive(cereal::make_nvp(std::string(name), outValue));
	return outValue;
};

template <typename T, typename Archive>
void saveObject(const T& object, std::ostream& stream, std::string_view name)
{
	Archive json(stream);
	json(cereal::make_nvp(std::string(name), object));
};

template <typename T, typename Archive>
std::tuple<std::optional<T>, FileState>
loadObject(const std::filesystem::path& filePath, std::string_view name)
{
	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(std::nullopt, FileState::Missing);

	auto fileStream = mio::mmap_istream(filePath.string());

	return std::make_tuple(
		std::make_optional(loadObject<T, Archive>(fileStream, name)), FileState::Valid);
}

template <typename T, typename Archive>
void saveObject(const T& object, const std::filesystem::path& filePath, std::string_view name)
{
	auto fileStream = mio::mmap_ostream(filePath.string());
	saveObject<T, Archive>(object, fileStream, name);
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::FileObject(
	const std::filesystem::path& filePath, T&& defaultObject)
	: T(std::get<0>(loadObject<T, InputArchive>(filePath, getTypeName<std::decay_t<T>>()))
			.value_or(std::forward<T>(defaultObject)))
	, myInfo{filePath}
{}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::FileObject(
	FileObject&& other) noexcept
	: T(std::forward<FileObject>(other))
	, myInfo(std::exchange(other.myInfo, {}))
{}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::~FileObject()
{
	if constexpr (SaveOnClose)
		if (!myInfo.path.empty())
			save();
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>&
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::operator=(
	FileObject&& other) noexcept
{
	myInfo = std::exchange(other.myInfo, {});
	return *this;
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
void FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::swap(FileObject& rhs) noexcept
{
	std::swap<T>(*this, rhs);
	std::swap(myInfo, rhs.myInfo);
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
void FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::reload()
{
	static_cast<T&>(*this) =
		std::get<0>(loadObject<T, InputArchive>(myInfo.path, getTypeName<std::decay_t<T>>()))
			.value_or(std::move(static_cast<T&>(*this)));
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
template <FileAccessMode M>
typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::save() const
{
	saveObject<T, OutputArchive>(
		static_cast<const T&>(*this), myInfo.path, getTypeName<std::decay_t<T>>());
}

template <const char* Id, const char* LoaderType, const char* LoaderVersion>
std::tuple<FileState, FileInfo> getFileInfo(
	const std::filesystem::path& filePath,
	std::istream& jsonStream,
	LoadFileInfoFromJSONFn loadJSONFn,
	bool sha2Enable)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(FileState::Missing, FileInfo{});

	auto [loaderTypeStr, loaderVersionStr, fileInfoStr] =
		loadJSONFn(jsonStream, std::string_view(Id));

	if (std::string_view(LoaderType).compare(loaderTypeStr) != 0 ||
		std::string_view(LoaderVersion).compare(loaderVersionStr) != 0)
		return std::make_tuple(FileState::Stale, FileInfo{});

	auto outFileInfo =
		FileInfo{filePath, std::filesystem::file_size(filePath), getFileTimeStamp(filePath)};

	if (sha2Enable)
	{
		ZoneScopedN("getFileInfo::sha2");

		mio::mmap_source file(filePath.string());
		picosha2::hash256(
			file.begin(), file.end(), outFileInfo.sha2.begin(), outFileInfo.sha2.end());
	}

	// perhaps add path check as well?
	if (outFileInfo.size != fileInfoStr.size ||
				outFileInfo.timeStamp.compare(fileInfoStr.timeStamp) != 0 || sha2Enable
			? outFileInfo.sha2 != fileInfoStr.sha2
			: false)
		return std::make_tuple(FileState::Stale, FileInfo{});

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

template <const char* LoaderType, const char* LoaderVersion>
void loadCachedSourceFile(
	const std::filesystem::path& sourceFilePath,
	const std::filesystem::path& cacheFilePath,
	LoadFileFn loadSourceFileFn,
	LoadFileFn loadBinaryCacheFn,
	SaveFileFn saveBinaryCacheFn)
{
	ZoneScoped;

	std::filesystem::path jsonFilePath(sourceFilePath);
	jsonFilePath += ".bin.json";

	std::filesystem::path binFilePath(cacheFilePath);
	binFilePath += ".bin";

	bool doImport;
	std::tuple<FileState, FileInfo> sourceFile, binFile;
	auto jsonFileStatus = std::filesystem::status(jsonFilePath);
	auto binFileStatus = std::filesystem::status(binFilePath);

	if (std::filesystem::exists(jsonFileStatus) &&
		std::filesystem::is_regular_file(jsonFileStatus) && !std::filesystem::exists(binFileStatus))
	{
		ZoneScopedN("loadCachedSourceFile::deleteJson");

		std::filesystem::remove(jsonFilePath);

		doImport = true;
	}
	else if (
		std::filesystem::exists(jsonFileStatus) && std::filesystem::is_regular_file(jsonFileStatus))
	{
		ZoneScopedN("loadCachedSourceFile::readJsonAndFileState");

		auto loadJSONFn = [](std::istream& stream, std::string_view id)
		{
			cereal::JSONInputArchive json(stream);

			std::string outLoaderType;
			std::string outLoaderVersion;
			FileInfo outFileInfo;

			json(cereal::make_nvp("loaderType", outLoaderType));
			json(cereal::make_nvp("loaderVersion", outLoaderVersion));
			json(cereal::make_nvp(id.data(), outFileInfo));

			return std::make_tuple(outLoaderType, outLoaderVersion, outFileInfo);
		};

		doImport = false;

		auto fileStream = mio::mmap_istream(jsonFilePath.string());

		static constexpr char sourceFileInfoIdStr[] = "sourceFileInfo";
		sourceFile = getFileInfo<sourceFileInfoIdStr, LoaderType, LoaderVersion>(
			sourceFilePath, fileStream, loadJSONFn, false);

		fileStream.clear();
		fileStream.seekg(0, std::ios_base::beg);

		static constexpr char binFileInfoIdStr[] = "binFileInfo";
		binFile = getFileInfo<binFileInfoIdStr, LoaderType, LoaderVersion>(
			binFilePath, fileStream, loadJSONFn, false);
	}
	else
	{
		doImport = true;
	}

	auto& [sourceFileState, sourceFileInfo] = sourceFile;
	auto& [binFileState, binFileInfo] = binFile;

	if (doImport || sourceFileState == FileState::Stale || binFileState != FileState::Valid)
	{
		ZoneScopedN("loadCachedSourceFile::importSourceFile");

		auto fileStream = mio::mmap_ostream(jsonFilePath.string());
		cereal::JSONOutputArchive json(fileStream);

		static const std::string loaderTypeStr(LoaderType);
		static const std::string loaderVersionStr(LoaderVersion);

		json(cereal::make_nvp("loaderType", loaderTypeStr));
		json(cereal::make_nvp("loaderVersion", loaderVersionStr));

		auto [sourceFileState, sourceFileInfo] =
			loadBinaryFile(sourceFilePath, loadSourceFileFn, true);
		json(CEREAL_NVP(sourceFileInfo));

		auto [binFileState, binFileInfo] = saveBinaryFile(binFilePath, saveBinaryCacheFn, true);
		json(CEREAL_NVP(binFileInfo));
	}
	else
	{
		ZoneScopedN("loadCachedSourceFile::loadBin");

		auto [binFileState, binFileInfo] = loadBinaryFile(binFilePath, loadBinaryCacheFn, false);
	}
}
