#pragma once

#include "utils.h"

#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <tuple>

#include <cereal/archives/json.hpp>

#include <glaze/glaze.hpp>
#include <glaze/core/macros.hpp>

enum class FileAccessMode : uint8_t
{
	ReadOnly,
	ReadWrite
};

enum class FileState : uint8_t
{
	Missing = 0,
	Valid,
};

struct FileInfo
{
	std::string path;
	uint64_t size = 0;
	std::string timeStamp;
	std::array<uint8_t, 32> sha2;
};

GLZ_META(FileInfo, path, size, timeStamp, sha2);

enum class ManifestState : uint8_t
{
	Missing = 0,
	InvalidVersion,
	InvalidSourceFile,
	InvalidCacheFile,
	Valid,
};
struct ManifestInfo
{
	std::string loaderType;
	std::string loaderVersion;
	FileInfo sourceFileInfo{};
	FileInfo cacheFileInfo{};
};

GLZ_META(ManifestInfo, loaderType, loaderVersion, sourceFileInfo, cacheFileInfo);

std::string getFileTimeStamp(const std::filesystem::path& filePath);

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> getFileInfo(const std::filesystem::path& filePath);

using LoadManifestInfoFn = std::function<ManifestInfo(std::string_view)>;

template <const char* LoaderType, const char* LoaderVersion, bool Sha256ChecksumEnable>
std::expected<ManifestInfo, ManifestState> loadManifest(std::string_view buffer, LoadManifestInfoFn loadManifestInfoFn);

using LoadFileFn = std::function<void(std::istream&&)>;
using SaveFileFn = std::function<void(std::ostream&&)>;

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> loadBinaryFile(const std::filesystem::path& filePath, LoadFileFn loadOp);

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, FileState> saveBinaryFile(const std::filesystem::path& filePath, SaveFileFn saveOp);

template <const char* LoaderType, const char* LoaderVersion>
void loadCachedSourceFile(
	const std::filesystem::path& sourceFilePath,
	LoadFileFn loadSourceFileFn,
	LoadFileFn loadBinaryCacheFn,
	SaveFileFn saveBinaryCacheFn);

template <typename T, typename Archive>
T loadObject(std::istream& stream, std::string_view name);

template <typename T, typename Archive>
std::tuple<std::optional<T>, FileState>
loadObject(const std::filesystem::path& filePath, std::string_view name);

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive, // todo: make this more watertight by using conditional
	bool SaveOnClose = false>
class FileObject
	: public Noncopyable
	, public T
{
	// todo: implement mechanism to only write changes when contents have changed.
	// todo: implement mechanism to update contents if an external process has changed the file.

public:
	constexpr FileObject() noexcept = default;
	FileObject(const std::filesystem::path& filePath, T&& defaultObject = T{});
	FileObject(FileObject&& other) noexcept;
	~FileObject();

	FileObject& operator=(FileObject&& other) noexcept;

	void swap(FileObject& rhs) noexcept;
	friend void swap(FileObject& lhs, FileObject& rhs) noexcept { lhs.swap(rhs); }

	void reload();

	template <FileAccessMode M = Mode>
	typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type save() const;

private:
	FileInfo myInfo;
};

template <typename T>
using ReadOnlyJSONFileObject =
	FileObject<T, FileAccessMode::ReadOnly, cereal::JSONInputArchive, cereal::JSONOutputArchive>;

template <typename T>
using ReadWriteJSONFileObject =
	FileObject<T, FileAccessMode::ReadWrite, cereal::JSONInputArchive, cereal::JSONOutputArchive>;

template <typename T>
using AutoSaveJSONFileObject = FileObject<
	T,
	FileAccessMode::ReadWrite,
	cereal::JSONInputArchive,
	cereal::JSONOutputArchive,
	true>;

#include "file.inl"
