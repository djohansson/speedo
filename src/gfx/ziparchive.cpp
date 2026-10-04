#include "ziparchive.h"

#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <climits>
#include <optional>
#include <cstring>
#include <format>
#include <fstream>
#include <span>

#include <stb_image.h> // its zlib decoder; the implementation is in imageimport.cpp

//NOLINTBEGIN(readability-magic-numbers)

namespace gfx::zip
{

namespace detail
{

constexpr uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr uint32_t kZip64EndOfCentralDirectoryLocator = 0x07064b50;
constexpr uint32_t kZip64EndOfCentralDirectory = 0x06064b50;
constexpr uint32_t kCentralDirectoryHeader = 0x02014b50;
constexpr uint32_t kLocalFileHeader = 0x04034b50;
constexpr uint16_t kZip64ExtraField = 0x0001;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;
constexpr uint16_t kFlagEncrypted = 1;

// little endian, as zip archives are
template <typename T>
[[nodiscard]] T Read(std::span<const char> bytes, size_t offset)
{
	T value{};
	if (offset + sizeof(T) <= bytes.size())
		std::memcpy(&value, bytes.data() + offset, sizeof(T));
	return value;
}

[[nodiscard]] uint32_t Crc32(std::span<const char> bytes) noexcept
{
	static const auto kTable = []
	{
		std::array<uint32_t, 256> table{};
		for (uint32_t i = 0; i < table.size(); i++)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1U) != 0U ? 0xedb88320U ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		return table;
	}();

	uint32_t crc = 0xffffffffU;
	for (char b : bytes)
		crc = kTable[(crc ^ static_cast<uint8_t>(b)) & 0xffU] ^ (crc >> 8);
	return crc ^ 0xffffffffU;
}

[[nodiscard]] bool ReadAt(std::ifstream& file, uint64_t offset, std::span<char> out)
{
	file.clear();
	file.seekg(static_cast<std::streamoff>(offset));
	file.read(out.data(), static_cast<std::streamsize>(out.size()));
	return file.good() || (file.eof() && file.gcount() == static_cast<std::streamsize>(out.size()));
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

} // namespace detail

std::expected<std::vector<Entry>, std::string> List(const std::filesystem::path& archive)
{
	using namespace detail;

	ZoneScopedN("zip::List");

	std::ifstream file(archive, std::ios::binary);
	if (!file)
		return std::unexpected(std::format("failed to open {}", archive.string()));

	std::error_code error;
	auto fileSize = std::filesystem::file_size(archive, error);
	if (error)
		return std::unexpected(std::format("failed to read {}: {}", archive.string(), error.message()));

	// the end of central directory record is in the last 22 bytes, plus a comment of up to 64 kB
	constexpr uint64_t kEndRecordSize = 22;
	constexpr uint64_t kMaxCommentSize = 65535;
	auto tailSize = std::min<uint64_t>(fileSize, kEndRecordSize + kMaxCommentSize);
	std::vector<char> tail(tailSize);
	if (tailSize < kEndRecordSize || !ReadAt(file, fileSize - tailSize, tail))
		return std::unexpected(std::format("{} is not a zip archive", archive.string()));

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
		std::array<char, kLocatorSize> locator{};
		if (locatorOffset < kLocatorSize || !ReadAt(file, locatorOffset - kLocatorSize, locator) ||
			Read<uint32_t>(locator, 0) != kZip64EndOfCentralDirectoryLocator)
			return std::unexpected(std::format("{}: zip64 end of central directory locator not found", archive.string()));

		std::array<char, 56> record{};
		if (!ReadAt(file, Read<uint64_t>(locator, 8), record) || Read<uint32_t>(record, 0) != kZip64EndOfCentralDirectory)
			return std::unexpected(std::format("{}: zip64 end of central directory not found", archive.string()));

		entryCount = Read<uint64_t>(record, 32);
		directorySize = Read<uint64_t>(record, 40);
		directoryOffset = Read<uint64_t>(record, 48);
	}

	if (directoryOffset + directorySize > fileSize)
		return std::unexpected(std::format("{}: the central directory is out of bounds", archive.string()));

	std::vector<char> directory(directorySize);
	if (!ReadAt(file, directoryOffset, directory))
		return std::unexpected(std::format("failed to read {}", archive.string()));

	std::vector<Entry> entries;
	entries.reserve(entryCount);
	size_t offset = 0;
	for (uint64_t entryIt = 0; entryIt < entryCount; entryIt++)
	{
		constexpr size_t kHeaderSize = 46;
		if (offset + kHeaderSize > directory.size() || Read<uint32_t>(directory, offset) != kCentralDirectoryHeader)
			return std::unexpected(std::format("{}: corrupt central directory", archive.string()));

		Entry entry;
		entry.flags = Read<uint16_t>(directory, offset + 8);
		entry.method = Read<uint16_t>(directory, offset + 10);
		entry.crc = Read<uint32_t>(directory, offset + 16);
		entry.compressedSize = Read<uint32_t>(directory, offset + 20);
		entry.size = Read<uint32_t>(directory, offset + 24);
		auto nameSize = Read<uint16_t>(directory, offset + 28);
		auto extraSize = Read<uint16_t>(directory, offset + 30);
		auto commentSize = Read<uint16_t>(directory, offset + 32);
		entry.localHeaderOffset = Read<uint32_t>(directory, offset + 42);

		if (offset + kHeaderSize + nameSize + extraSize + commentSize > directory.size())
			return std::unexpected(std::format("{}: corrupt central directory", archive.string()));

		entry.name.assign(directory.data() + offset + kHeaderSize, nameSize);
		std::ranges::replace(entry.name, '\\', '/');

		// zip64 sizes and offsets: the extra field holds those whose header value is all ones, in this order
		for (size_t extra = offset + kHeaderSize + nameSize; extra + 4 <= offset + kHeaderSize + nameSize + extraSize;)
		{
			auto id = Read<uint16_t>(directory, extra);
			auto size = Read<uint16_t>(directory, extra + 2);
			if (id == kZip64ExtraField)
			{
				size_t field = extra + 4;
				for (auto* value : {&entry.size, &entry.compressedSize, &entry.localHeaderOffset})
				{
					if (*value == 0xffffffff && field + 8 <= extra + 4 + size)
					{
						*value = Read<uint64_t>(directory, field);
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

std::expected<void, std::string> ExtractAll(
	const std::filesystem::path& archive,
	const std::filesystem::path& directory,
	std::atomic_uint8_t* progress,
	const std::function<bool()>& cancelled)
{
	using namespace detail;

	ZoneScopedN("zip::ExtractAll");

	auto entries = List(archive);
	if (!entries)
		return std::unexpected(entries.error());

	std::ifstream file(archive, std::ios::binary);
	if (!file)
		return std::unexpected(std::format("failed to open {}", archive.string()));

	uint64_t totalSize = 0;
	for (const auto& entry : *entries)
		totalSize += entry.size;

	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error)
		return std::unexpected(std::format("failed to create {}: {}", directory.string(), error.message()));

	uint64_t extractedSize = 0;
	std::vector<char> compressed;
	std::vector<char> data;
	for (const auto& entry : *entries)
	{
		if (cancelled && cancelled())
			return std::unexpected("cancelled");

		if (entry.IsDirectory() || IsResourceFork(entry.name))
			continue;

		auto path = SafePath(entry.name);
		if (!path)
			return std::unexpected(std::format("{}: refusing to extract {} outside the target directory", archive.string(), entry.name));

		// the data follows the local header, whose name and extra field may differ in size from the central directory's
		constexpr size_t kLocalHeaderSize = 30;
		std::array<char, kLocalHeaderSize> header{};
		if (!ReadAt(file, entry.localHeaderOffset, header) || Read<uint32_t>(header, 0) != kLocalFileHeader)
			return std::unexpected(std::format("{}: corrupt local header for {}", archive.string(), entry.name));
		auto dataOffset = entry.localHeaderOffset + kLocalHeaderSize + Read<uint16_t>(header, 26) + Read<uint16_t>(header, 28);

		// the local header describes the data that actually follows it. unless it defers to a data descriptor (or zip64),
		// its values win: some archives have a stale central directory entry (e.g. cube.zip in the McGuire archive,
		// after a file was added to it), which unzip ignores as well
		auto flags = entry.flags;
		auto method = entry.method;
		auto crc = entry.crc;
		uint64_t compressedSize = entry.compressedSize;
		uint64_t size = entry.size;
		constexpr uint16_t kFlagDataDescriptor = 8;
		auto localFlags = Read<uint16_t>(header, 6);
		auto localCompressedSize = Read<uint32_t>(header, 18);
		auto localSize = Read<uint32_t>(header, 22);
		if ((localFlags & kFlagDataDescriptor) == 0 && localCompressedSize != 0xffffffff && localSize != 0xffffffff)
		{
			flags = localFlags;
			method = Read<uint16_t>(header, 8);
			crc = Read<uint32_t>(header, 14);
			compressedSize = localCompressedSize;
			size = localSize;
		}

		if ((flags & kFlagEncrypted) != 0)
			return std::unexpected(std::format("{}: {} is encrypted, which isn't supported", archive.string(), entry.name));
		if (method != kMethodStored && method != kMethodDeflated)
			return std::unexpected(std::format("{}: {} uses compression method {}, only stored (0) and deflated (8) are supported", archive.string(), entry.name, method));
		if (size > INT_MAX || compressedSize > INT_MAX)
			return std::unexpected(std::format("{}: {} is too large ({} bytes)", archive.string(), entry.name, size));

		compressed.resize(compressedSize);
		if (!ReadAt(file, dataOffset, compressed))
			return std::unexpected(std::format("{}: failed to read {}", archive.string(), entry.name));

		if (method == kMethodStored)
		{
			if (compressedSize != size)
				return std::unexpected(std::format("{}: corrupt sizes for {}", archive.string(), entry.name));
			std::swap(data, compressed);
		}
		else
		{
			data.resize(size);
			auto decoded = size == 0 ? 0 : stbi_zlib_decode_noheader_buffer(
				data.data(), static_cast<int>(data.size()), compressed.data(), static_cast<int>(compressed.size()));
			if (decoded != static_cast<int>(size))
				return std::unexpected(std::format("{}: failed to inflate {}", archive.string(), entry.name));
		}

		if (Crc32(data) != crc)
			return std::unexpected(std::format("{}: crc mismatch for {}", archive.string(), entry.name));

		auto target = directory / *path;
		std::filesystem::create_directories(target.parent_path(), error);
		std::ofstream out(target, std::ios::binary | std::ios::trunc);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		if (!out)
			return std::unexpected(std::format("failed to write {}", target.string()));

		extractedSize += entry.size; // as totalSize counts
		if (progress != nullptr && totalSize > 0)
			*progress = static_cast<uint8_t>(255 * extractedSize / totalSize);
	}

	return {};
}

} // namespace gfx::zip

//NOLINTEND(readability-magic-numbers)
