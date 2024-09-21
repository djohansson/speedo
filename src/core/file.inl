#include "application.h"
#include "profiling.h"

#include <array>
#include <iostream>

#include <glaze/glaze.hpp>
#include <utility>

#include <picosha2.h>

#include <stduuid/uuid.h>

namespace file
{

namespace detail
{

enum class AssetManifestErrorCode : uint8_t
{
	kMissing,
	kInvalidLocation,
	kInvalidSourceFile,
	kInvalidCacheFile,
};

const char* ToString(AssetManifestErrorCode code) noexcept;

using AssetManifestError = std::variant<AssetManifestErrorCode, std::error_code>;

struct AssetManifest
{
	file::Record assetFileInfo;
	file::Record cacheFileInfo;
};

using LoadAssetManifestInfoFn = std::function<std::expected<AssetManifest, std::error_code>(std::string_view)>;

template <bool Sha256ChecksumEnable>
std::expected<AssetManifest, AssetManifestError>
LoadJSONAssetManifest(std::string_view buffer, const LoadAssetManifestInfoFn& loadManifestInfoFn)
{
	ZoneScoped;

	auto manifestInfo = loadManifestInfoFn(buffer);

	if (!manifestInfo)
		return std::unexpected(manifestInfo.error());

	auto assetFileInfo = GetRecord<Sha256ChecksumEnable>(manifestInfo->assetFileInfo.path);

	if (!assetFileInfo ||
		(assetFileInfo->size != manifestInfo->assetFileInfo.size) ||
		(assetFileInfo->timeStamp.compare(manifestInfo->assetFileInfo.timeStamp)) != 0 ||
		(Sha256ChecksumEnable ? (assetFileInfo->sha2 != manifestInfo->assetFileInfo.sha2) : false))
		return std::unexpected(AssetManifestErrorCode::kInvalidSourceFile);

	auto cacheFileInfo = GetRecord<Sha256ChecksumEnable>(manifestInfo->cacheFileInfo.path);

	if (!cacheFileInfo ||
		(cacheFileInfo->size != manifestInfo->cacheFileInfo.size) ||
		(cacheFileInfo->timeStamp.compare(manifestInfo->cacheFileInfo.timeStamp)) != 0 ||
		(Sha256ChecksumEnable ? (cacheFileInfo->sha2 != manifestInfo->cacheFileInfo.sha2) : false))
		return std::unexpected(AssetManifestErrorCode::kInvalidCacheFile);

	return manifestInfo.value();
}

} // namespace detail

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> GetRecord(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::error_code error;
	auto fileStatus = std::filesystem::status(filePath, error);

	if (error)
		return std::unexpected(error);

	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

	auto timestamp = GetTimeStamp(filePath);
	if (!timestamp)
		return std::unexpected(timestamp.error());

	auto fileInfo = Record{
		filePath.string(),
		timestamp.value(),
		{},
		std::filesystem::file_size(filePath)};

	if constexpr (Sha256ChecksumEnable)
	{
		ZoneScopedN("GetRecord::sha2");

		static constexpr size_t kSha2Size = 32;
		std::array<uint8_t, kSha2Size> sha2;
		mio::mmap_source file;
		std::error_code error;
		file.map(filePath.string(), error);
		if (error)
			return std::unexpected(error);

		picosha2::hash256(file.cbegin(), file.cend(), sha2.begin(), sha2.end());
		picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), fileInfo.sha2);
	}

	return fileInfo;
}

template <typename T>
std::expected<T, std::error_code> LoadJSONObject(std::string_view buffer) noexcept
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
std::expected<T, std::error_code> LoadJSONObject(const std::filesystem::path& filePath)
{
	std::error_code error;
	auto fileStatus = std::filesystem::status(filePath, error);

	if (error)
		return std::unexpected(error);

	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

	auto file = mio::mmap_source();
	file.map(filePath.string(), error);

	if (error)
		return std::unexpected(error);

	return LoadJSONObject<T>(std::string_view(file.cbegin(), file.cend()));
}

template <typename T>
std::expected<void, std::error_code> SaveJSONObject(const T& object, const std::string& filePath)
{
	auto file = mio_extra::ResizeableMemoryMapSink();
	std::error_code error;

	file.map(filePath, error);

	if (error)
		return std::unexpected(error);

	if (auto result = glz::write<glz::opts{.prettify = true}>(object, file); !result)
		return std::unexpected(std::make_error_code(std::errc::io_error));
	
	file.truncate(file.Size(), error);

	if (error)
		return std::unexpected(error);

	return {};
}

template <typename T>
std::expected<T, std::error_code> LoadBinaryObject(const std::filesystem::path& filePath)
{
	std::error_code error;
	auto fileStatus = std::filesystem::status(filePath, error);

	if (error)
		return std::unexpected(error);

	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::unexpected(error);

	auto file = mio::mmap_source();
	file.map(filePath.string(), error);

	if (error)
		return std::unexpected(error);

	auto inStream = zpp::bits::in(file);

	T object;
	auto result = inStream(object);

	if (result.failure())
		return std::unexpected(result.error().code);

	if (inStream.position() != file.size())
		return std::unexpected(std::make_error_code(std::errc::invalid_argument));

	return object;
}

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> LoadBinary(const std::filesystem::path& filePath, const LoadFn& loadOp)
{
	ZoneScoped;

	auto fileInfo = GetRecord<Sha256ChecksumEnable>(filePath);

	if (fileInfo)
	{
		auto file = mio::mmap_source();
		std::error_code error;
		file.map(fileInfo->path, error);
		if (error)
			return std::unexpected(error);

		auto inStream = zpp::bits::in(file);

		//ASSERT(in.position() == file.size());

		if (auto error = loadOp(inStream))
			return std::unexpected(error);
	}

	return fileInfo;
}

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> SaveBinary(const std::filesystem::path& filePath, const SaveFn& saveOp)
{
	ZoneScoped;

	// todo: check if file exist and prompt for overwrite?
	// intended scope - file needs to be closed before we call GetRecord (due to internal call to std::filesystem::file_size)
	{
		std::error_code error;
		auto file = mio_extra::ResizeableMemoryMapSink();
		file.map(filePath.string(), error);

		if (error)
			return std::unexpected(error);

		auto out = zpp::bits::out(file, zpp::bits::no_fit_size{}, zpp::bits::no_enlarge_overflow{});

		if (error = saveOp(out); error)
			return std::unexpected(error);

		file.truncate(out.position(), error);

		if (error)
			return std::unexpected(error);
	}

	return GetRecord<Sha256ChecksumEnable>(filePath);
}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
Object<T, Mode, SaveOnDestruct>::Object(
	const std::filesystem::path& filePath, T&& defaultObject)
	: T(LoadJSONObject<T>(filePath).value_or(std::forward<T>(defaultObject)))
	, myInfo{filePath.string()}
{}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
Object<T, Mode, SaveOnDestruct>::Object(
	Object&& other) noexcept
	: T(std::forward<Object>(other))
	, myInfo(std::exchange(other.myInfo, {}))
{}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
Object<T, Mode, SaveOnDestruct>::~Object()
{
	if constexpr (SaveOnDestruct)
		if (!myInfo.path.empty())
			Save();
}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
Object<T, Mode, SaveOnDestruct>&
Object<T, Mode, SaveOnDestruct>::operator=(Object&& other) noexcept
{
	myInfo = std::exchange(other.myInfo, {});
	return *this;
}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
void Object<T, Mode, SaveOnDestruct>::Swap(Object& rhs) noexcept
{
	std::swap<T>(*this, rhs);
	std::swap(myInfo, rhs.myInfo);
}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
void Object<T, Mode, SaveOnDestruct>::Reload()
{
	static_cast<T&>(*this) = LoadJSONObject<T>(myInfo.path).value_or(std::move(static_cast<T&>(*this)));
}

template <typename T, AccessMode Mode, bool SaveOnDestruct>
std::enable_if_t<Object<T, Mode, SaveOnDestruct>::kMode == AccessMode::kReadWrite, void> Object<T, Mode, SaveOnDestruct>::Save() const
{
	auto result = SaveJSONObject(static_cast<const T&>(*this), myInfo.path);
	ASSERT(result);
}

} // namespace file
