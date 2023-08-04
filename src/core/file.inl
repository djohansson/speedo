#include "profiling.h"
#include "typeinfo.h"

#include <client/client.h> // TODO: eliminate this dependency

#include <mio/mmap_iostream.hpp>

#include <picosha2.h>

namespace mio_extra
{
	struct stl_mmap_sink : public mio::mmap_sink
	{
		stl_mmap_sink(const std::string& path, const size_type offset = 0, const size_type length = mio::map_entire_file)
			: mio::mmap_sink(path, offset, length)
		{}

		void resize(size_t size)
		{
			std::error_code error;
			mio::mmap_sink::remap(0, size, error);
			if (error)
				throw std::system_error(std::move(error));
		}
	};
}

template <typename T>
std::expected<T, bool> loadObject(std::string_view buffer)
{
	if (auto s = glz::read_json<T>(buffer))
		return s.value();
	else
		std::cout << "Manifest parse error: " << glz::format_error(s.error(), buffer) << std::endl;
	
	return std::unexpected(false);
};

template <typename T>
std::expected<T, FileState> loadObject(const std::filesystem::path& filePath)
{
	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(FileState::Missing);

	auto file = mio::mmap_source(filePath.string());
	
	if (auto object = loadObject<T>(std::string_view(file.cbegin(), file.cend())))
		return object.value();

	return std::unexpected(FileState::Corrupted);
}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
FileObject<T, Mode, SaveOnClose>::FileObject(
	const std::filesystem::path& filePath, T&& defaultObject)
	: T(loadObject<T>(filePath).value_or(std::forward<T>(defaultObject)))
	, myInfo{filePath.string()}
{}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
FileObject<T, Mode, SaveOnClose>::FileObject(
	FileObject&& other) noexcept
	: T(std::forward<FileObject>(other))
	, myInfo(std::exchange(other.myInfo, {}))
{}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
FileObject<T, Mode, SaveOnClose>::~FileObject()
{
	if constexpr (SaveOnClose)
		if (!myInfo.path.empty())
			save();
}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
FileObject<T, Mode, SaveOnClose>&
FileObject<T, Mode, SaveOnClose>::operator=(FileObject&& other) noexcept
{
	myInfo = std::exchange(other.myInfo, {});
	return *this;
}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
void FileObject<T, Mode, SaveOnClose>::swap(FileObject& rhs) noexcept
{
	std::swap<T>(*this, rhs);
	std::swap(myInfo, rhs.myInfo);
}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
void FileObject<T, Mode, SaveOnClose>::reload()
{
	static_cast<T&>(*this) = loadObject<T>(myInfo.path).value_or(std::move(static_cast<T&>(*this)));
}

template <typename T, FileAccessMode Mode, bool SaveOnClose>
template <FileAccessMode M>
typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type
FileObject<T, Mode, SaveOnClose>::save() const
{
	auto file = mio_extra::stl_mmap_sink(myInfo.path);

	std::error_code error;
	file.truncate(glz::write<glz::opts{.prettify = true}>(*this, file), error);

	if (error)
		throw std::system_error(std::move(error));
}

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> getFileInfo(const std::filesystem::path& filePath)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(FileState::Missing);

	auto fileInfo = FileInfo{
		filePath.string(),
		getFileTimeStamp(filePath),
		{},
		std::filesystem::file_size(filePath)};

	if constexpr (Sha256ChecksumEnable)
	{
		ZoneScopedN("getFileInfo::sha2");

		std::array<uint8_t, 32> sha2;
		mio::mmap_source file(filePath.string());
		picosha2::hash256(file.cbegin(), file.cend(), sha2.begin(), sha2.end());
		picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), fileInfo.sha2);
	}

	return fileInfo;
}

template <const char* LoaderType, const char* LoaderVersion, bool Sha256ChecksumEnable>
std::expected<ManifestInfo, ManifestState> loadManifest(std::string_view buffer, LoadManifestInfoFn loadManifestInfoFn)
{
	ZoneScoped;

	auto manifestInfo = loadManifestInfoFn(buffer);

	if (!manifestInfo)
		return std::unexpected(ManifestState::Corrupted);

	if (std::string_view(LoaderType).compare(manifestInfo->loaderType) != 0 ||
		std::string_view(LoaderVersion).compare(manifestInfo->loaderVersion) != 0)
		return std::unexpected(ManifestState::InvalidVersion);

	auto sourceFileInfo = getFileInfo<Sha256ChecksumEnable>(manifestInfo->sourceFileInfo.path);

	if (!sourceFileInfo ||
		sourceFileInfo->size != manifestInfo->sourceFileInfo.size ||
		sourceFileInfo->timeStamp.compare(manifestInfo->sourceFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? sourceFileInfo->sha2 != manifestInfo->sourceFileInfo.sha2 : false)
		return std::unexpected(ManifestState::InvalidSourceFile);

	auto cacheFileInfo = getFileInfo<Sha256ChecksumEnable>(manifestInfo->cacheFileInfo.path);

	if (!cacheFileInfo ||
		cacheFileInfo->size != manifestInfo->cacheFileInfo.size ||
		cacheFileInfo->timeStamp.compare(manifestInfo->cacheFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? cacheFileInfo->sha2 != manifestInfo->cacheFileInfo.sha2 : false)
		return std::unexpected(ManifestState::InvalidCacheFile);

	return manifestInfo.value();
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
	// intended scope - file needs to be closed before we call getFileInfo (due to internal call to std::filesystem::file_size)
	{
		saveOp(mio::mmap_ostream(filePath.string()));
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

		auto manifestFile = mio::mmap_source(manifestPath.string());

		manifest = loadManifest<LoaderType, LoaderVersion, false>(
			std::string_view(manifestFile.cbegin(), manifestFile.cend()),
			[](std::string_view buffer){ return loadObject<ManifestInfo>(buffer); });
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

		auto manifestFile = mio_extra::stl_mmap_sink(manifestPath.string());

		std::error_code error;
		manifestFile.truncate(
			glz::write<glz::opts{.prettify = true}>(
				ManifestInfo
				{
					LoaderType,
					LoaderVersion,
					loadBinaryFile<true>(sourceFilePath, loadSourceFileFn).value(),
					saveBinaryFile<true>(cacheDir / idStr, saveBinaryCacheFn).value(),
				},
				manifestFile),
			error);

		if (error)
			throw std::system_error(std::move(error));
	}
	else
	{
		ZoneScopedN("loadCachedSourceFile::load");

		auto cacheFileInfo = loadBinaryFile<false>(manifest->cacheFileInfo.path, loadBinaryCacheFn);
		assert(cacheFileInfo);
	}
}
