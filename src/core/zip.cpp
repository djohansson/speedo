#include "zip.h"

#include <core/file.h>
#include <core/profiling.h>
#include <core/taskexecutor.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>

#include <libdeflate.h>

//NOLINTBEGIN(readability-magic-numbers)

namespace core::zip
{

namespace detail
{

constexpr uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr uint32_t kZip64EndOfCentralDirectoryLocator = 0x07064b50;
constexpr uint32_t kZip64EndOfCentralDirectory = 0x06064b50;
constexpr uint32_t kCentralDirectoryHeader = 0x02014b50;
constexpr uint32_t kLocalFileHeader = 0x04034b50;
constexpr uint16_t kZip64ExtraField = 0x0001;
constexpr uint16_t kFlagEncrypted = 1;
constexpr uint16_t kFlagDataDescriptor = 8;

// little endian, as zip archives are
template <typename T>
[[nodiscard]] T Read(std::span<const std::byte> bytes, size_t offset)
{
	T value{};
	if (offset + sizeof(T) <= bytes.size())
		std::memcpy(&value, bytes.data() + offset, sizeof(T));
	return value;
}

// the archive's bytes at offset, or nothing if they are past its end
[[nodiscard]] std::optional<std::span<const std::byte>> At(std::span<const std::byte> archive, uint64_t offset, uint64_t size)
{
	if (offset > archive.size() || size > archive.size() - offset)
		return std::nullopt;
	return archive.subspan(offset, size);
}

// the path an entry is extracted to below directory, or nothing if it would land outside it (an absolute path, or
// one with .. in it: "zip slip")
[[nodiscard]] std::optional<std::filesystem::path> SafePath(const std::string& name)
{
	std::filesystem::path path(name);
	if (name.empty() || path.has_root_name() || path.has_root_directory() || name.starts_with('/'))
		return std::nullopt;

	for (const auto& part : path)
		if (part == "..")
			return std::nullopt;

	return path.lexically_normal();
}

[[nodiscard]] bool IsResourceFork(const std::string& name)
{
	return name.starts_with("__MACOSX/") || std::filesystem::path(name).filename().string().starts_with("._");
}

[[nodiscard]] std::span<const std::byte> Bytes(const file::MappedFile& file)
{
	return {file.data(), file.size()};
}

} // namespace detail

std::expected<std::vector<Entry>, std::string> List(const std::filesystem::path& archive)
{
	using namespace detail;

	ZoneScopedN("zip::List");

	auto mapped = file::Map(archive);
	if (!mapped)
		return std::unexpected(std::format("failed to open {}: {}", archive.string(), mapped.error().message()));
	auto file = Bytes(*mapped);
	uint64_t fileSize = file.size();

	// the end of central directory record is in the last 22 bytes, plus a comment of up to 64 kB
	constexpr uint64_t kEndRecordSize = 22;
	constexpr uint64_t kMaxCommentSize = 65535;
	auto tailSize = std::min<uint64_t>(fileSize, kEndRecordSize + kMaxCommentSize);
	if (tailSize < kEndRecordSize)
		return std::unexpected(std::format("{} is not a zip archive", archive.string()));
	auto tail = file.subspan(fileSize - tailSize);

	std::optional<size_t> endRecord;
	for (auto offset = tail.size() - kEndRecordSize + 1; offset-- > 0;)
	{
		if (Read<uint32_t>(tail, offset) == kEndOfCentralDirectory)
		{
			endRecord = offset;
			break;
		}
	}
	if (!endRecord)
		return std::unexpected(std::format("{} is not a zip archive", archive.string()));

	uint64_t entryCount = Read<uint16_t>(tail, *endRecord + 10);
	uint64_t directorySize = Read<uint32_t>(tail, *endRecord + 12);
	uint64_t directoryOffset = Read<uint32_t>(tail, *endRecord + 16);

	if (Read<uint16_t>(tail, *endRecord + 4) != 0 || Read<uint16_t>(tail, *endRecord + 6) != 0)
		return std::unexpected(std::format("{} is split over several files, which isn't supported", archive.string()));

	// zip64: the real values are in a zip64 end of central directory record, which a locator before this one points to
	if (entryCount == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff)
	{
		constexpr size_t kLocatorSize = 20;
		auto locatorOffset = (fileSize - tailSize) + *endRecord;
		auto locator = locatorOffset < kLocatorSize ? std::nullopt : At(file, locatorOffset - kLocatorSize, kLocatorSize);
		if (!locator || Read<uint32_t>(*locator, 0) != kZip64EndOfCentralDirectoryLocator)
			return std::unexpected(std::format("{}: zip64 end of central directory locator not found", archive.string()));

		constexpr size_t kRecordSize = 56;
		auto record = At(file, Read<uint64_t>(*locator, 8), kRecordSize);
		if (!record || Read<uint32_t>(*record, 0) != kZip64EndOfCentralDirectory)
			return std::unexpected(std::format("{}: zip64 end of central directory not found", archive.string()));

		entryCount = Read<uint64_t>(*record, 32);
		directorySize = Read<uint64_t>(*record, 40);
		directoryOffset = Read<uint64_t>(*record, 48);
	}

	auto directory = At(file, directoryOffset, directorySize);
	if (!directory)
		return std::unexpected(std::format("{}: the central directory is out of bounds", archive.string()));

	std::vector<Entry> entries;
	entries.reserve(entryCount);
	size_t offset = 0;
	for (uint64_t entryIt = 0; entryIt < entryCount; entryIt++)
	{
		constexpr size_t kHeaderSize = 46;
		if (offset + kHeaderSize > directory->size() || Read<uint32_t>(*directory, offset) != kCentralDirectoryHeader)
			return std::unexpected(std::format("{}: corrupt central directory", archive.string()));

		Entry entry;
		entry.flags = Read<uint16_t>(*directory, offset + 8);
		entry.method = Read<uint16_t>(*directory, offset + 10);
		entry.crc = Read<uint32_t>(*directory, offset + 16);
		entry.compressedSize = Read<uint32_t>(*directory, offset + 20);
		entry.size = Read<uint32_t>(*directory, offset + 24);
		auto nameSize = Read<uint16_t>(*directory, offset + 28);
		auto extraSize = Read<uint16_t>(*directory, offset + 30);
		auto commentSize = Read<uint16_t>(*directory, offset + 32);
		entry.localHeaderOffset = Read<uint32_t>(*directory, offset + 42);

		if (offset + kHeaderSize + nameSize + extraSize + commentSize > directory->size())
			return std::unexpected(std::format("{}: corrupt central directory", archive.string()));

		entry.name.assign(reinterpret_cast<const char*>(directory->data() + offset + kHeaderSize), nameSize);
		std::ranges::replace(entry.name, '\\', '/');

		// zip64 sizes and offsets: the extra field holds those whose header value is all ones, in this order
		for (size_t extra = offset + kHeaderSize + nameSize; extra + 4 <= offset + kHeaderSize + nameSize + extraSize;)
		{
			auto id = Read<uint16_t>(*directory, extra);
			auto size = Read<uint16_t>(*directory, extra + 2);
			if (id == kZip64ExtraField)
			{
				size_t field = extra + 4;
				for (auto* value : {&entry.size, &entry.compressedSize, &entry.localHeaderOffset})
				{
					if (*value == 0xffffffff && field + 8 <= extra + 4 + size)
					{
						*value = Read<uint64_t>(*directory, field);
						field += 8;
					}
				}
			}
			extra += 4 + size;
		}

		entries.push_back(std::move(entry));
		offset += kHeaderSize + nameSize + extraSize + commentSize;
	}

	return entries;
}

std::expected<EntryData, std::string> Locate(std::span<const std::byte> archive, const Entry& entry)
{
	using namespace detail;

	// the data follows the local header, whose name and extra field may differ in size from the central directory's
	constexpr size_t kLocalHeaderSize = 30;
	auto header = At(archive, entry.localHeaderOffset, kLocalHeaderSize);
	if (!header || Read<uint32_t>(*header, 0) != kLocalFileHeader)
		return std::unexpected(std::format("corrupt local header for {}", entry.name));
	auto dataOffset = entry.localHeaderOffset + kLocalHeaderSize + Read<uint16_t>(*header, 26) + Read<uint16_t>(*header, 28);

	// the local header's values win, unless it defers to a data descriptor (or zip64)
	auto flags = entry.flags;
	EntryData data{.size = entry.size, .crc = entry.crc, .method = entry.method};
	uint64_t compressedSize = entry.compressedSize;
	auto localFlags = Read<uint16_t>(*header, 6);
	auto localCompressedSize = Read<uint32_t>(*header, 18);
	auto localSize = Read<uint32_t>(*header, 22);
	if ((localFlags & kFlagDataDescriptor) == 0 && localCompressedSize != 0xffffffff && localSize != 0xffffffff)
	{
		flags = localFlags;
		data.method = Read<uint16_t>(*header, 8);
		data.crc = Read<uint32_t>(*header, 14);
		compressedSize = localCompressedSize;
		data.size = localSize;
	}

	if ((flags & kFlagEncrypted) != 0)
		return std::unexpected(std::format("{} is encrypted, which isn't supported", entry.name));
	if (data.method != kMethodStored && data.method != kMethodDeflated)
		return std::unexpected(
			std::format("{} uses compression method {}, only stored (0) and deflated (8) are supported", entry.name, data.method));
	if (data.method == kMethodStored && compressedSize != data.size)
		return std::unexpected(std::format("corrupt sizes for {}", entry.name));

	auto compressed = At(archive, dataOffset, compressedSize);
	if (!compressed)
		return std::unexpected(std::format("{} is out of bounds", entry.name));
	data.compressed = *compressed;

	return data;
}

bool Inflate(std::span<const std::byte> compressed, std::span<std::byte> out)
{
	// a decompressor per thread: allocating one takes about 32 kB
	static thread_local std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> tDecompressor(
		libdeflate_alloc_decompressor(), &libdeflate_free_decompressor);
	if (!tDecompressor)
		return false;
	return libdeflate_deflate_decompress(tDecompressor.get(), compressed.data(), compressed.size(), out.data(), out.size(), nullptr) ==
		   LIBDEFLATE_SUCCESS;
}

uint32_t Crc32(std::span<const std::byte> bytes) noexcept
{
	return libdeflate_crc32(0, bytes.data(), bytes.size());
}

std::expected<void, std::string> ExtractAll(
	const std::filesystem::path& archive,
	const std::filesystem::path& directory,
	TaskExecutor* executor,
	std::atomic_uint8_t* progress,
	const std::function<bool()>& cancelled)
{
	using namespace detail;

	ZoneScopedN("zip::ExtractAll");

	auto entries = List(archive);
	if (!entries)
		return std::unexpected(entries.error());

	auto mapped = file::Map(archive);
	if (!mapped)
		return std::unexpected(std::format("failed to open {}: {}", archive.string(), mapped.error().message()));
	auto file = Bytes(*mapped);

	// what to extract, where, and the directories first (here: concurrent create_directories of the same path can fail)
	struct Target
	{
		const Entry* entry = nullptr;
		std::filesystem::path path;
	};
	std::vector<Target> targets;
	uint64_t totalSize = 0;
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error)
		return std::unexpected(std::format("failed to create {}: {}", directory.string(), error.message()));
	for (const auto& entry : *entries)
	{
		if (entry.IsDirectory() || IsResourceFork(entry.name))
			continue;

		auto path = SafePath(entry.name);
		if (!path)
			return std::unexpected(std::format("{}: refusing to extract {} outside the target directory", archive.string(), entry.name));

		auto target = directory / *path;
		std::filesystem::create_directories(target.parent_path(), error);
		if (error)
			return std::unexpected(std::format("failed to create {}: {}", target.parent_path().string(), error.message()));
		targets.push_back({.entry = &entry, .path = std::move(target)});
		totalSize += entry.size;
	}

	// the entries are taken one at a time by workers: one per executor thread, or this thread alone without an executor
	struct State
	{
		std::span<const std::byte> archive;
		const std::vector<Target>& targets;
		std::atomic_size_t next = 0;
		std::atomic_uint64_t extractedSize = 0;
		uint64_t totalSize = 0;
		std::atomic_uint8_t* progress = nullptr;
		const std::function<bool()>& cancelled;
		std::atomic_bool failed = false;
		std::mutex errorMutex;
		std::string error;

		void Fail(std::string message)
		{
			std::lock_guard lock(errorMutex);
			if (!failed.exchange(true))
				error = std::move(message);
		}

		// each entry inflated straight into its file, mapped: no buffer of the entry's size per worker
		void Work()
		{
			for (size_t targetIt = next++; targetIt < targets.size() && !failed; targetIt = next++)
			{
				if (cancelled && cancelled())
				{
					Fail("cancelled");
					return;
				}

				const auto& [entry, path] = targets[targetIt];
				auto located = Locate(archive, *entry);
				if (!located)
				{
					Fail(located.error());
					return;
				}

				if (located->size == 0)
				{
					if (auto written = file::Write(path, {}); !written)
					{
						Fail(std::format("failed to write {}: {}", path.string(), written.error().message()));
						return;
					}
				}
				else
				{
					mio_extra::resizeable_mmap_sink<std::byte> out;
					std::error_code mapError;
					out.map(path.string(), mapError); // creates it
					if (mapError)
					{
						Fail(std::format("failed to write {}: {}", path.string(), mapError.message()));
						return;
					}
					if (auto resized = out.resize(located->size); !resized)
					{
						Fail(std::format("failed to write {}: {}", path.string(), resized.error().message()));
						return;
					}
					std::span<std::byte> data(out.data(), located->size);
					if (located->method == kMethodDeflated)
					{
						if (!Inflate(located->compressed, data))
						{
							Fail(std::format("failed to inflate {}", entry->name));
							return;
						}
					}
					else
					{
						std::ranges::copy(located->compressed, data.begin());
					}
					if (Crc32(data) != located->crc)
					{
						Fail(std::format("crc mismatch for {}", entry->name));
						return;
					}
				}

				auto extracted = extractedSize += entry->size; // as totalSize counts
				if (progress != nullptr && totalSize > 0)
					progress->store(static_cast<uint8_t>(255 * extracted / totalSize), std::memory_order_relaxed);
			}
		}
	} state{.archive = file, .targets = targets, .totalSize = totalSize, .progress = progress, .cancelled = cancelled};

	if (executor == nullptr)
	{
		state.Work();
	}
	else
	{
		auto workerCount = std::min<size_t>(std::max(1U, std::thread::hardware_concurrency()), targets.size());
		std::vector<Future<void>> futures;
		std::vector<TaskHandle> handles;
		for (size_t workerIt = 0; workerIt < workerCount; workerIt++)
		{
			auto [handle, future] = CreateTask([&state] { state.Work(); });
			handles.push_back(handle);
			futures.push_back(std::move(future));
		}
		executor->Submit(handles);
		// helping out with the executor's ready queue meanwhile (this may be a task itself)
		for (auto& future : futures)
			(void)executor->Join(std::move(future));
	}

	if (state.failed)
		return std::unexpected(std::format("{}: {}", archive.string(), state.error));
	return {};
}

} // namespace core::zip

//NOLINTEND(readability-magic-numbers)
