#pragma once

#include "utils.h"

#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <tuple>

#include <glaze/glaze.hpp>
#include <glaze/core/macros.hpp>

#include <mio/mmap.hpp>

#include <zpp_bits.h>

enum class FileAccessMode : uint8_t
{
	ReadOnly,
	ReadWrite
};

enum class FileState : uint8_t
{
	Missing,
	Corrupted,
	Valid,
};

struct FileInfo
{
	std::string path;
	std::string timeStamp;
	std::string sha2;
	uint64_t size = 0;
};

GLZ_META(FileInfo, path, size, timeStamp, sha2);

enum class ManifestState : uint8_t
{
	Missing,
	Corrupted,
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

using LoadManifestInfoFn = std::function<std::expected<ManifestInfo, bool>(std::string_view)>;

template <const char* LoaderType, const char* LoaderVersion, bool Sha256ChecksumEnable>
std::expected<ManifestInfo, ManifestState> loadManifest(std::string_view buffer, LoadManifestInfoFn loadManifestInfoFn);

using LoadFileFn = std::function<void(zpp::bits::in<mio::mmap_source>&)>;
using SaveFileFn = std::function<void(zpp::bits::out<mio_extra::resizeable_mmap_sink, zpp::bits::no_fit_size>&)>;

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

template <typename T>
std::expected<T, bool> loadObject(std::string_view buffer);

template <typename T>
std::expected<T, FileState> loadObject(const std::filesystem::path& filePath);

template <typename T, FileAccessMode Mode, bool SaveOnClose = false>
class FileObject : public Noncopyable, public T
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
using ReadOnlyFileObject = FileObject<T, FileAccessMode::ReadOnly>;

template <typename T>
using ReadWriteFileObject = FileObject<T, FileAccessMode::ReadWrite>;

template <typename T>
using AutoSaveFileObject = FileObject<T, FileAccessMode::ReadWrite, true>;

#include "file.inl"
