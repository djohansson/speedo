#pragma once

#include "utils.h"
#include "mio_extra.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <variant>

#include <zpp_bits.h>

namespace file
{

enum class AccessMode : uint8_t
{
	kReadOnly,
	kReadWrite
};

struct Record
{
	std::string path;
	std::string timeStamp;
	std::string sha2;
	uint64_t size = 0;
};

template <typename T, AccessMode Mode, bool SaveOnDestruct = false>
class Object : public T
{
	// todo: implement mechanism to only write changes when contents have changed.
	// todo: implement mechanism to update contents if an external process has changed the file.
	// todo: construct or cast directly on mapped memory.
	// todo: parameter to chose format (binary/json/whatever)

public:

	static constexpr auto kMode = Mode;

	constexpr Object() noexcept = default;
	Object(const Object&) = delete;
	explicit Object(const std::filesystem::path& filePath, T&& defaultObject = T{});
	Object(Object&& other) noexcept;
	~Object();

	Object& operator=(const Object&) = delete;
	[[maybe_unused]] Object& operator=(Object&& other) noexcept;

	void Swap(Object& rhs) noexcept;
	friend void Swap(Object& lhs, Object& rhs) noexcept { lhs.Swap(rhs); }

	void Reload();

	std::enable_if_t<kMode == AccessMode::kReadWrite, void> Save() const;

private:
	Record myInfo;
};

using InputSerializer = zpp::bits::in<mio::mmap_source>;
using OutputSerializer = zpp::bits::out<mio_extra::ResizeableMemoryMapSink, zpp::bits::no_fit_size, zpp::bits::no_enlarge_overflow>;

using LoadFn = std::function<std::error_code(InputSerializer&)>;
using SaveFn = std::function<std::error_code(OutputSerializer&)>;

std::expected<std::string, std::error_code> GetTimeStamp(const std::filesystem::path& filePath) noexcept;

std::expected<std::filesystem::path, std::error_code> GetCanonicalPath(
	const char* pathStr,
	const char* defaultPathStr,
	bool createIfMissing = false) noexcept;

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> GetRecord(const std::filesystem::path& filePath);

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> LoadBinary(const std::filesystem::path& filePath, const LoadFn& loadOp);

template <bool Sha256ChecksumEnable>
std::expected<Record, std::error_code> SaveBinary(const std::filesystem::path& filePath, const SaveFn& saveOp);

template <typename T>
std::expected<T, std::error_code> LoadBinaryObject(const std::filesystem::path& filePath);

template <typename T>
std::expected<T, std::error_code> LoadJSONObject(std::string_view buffer) noexcept;

template <typename T>
std::expected<T, std::error_code> LoadJSONObject(const std::filesystem::path& filePath);

template <typename T>
[[maybe_unused]] std::expected<void, std::error_code> SaveJSONObject(const T& object, const std::string& filePath);

[[maybe_unused]] std::expected<Record, std::error_code> LoadAsset(
	const std::filesystem::path& filePath,
	const LoadFn& loadSourceFileFn,
	const LoadFn& loadBinaryCacheFn,
	const SaveFn& SaveBinaryCacheFn,
	const std::string& parameterHash);

} // namespace file

#include "file.inl"
