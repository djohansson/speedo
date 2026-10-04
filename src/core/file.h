#pragma once

#include <core/mio_extra.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <zpp_bits.h>

namespace core
{

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

// a step's share of a load's progress (0-255): advanced from its value when the step starts, to `end` when it is done.
// a step that takes long checks `cancelled` (if set) as it goes, and fails with std::errc::operation_canceled.
struct Progress
{
	std::atomic_uint8_t* value = nullptr;
	uint8_t end = 0;
	std::function<bool()> cancelled;
};

template <typename T, AccessMode Mode, bool SaveOnDestruct = false>
class Object : public T
{
	// todo: implement mechanism to only write changes when contents have changed.
	// todo: implement mechanism to update contents if an external process has changed the file.
	// todo: construct or cast directly on mapped memory.
	// todo: parameter to chose format (binary/json/whatever)
	// todo: retire this class and remake it as taking a std::span instead of a filename
	//       this way we can create an API that tracks changes to data, and only wries to the backing store when changes have been made.

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

using InputSerializer = zpp::bits::in<mio::basic_mmap_source<std::byte>>;
using OutputSerializer = zpp::bits::out<mio_extra::resizeable_mmap_sink<std::byte>, zpp::bits::no_fit_size, zpp::bits::no_enlarge_overflow>;

using LoadFn = std::function<std::error_code(InputSerializer&)>;
using SaveFn = std::function<std::error_code(OutputSerializer&)>;
// files the source file pulled in while loading (e.g. shader includes/imports). called after a successful source load;
// their records are kept in the manifest, so that changing any of them invalidates the cache as well.
using DependenciesFn = std::function<std::vector<std::filesystem::path>()>;

[[nodiscard]] std::expected<std::string, std::error_code> GetTimeStamp(const std::filesystem::path& filePath) noexcept;

[[nodiscard]] std::expected<std::filesystem::path, std::error_code> GetCanonicalPath(
	const char* pathStr,
	const char* defaultPathStr,
	bool createIfMissing = false) noexcept;

template <bool Sha256ChecksumEnable>
[[nodiscard]] std::expected<Record, std::error_code> GetRecord(const std::filesystem::path& filePath, Progress progress = {});

template <bool Sha256ChecksumEnable>
[[nodiscard]] std::expected<Record, std::error_code> LoadBinary(const std::filesystem::path& filePath, const LoadFn& loadOp, Progress hashProgress = {});

template <bool Sha256ChecksumEnable>
[[nodiscard]] std::expected<Record, std::error_code> SaveBinary(const std::filesystem::path& filePath, const SaveFn& saveOp, Progress hashProgress = {});

template <typename T>
[[nodiscard]] std::expected<T, std::error_code> LoadObject(std::span<std::byte> buffer) noexcept;

template <typename T>
[[nodiscard]] std::expected<T, std::error_code> LoadObject(const std::filesystem::path& filePath);

template <typename T>
[[nodiscard]] std::expected<void, std::error_code> SaveObject(const T& object, const std::string& filePath);

[[nodiscard]] std::expected<Record, std::error_code> LoadAsset(
	const std::filesystem::path& filePath,
	const LoadFn& loadSourceFileFn,
	const LoadFn& loadBinaryCacheFn,
	const SaveFn& SaveBinaryCacheFn,
	const std::string& parameterHash,
	const DependenciesFn& dependenciesFn = {},
	std::atomic_uint8_t* progressOut = nullptr, // see LoadAsset in file.cpp for the share its steps take
	const std::function<bool()>& cancelled = {}); // checked while hashing (the load ops check it themselves)

} // namespace file

} // namespace core

#include "file.inl"
