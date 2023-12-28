#include "application.h"
#include "profiling.h"

#include <array>
#include <iostream>

#include <glaze/glaze.hpp>

#include <stduuid/uuid.h>

#include <picosha2.h>

namespace file
{

namespace detail
{

enum class AssetManifestErrorCode : uint8_t
{
	Missing,
	InvalidVersion,
	InvalidLocation,
	InvalidSourceFile,
	InvalidCacheFile,
};

using AssetManifestError = std::variant<AssetManifestErrorCode, std::error_code>;

struct AssetManifest
{
	std::string loaderType;
	std::string loaderVersion;
	file::Record assetFileInfo;
	file::Record cacheFileInfo;
};

using LoadAssetManifestInfoFn = std::function<std::expected<AssetManifest, std::error_code>(std::string_view)>;

template <const char* LoaderType, const char* LoaderVersion, bool Sha256ChecksumEnable>
std::expected<AssetManifest, AssetManifestError>
loadJSONAssetManifest(std::string_view buffer, LoadAssetManifestInfoFn loadManifestInfoFn)
{
	ZoneScoped;

	auto manifestInfo = loadManifestInfoFn(buffer);

	if (!manifestInfo)
		return std::unexpected(manifestInfo.error());

	if (std::string_view(LoaderType).compare(manifestInfo->loaderType) != 0 ||
		std::string_view(LoaderVersion).compare(manifestInfo->loaderVersion) != 0)
		return std::unexpected(AssetManifestErrorCode::InvalidVersion);

	auto assetFileInfo = getRecord<Sha256ChecksumEnable>(manifestInfo->assetFileInfo.path);

	if (!assetFileInfo ||
		assetFileInfo->size != manifestInfo->assetFileInfo.size ||
		assetFileInfo->timeStamp.compare(manifestInfo->assetFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? assetFileInfo->sha2 != manifestInfo->assetFileInfo.sha2 : false)
		return std::unexpected(AssetManifestErrorCode::InvalidSourceFile);

	auto cacheFileInfo = getRecord<Sha256ChecksumEnable>(manifestInfo->cacheFileInfo.path);

	if (!cacheFileInfo ||
		cacheFileInfo->size != manifestInfo->cacheFileInfo.size ||
		cacheFileInfo->timeStamp.compare(manifestInfo->cacheFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? cacheFileInfo->sha2 != manifestInfo->cacheFileInfo.sha2 : false)
		return std::unexpected(AssetManifestErrorCode::InvalidCacheFile);

	return manifestInfo.value();
}

} // namespace detail

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> getRecord(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::error_code error;
	auto fileStatus = std::filesystem::status(filePath, error);

	if (error)
		return std::unexpected(error);

	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

	auto fileInfo = Record{
		filePath.string(),
		getTimeStamp(filePath),
		{},
		std::filesystem::file_size(filePath)};

	if constexpr (Sha256ChecksumEnable)
	{
		ZoneScopedN("getRecord::sha2");

		std::array<uint8_t, 32> sha2;
		mio::mmap_source file(filePath.string());
		picosha2::hash256(file.cbegin(), file.cend(), sha2.begin(), sha2.end());
		picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), fileInfo.sha2);
	}

	return fileInfo;
}

template <typename T>
std::expected<T, std::error_code> loadJSONObject(std::string_view buffer)
{
	auto obj = glz::read_json<T>(buffer);

	if (!obj)
	{
		std::cerr << "JSON parse error: " << glz::format_error(obj.error(), buffer) << std::endl;

		return std::unexpected(std::make_error_code(std::errc::invalid_argument));
	}

	return obj.value();
};

template <typename T>
std::expected<T, std::error_code> loadJSONObject(const std::filesystem::path& filePath)
{
	std::error_code error;
	auto fileStatus = std::filesystem::status(filePath, error);

	if (error)
		return std::unexpected(error);

	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

	auto file = mio::mmap_source(filePath.string());

	return loadJSONObject<T>(std::string_view(file.cbegin(), file.cend()));
}

template <typename T>
std::expected<void, std::error_code> saveJSONObject(const T& object, const std::string& filePath)
{
	auto file = mio_extra::resizeable_mmap_sink(filePath);

	glz::write<glz::opts{.prettify = true}>(object, file);

	std::error_code error;
	file.truncate(file.hightWaterMark(), error);

	if (error)
		return std::unexpected(error);

	return {};
}

template <typename T>
std::expected<T, std::error_code> loadBinaryObject(const std::filesystem::path& filePath)
{
	std::error_code error;
	auto fileStatus = std::filesystem::status(filePath, error);

	if (error)
		return std::unexpected(error);

	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

	auto file = mio::mmap_source(filePath.string());
	auto in = zpp::bits::in(file);

	T object;
	auto result = in(object);

	if (result.failure())
		return std::unexpected(result.error().code);

	if (in.position() != file.size())
		return std::unexpected(std::make_error_code(std::errc::invalid_argument));

	return object;
}

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> loadBinary(const std::filesystem::path& filePath, LoadFn loadOp)
{
	ZoneScoped;

	auto fileInfo = getRecord<Sha256ChecksumEnable>(filePath);

	if (fileInfo)
	{
		auto file = mio::mmap_source(fileInfo->path);
		auto in = zpp::bits::in(file);

		//assert(in.position() == file.size());

		if (auto error = loadOp(in))
			return std::unexpected(error);
	}

	return fileInfo;
}

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> saveBinary(const std::filesystem::path& filePath, SaveFn saveOp)
{
	ZoneScoped;

	// todo: check if file exist and prompt for overwrite?
	// intended scope - file needs to be closed before we call getRecord (due to internal call to std::filesystem::file_size)
	{
		auto file = mio_extra::resizeable_mmap_sink(filePath.string());
		auto out = zpp::bits::out(file, zpp::bits::no_fit_size{}, zpp::bits::no_enlarge_overflow{});

		if (auto error = saveOp(out))
			return std::unexpected(error);
		
		std::error_code error;
		file.truncate(out.position(), error);

		if (error)
			return std::unexpected(error);
	}

	return getRecord<Sha256ChecksumEnable>(filePath);
}

template <typename T, AccessMode Mode, bool SaveOnClose>
Object<T, Mode, SaveOnClose>::Object(
	const std::filesystem::path& filePath, T&& defaultObject)
	: T(loadJSONObject<T>(filePath).value_or(std::forward<T>(defaultObject)))
	, myInfo{filePath.string()}
{}

template <typename T, AccessMode Mode, bool SaveOnClose>
Object<T, Mode, SaveOnClose>::Object(
	Object&& other) noexcept
	: T(std::forward<Object>(other))
	, myInfo(std::exchange(other.myInfo, {}))
{}

template <typename T, AccessMode Mode, bool SaveOnClose>
Object<T, Mode, SaveOnClose>::~Object()
{
	if constexpr (SaveOnClose)
		if (!myInfo.path.empty())
			save();
}

template <typename T, AccessMode Mode, bool SaveOnClose>
Object<T, Mode, SaveOnClose>&
Object<T, Mode, SaveOnClose>::operator=(Object&& other) noexcept
{
	myInfo = std::exchange(other.myInfo, {});
	return *this;
}

template <typename T, AccessMode Mode, bool SaveOnClose>
void Object<T, Mode, SaveOnClose>::swap(Object& rhs) noexcept
{
	std::swap<T>(*this, rhs);
	std::swap(myInfo, rhs.myInfo);
}

template <typename T, AccessMode Mode, bool SaveOnClose>
void Object<T, Mode, SaveOnClose>::reload()
{
	static_cast<T&>(*this) = loadJSONObject<T>(myInfo.path).value_or(std::move(static_cast<T&>(*this)));
}

template <typename T, AccessMode Mode, bool SaveOnClose>
template <AccessMode M>
typename std::enable_if<M == AccessMode::ReadWrite, void>::type
Object<T, Mode, SaveOnClose>::save() const
{
	if (!saveJSONObject(static_cast<const T&>(*this), myInfo.path))
		throw std::runtime_error("Failed to save file: " + myInfo.path);
}

template <const char* LoaderType, const char* LoaderVersion>
void loadAsset(
	const std::filesystem::path& assetFilePath,
	LoadFn loadSourceFileFn,
	LoadFn loadBinaryCacheFn,
	SaveFn saveBinaryCacheFn)
{
	using namespace detail;
	
	ZoneScoped;
	
	std::filesystem::path manifestPath(assetFilePath);
	manifestPath += ".json";
	
	std::expected<AssetManifest, AssetManifestError> manifest;

	auto manifestStatus = std::filesystem::status(manifestPath);

	if (std::filesystem::exists(manifestStatus) && std::filesystem::is_regular_file(manifestStatus))
	{
		ZoneScopedN("loadAsset::loadJSONAssetManifest");

		auto manifestFile = mio::mmap_source(manifestPath.string());

		manifest = loadJSONAssetManifest<LoaderType, LoaderVersion, false>(
			std::string_view(manifestFile.cbegin(), manifestFile.cend()),
			[](std::string_view buffer){ return loadJSONObject<AssetManifest>(buffer); });
	}
	else
	{
		manifest = std::unexpected(AssetManifestErrorCode::Missing);
	}
	
	if (std::holds_alternative<AssetManifestErrorCode>(manifest.error()))
	{
loadAsset_importSourceFile:
		ZoneScopedN("loadAsset::importSourceFile");

		auto cacheDir = std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["UserProfilePath"]);
		auto cacheDirStatus = std::filesystem::status(cacheDir);
		if (!std::filesystem::exists(cacheDirStatus) ||
			!std::filesystem::is_directory(cacheDirStatus))
			std::filesystem::create_directory(cacheDir);

		auto id = uuids::uuid_system_generator{}();
		auto idStr = uuids::to_string(id);

		auto manifestFile = mio_extra::resizeable_mmap_sink(manifestPath.string());

		auto asset = loadBinary<true>(assetFilePath, loadSourceFileFn);
		if (!asset)
			throw std::system_error(asset.error());

		auto cache = saveBinary<true>(cacheDir / idStr, saveBinaryCacheFn);
		if (!cache)
			throw std::system_error(cache.error());

		glz::write<glz::opts{.prettify = true}>(
			AssetManifest{LoaderType, LoaderVersion, asset.value(), cache.value()}, manifestFile);

		std::error_code error;
		manifestFile.truncate(manifestFile.hightWaterMark(), error);

		if (error)
			throw std::system_error(std::move(error));
	}
	else if (std::holds_alternative<std::error_code>(manifest.error()))
	{
		// manifest is invalid, delete it and re-import source file
		std::cerr << "Asset manifest is invalid: " << manifestPath << '\n';
		std::cerr << "Deleting manifest and reimporting source file\n";
		std::filesystem::remove(manifestPath);
		goto loadAsset_importSourceFile;
	}
	else
	{
		ZoneScopedN("loadAsset::load");

		auto cacheFileInfo = loadBinary<false>(manifest->cacheFileInfo.path, loadBinaryCacheFn);
		
		if (!cacheFileInfo)
			throw std::system_error(cacheFileInfo.error());
	}
}

} // namespace file
