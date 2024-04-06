#pragma once

#include "utils.h"
#include "mio_extra.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <variant>

#include <zpp_bits.h>

namespace file
{

enum class AccessMode : uint8_t
{
	ReadOnly,
	ReadWrite
};

struct Record
{
	std::string path;
	std::string timeStamp;
	std::string sha2;
	uint64_t size = 0;
};

template <typename T, AccessMode Mode, bool SaveOnClose = false>
class Object : public Noncopyable, public T
{
	// todo: implement mechanism to only write changes when contents have changed.
	// todo: implement mechanism to update contents if an external process has changed the file.
	// todo: construct or cast directly on mapped memory.
	// todo: parameter to chose format (binary/json/whatever)

public:
	constexpr Object() noexcept = default;
	Object(const std::filesystem::path& filePath, T&& defaultObject = T{});
	Object(Object&& other) noexcept;
	~Object();

	Object& operator=(Object&& other) noexcept;

	void swap(Object& rhs) noexcept;
	friend void swap(Object& lhs, Object& rhs) noexcept { lhs.swap(rhs); }

	void reload();

	template <AccessMode M = Mode>
	typename std::enable_if<M == AccessMode::ReadWrite, void>::type save() const;

private:
	Record myInfo;
};

using InputSerializer = zpp::bits::in<mio::mmap_source>;
using OutputSerializer = zpp::bits::out<mio_extra::resizeable_mmap_sink, zpp::bits::no_fit_size, zpp::bits::no_enlarge_overflow>;

using LoadFn = std::function<std::error_code(InputSerializer&)>;
using SaveFn = std::function<std::error_code(OutputSerializer&)>;

std::string getTimeStamp(const std::filesystem::path& filePath);

std::filesystem::path getCanonicalPath(
	const char* pathStr,
	const char* defaultPathStr,
	bool createIfMissing = false);

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> getRecord(const std::filesystem::path& filePath);

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> loadBinary(const std::filesystem::path& filePath, LoadFn loadOp);

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> saveBinary(const std::filesystem::path& filePath, SaveFn saveOp);

template <typename T>
std::expected<T, std::error_code> loadBinaryObject(const std::filesystem::path& filePath);

template <typename T>
std::expected<T, std::error_code> loadJSONObject(std::string_view buffer);

template <typename T>
std::expected<T, std::error_code> loadJSONObject(const std::filesystem::path& filePath);

template <typename T>
std::expected<void, std::error_code> saveJSONObject(const T& object, const std::string& filePath);

template <const char* LoaderType, const char* LoaderVersion>
void loadAsset(const std::filesystem::path& filePath, LoadFn loadFileFn, LoadFn loadBinaryCacheFn, SaveFn saveBinaryCacheFn);

} // namespace file

#include "file.inl"
