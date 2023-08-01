#include "profiling.h"
#include "typeinfo.h"

#include <client/client.h> // TODO: eliminate this dependency

#include <cereal/cereal.hpp>
#include <cereal/types/array.hpp>
#include <cereal/types/string.hpp>

#include <mio/mmap_iostream.hpp>

#include <picosha2.h>

// template <class Archive>
// void load(Archive& archive, FileInfo& info)
// {
// 	archive(cereal::make_nvp("path", info.path));
// 	archive(cereal::make_nvp("size", info.size));
// 	archive(cereal::make_nvp("timeStamp", info.timeStamp));
// 	archive.loadBinaryValue(info.sha2.data(), info.sha2.size(), "sha2");
// }

// template <class Archive>
// void save(Archive& archive, const FileInfo& info)
// {
// 	archive(cereal::make_nvp("path", info.path));
// 	archive(cereal::make_nvp("size", info.size));
// 	archive(cereal::make_nvp("timeStamp", info.timeStamp));
// 	archive.saveBinaryValue(info.sha2.data(), info.sha2.size(), "sha2");
// }

template <typename T, typename Archive>
T loadObject(std::istream& stream, std::string_view name)
{
	Archive archive(stream);
	T outValue{};
	archive(cereal::make_nvp(std::string(name), outValue));
	return outValue;
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
	, myInfo{filePath.string()}
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
	auto fileStream = mio::mmap_ostream(myInfo.path);
	OutputArchive archive(fileStream);
	archive(cereal::make_nvp(std::string(getTypeName<std::decay_t<T>>()), static_cast<const T&>(*this))); 
}

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> getFileInfo(const std::filesystem::path& filePath)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(FileState::Missing);

	auto fileInfo = FileInfo{filePath.string(), std::filesystem::file_size(filePath), getFileTimeStamp(filePath)/*, {}*/};

	if constexpr (Sha256ChecksumEnable)
	{
		ZoneScopedN("getFileInfo::sha2");

		mio::mmap_source file(filePath.string());
		picosha2::hash256(file.cbegin(), file.cend(), fileInfo.sha2.begin(), fileInfo.sha2.end());
	}

	return fileInfo;
}

template <const char* LoaderType, const char* LoaderVersion, bool Sha256ChecksumEnable>
std::expected<ManifestInfo, ManifestState> loadManifest(std::string_view buffer, LoadManifestInfoFn loadManifestInfoFn)
{
	ZoneScoped;

	auto manifestInfo = loadManifestInfoFn(buffer);

	if (std::string_view(LoaderType).compare(manifestInfo.loaderType) != 0 ||
		std::string_view(LoaderVersion).compare(manifestInfo.loaderVersion) != 0)
		return std::unexpected(ManifestState::InvalidVersion);

	auto sourceFileInfo = getFileInfo<Sha256ChecksumEnable>(manifestInfo.sourceFileInfo.path);

	if (!sourceFileInfo ||
		sourceFileInfo->size != manifestInfo.sourceFileInfo.size ||
		sourceFileInfo->timeStamp.compare(manifestInfo.sourceFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? sourceFileInfo->sha2 != manifestInfo.sourceFileInfo.sha2 : false)
		return std::unexpected(ManifestState::InvalidSourceFile);

	auto cacheFileInfo = getFileInfo<Sha256ChecksumEnable>(manifestInfo.cacheFileInfo.path);

	if (!cacheFileInfo ||
		cacheFileInfo->size != manifestInfo.cacheFileInfo.size ||
		cacheFileInfo->timeStamp.compare(manifestInfo.cacheFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? cacheFileInfo->sha2 != manifestInfo.cacheFileInfo.sha2 : false)
		return std::unexpected(ManifestState::InvalidCacheFile);

	return manifestInfo;
}

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> loadBinaryFile(const std::filesystem::path& filePath, LoadFileFn loadOp)
{
	ZoneScoped;

	auto fileInfo = getFileInfo<Sha256ChecksumEnable>(filePath);

	if (fileInfo)
		loadOp(mio::mmap_istream(fileInfo->path));

	return fileInfo;
}

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> saveBinaryFile(const std::filesystem::path& filePath, SaveFileFn saveOp)
{
	ZoneScoped;

	// todo: check if file exist and prompt for overwrite?
	// intended scope - fileStreamBuf needs to be destroyed before we call std::filesystem::file_size
	{
		mio::mmap_iostreambuf fileStreamBuf(filePath.string());
		saveOp(std::iostream(&fileStreamBuf));
	}

	return getFileInfo<Sha256ChecksumEnable>(filePath);
}

template <const char* LoaderType, const char* LoaderVersion>
void loadCachedSourceFile(
	const std::filesystem::path& sourceFilePath,
	LoadFileFn loadSourceFileFn,
	LoadFileFn loadBinaryCacheFn,
	SaveFileFn saveBinaryCacheFn)
{
	ZoneScoped;
	
	std::filesystem::path manifestPath(sourceFilePath);
	manifestPath += ".json";
	
	std::expected<ManifestInfo, ManifestState> manifest;

	auto manifestStatus = std::filesystem::status(manifestPath);

	if (std::filesystem::exists(manifestStatus) &&
		std::filesystem::is_regular_file(manifestStatus))
	{
		ZoneScopedN("loadCachedSourceFile::loadManifest");

		auto loadManifestInfoFn = [](std::string_view buffer)
		{
			if (auto s = glz::read_json<ManifestInfo>(buffer); s)
				return s.value();
			else
				std::cout << "Error: " << s.error() << std::endl;
			
			return ManifestInfo{};
		};

		auto manifestFile = mio::mmap_source(manifestPath.string());

		manifest = loadManifest<LoaderType, LoaderVersion, false>(std::string_view(manifestFile.cbegin(), manifestFile.cend()), loadManifestInfoFn);
	}
	else
	{
		manifest = std::unexpected(ManifestState::Missing);
	}

	if (!manifest)
	{
		ZoneScopedN("loadCachedSourceFile::importSourceFile");

		auto cacheDir = std::filesystem::path(client_getUserProfilePath()) / ".cache";
		auto cacheDirStatus = std::filesystem::status(cacheDir);
		if (!std::filesystem::exists(cacheDirStatus) ||
			!std::filesystem::is_directory(cacheDirStatus))
			std::filesystem::create_directory(cacheDir);

		auto id = uuids::uuid_system_generator{}();
		auto idStr = uuids::to_string(id);

		auto manifestFile = mio::mmap_ostream(manifestPath.string());

		// todo: use mio buffer directly instead of using temp std::string
		std::string buffer;
		glz::write<glz::opts{.prettify = true}>(
			ManifestInfo
			{
				LoaderType,
				LoaderVersion,
				loadBinaryFile<true>(sourceFilePath, loadSourceFileFn).value(),
				saveBinaryFile<true>(cacheDir / idStr, saveBinaryCacheFn).value(),
			},
			buffer);

		manifestFile << buffer;
	}
	else
	{
		ZoneScopedN("loadCachedSourceFile::load");

		auto cacheFileInfo = loadBinaryFile<false>(manifest->cacheFileInfo.path, loadBinaryCacheFn);
		assert(cacheFileInfo);
	}
}
