#pragma once

#include "utils.h"
#include "concurrency-utils.h"
#include "mio_extra.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <variant>

#include <zpp_bits.h>

enum class FileAccessMode : uint8_t
{
	ReadOnly,
	ReadWrite
};

struct FileInfo
{
	std::string path;
	std::string timeStamp;
	std::string sha2;
	uint64_t size = 0;
};

using InputSerializer = zpp::bits::in<mio::mmap_source>;
using OutputSerializer = zpp::bits::out<mio_extra::resizeable_mmap_sink, zpp::bits::no_fit_size, zpp::bits::no_enlarge_overflow>;

using LoadFileFn = std::function<std::error_code(InputSerializer&)>;
using SaveFileFn = std::function<std::error_code(OutputSerializer&)>;

std::string getFileTimeStamp(const std::filesystem::path& filePath);

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, std::error_code> getFileInfo(const std::filesystem::path& filePath);

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, std::error_code> loadBinaryFile(const std::filesystem::path& filePath, LoadFileFn loadOp);

template <bool Sha256ChecksumEnable>
std::expected<FileInfo, std::error_code> saveBinaryFile(const std::filesystem::path& filePath, SaveFileFn saveOp);

template <typename T>
std::expected<T, std::error_code> loadBinaryObject(const std::filesystem::path& filePath);

template <typename T>
std::expected<T, std::error_code> loadJSONObject(std::string_view buffer);

template <typename T>
std::expected<T, std::error_code> loadJSONObject(const std::filesystem::path& filePath);

template <typename T>
std::expected<void, std::error_code> saveJSONObject(const T& object, const std::string& filePath);

template <typename T, FileAccessMode Mode, bool SaveOnClose = false>
class JSONFileObject : public Noncopyable, public T
{
	// todo: implement mechanism to only write changes when contents have changed.
	// todo: implement mechanism to update contents if an external process has changed the file.
	// todo: construct or cast directly on mapped memory.

public:
	constexpr JSONFileObject() noexcept = default;
	JSONFileObject(const std::filesystem::path& filePath, T&& defaultObject = T{});
	JSONFileObject(JSONFileObject&& other) noexcept;
	~JSONFileObject();

	JSONFileObject& operator=(JSONFileObject&& other) noexcept;

	void swap(JSONFileObject& rhs) noexcept;
	friend void swap(JSONFileObject& lhs, JSONFileObject& rhs) noexcept { lhs.swap(rhs); }

	void reload();

	template <FileAccessMode M = Mode>
	typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type save() const;

private:
	FileInfo myInfo;
};

template <typename T>
using ReadOnlyJSONFileObject = JSONFileObject<T, FileAccessMode::ReadOnly>;

template <typename T>
using ReadWriteJSONFileObject = JSONFileObject<T, FileAccessMode::ReadWrite>;

template <typename T>
using AutoSaveJSONFileObject = JSONFileObject<T, FileAccessMode::ReadWrite, true>;

template <const char* LoaderType, const char* LoaderVersion>
void loadAsset(const std::filesystem::path& filePath, LoadFileFn loadFileFn, LoadFileFn loadBinaryCacheFn, SaveFileFn saveBinaryCacheFn);

#include "file.inl"
