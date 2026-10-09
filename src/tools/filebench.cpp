// measures ways of reading a file whole: the std streams the importers used (an istreambuf_iterator, and one read into a
// buffer of the file's size), stdio, and core's memory mapped files (mio), read in place or copied out. each method hashes
// the bytes it read (XXH3), so every page is touched, and the hashes must agree.
//
// usage: filebench [--iterations <n>] [--work <dir>] [--sizes <MiB>,...] [--import] [files...]
//
// with --import, it times the gfx importers on the files given instead (models, panoramas, images; warm).
//
// without files, it writes files of --sizes (default 1, 16 and 256 MiB) of random bytes into --work (default the system's
// temporary directory). each method runs warm (the file in the page cache, read once before timing) and cold (evicted
// before each run: posix_fadvise on linux; on macos, which can't evict a file's pages without root, each run reads a fresh
// copy written past the cache with F_NOCACHE). cold runs aren't supported on windows.
//
// it uses the std streams and stdio on purpose: they are what is measured (see CLAUDE.md, "File I/O").

#include <core/mio_extra.h>
#include <gfx/environment.h>
#include <gfx/imageimport.h>
#include <gfx/meshimport.h>

#include <xxhash.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define FILEBENCH_COLD 1
#else
#define FILEBENCH_COLD 0
#endif

namespace filebench
{

struct Method
{
	std::string_view name;
	std::function<uint64_t(const std::filesystem::path&)> read; // the hash of the file's bytes
};

[[nodiscard]] uint64_t Hash(const void* data, size_t size)
{
	return XXH3_64bits(data, size);
}

// as imageimport.cpp's ReadFile and assettest did: a character at a time into a growing vector
[[nodiscard]] uint64_t StreamIterator(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return Hash(chars.data(), chars.size());
}

// one read into a buffer of the file's size
[[nodiscard]] uint64_t StreamRead(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	std::vector<std::byte> bytes(std::filesystem::file_size(path));
	file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return Hash(bytes.data(), bytes.size());
}

// stdio, as stb_image, cgltf and tinyobj's mtl reader do
[[nodiscard]] uint64_t Stdio(const std::filesystem::path& path)
{
	std::vector<std::byte> bytes(std::filesystem::file_size(path));
	std::FILE* file = std::fopen(path.string().c_str(), "rb");
	if (file == nullptr)
		return 0;
	auto read = std::fread(bytes.data(), 1, bytes.size(), file);
	std::fclose(file);
	return Hash(bytes.data(), read);
}

// core's memory mapped file, read in place
[[nodiscard]] uint64_t Mapped(const std::filesystem::path& path)
{
	std::error_code error;
	auto file = mio::make_mmap<mio::basic_mmap_source<std::byte>>(path.string(), 0, mio::map_entire_file, error);
	if (error)
		return 0;
	return Hash(file.data(), file.size());
}

#if FILEBENCH_COLD
// core's memory mapped file, read in place after advising the kernel of the access (read-ahead)
template <int Advice>
[[nodiscard]] uint64_t MappedAdvised(const std::filesystem::path& path)
{
	std::error_code error;
	auto file = mio::make_mmap<mio::basic_mmap_source<std::byte>>(path.string(), 0, mio::map_entire_file, error);
	if (error)
		return 0;
	// mapped from offset 0, so data() is the page aligned start of the mapping
	::madvise(const_cast<std::byte*>(file.data()), file.size(), Advice);
	return Hash(file.data(), file.size());
}
#endif

#if defined(__APPLE__)
// core's memory mapped file, read in place after asking for the whole file to be read ahead (F_RDADVISE)
[[nodiscard]] uint64_t MappedReadAdvised(const std::filesystem::path& path)
{
	std::error_code error;
	auto file = mio::make_mmap<mio::basic_mmap_source<std::byte>>(path.string(), 0, mio::map_entire_file, error);
	if (error)
		return 0;
	radvisory advisory{.ra_offset = 0, .ra_count = static_cast<int>(std::min<size_t>(file.size(), INT32_MAX))};
	::fcntl(file.file_handle(), F_RDADVISE, &advisory);
	return Hash(file.data(), file.size());
}
#endif

// core's memory mapped file, copied into a buffer (what a parser that wants to own its bytes would do)
[[nodiscard]] uint64_t MappedCopy(const std::filesystem::path& path)
{
	std::error_code error;
	auto file = mio::make_mmap<mio::basic_mmap_source<std::byte>>(path.string(), 0, mio::map_entire_file, error);
	if (error)
		return 0;
	std::vector<std::byte> bytes(file.size());
	std::memcpy(bytes.data(), file.data(), file.size());
	return Hash(bytes.data(), bytes.size());
}

[[nodiscard]] bool WriteRandom(const std::filesystem::path& path, size_t size)
{
	std::mt19937_64 random(size);
	std::vector<uint64_t> words((size + 7) / 8);
	std::ranges::generate(words, std::ref(random));
	std::FILE* file = std::fopen(path.string().c_str(), "wb");
	if (file == nullptr)
		return false;
	auto written = std::fwrite(words.data(), 1, size, file);
	std::fclose(file);
	return written == size;
}

#if FILEBENCH_COLD
// a copy of source at target whose pages aren't in the page cache, or nothing if that can't be done
[[nodiscard]] bool MakeCold(const std::filesystem::path& source, const std::filesystem::path& target)
{
#if defined(__linux__)
	(void)target;
	int fd = ::open(source.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	bool evicted = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
	::close(fd);
	return evicted;
#else
	// macos: written with F_NOCACHE, the copy's pages don't stay in the unified buffer cache
	std::error_code error;
	std::filesystem::remove(target, error);
	int in = ::open(source.c_str(), O_RDONLY);
	int out = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (in < 0 || out < 0 || ::fcntl(out, F_NOCACHE, 1) != 0)
	{
		if (in >= 0)
			::close(in);
		if (out >= 0)
			::close(out);
		return false;
	}
	std::vector<std::byte> buffer(size_t{16} << 20);
	bool ok = true;
	for (ssize_t read = 0; ok && (read = ::read(in, buffer.data(), buffer.size())) > 0;)
		ok = ::write(out, buffer.data(), static_cast<size_t>(read)) == read;
	ok = ok && ::fsync(out) == 0;
	::close(in);
	::close(out);
	return ok;
#endif
}
#endif

struct Result
{
	double medianSeconds = 0.0;
	uint64_t hash = 0;
};

[[nodiscard]] std::optional<Result> Run(
	const Method& method, const std::filesystem::path& path, const std::filesystem::path& coldPath, bool cold, unsigned iterations)
{
	std::vector<double> seconds;
	uint64_t hash = 0;
	if (!cold)
		hash = method.read(path); // into the page cache

	for (unsigned iteration = 0; iteration < iterations; iteration++)
	{
		const std::filesystem::path* target = &path;
#if FILEBENCH_COLD
		if (cold)
		{
			if (!MakeCold(path, coldPath))
				return std::nullopt;
#if !defined(__linux__)
			target = &coldPath;
#endif
		}
#else
		(void)coldPath;
		if (cold)
			return std::nullopt;
#endif
		auto start = std::chrono::steady_clock::now();
		auto runHash = method.read(*target);
		seconds.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		if (hash != 0 && runHash != hash)
		{
			std::println(stderr, "{}: {} read different bytes", path.string(), method.name);
			return std::nullopt;
		}
		hash = runHash;
	}

	std::ranges::sort(seconds);
	return Result{.medianSeconds = seconds[seconds.size() / 2], .hash = hash};
}

// --import: times the gfx importers on files (models, panoramas, images), warm (each imported once before timing)
[[nodiscard]] int ImportFiles(const std::vector<std::filesystem::path>& files, unsigned iterations)
{
	std::println("{} iterations, medians, warm", iterations);
	for (const auto& file : files)
	{
		auto extension = file.extension().string();
		std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::function<std::optional<std::string>()> import;
		if (gfx::mesh::IsModelFile(file))
			import = [&file] { auto mesh = gfx::mesh::Import(file); return mesh ? std::nullopt : std::optional(mesh.error()); };
		else if (extension == ".hdr")
			import = [&file] { auto panorama = gfx::environment::Import(file); return panorama ? std::nullopt : std::optional(panorama.error()); };
		else
			import = [&file] { auto pixels = gfx::image::Decode(file, {}); return pixels ? std::nullopt : std::optional(pixels.error()); };

		if (auto failed = import())
		{
			std::println(stderr, "{}", *failed);
			return 1;
		}
		std::vector<double> seconds;
		for (unsigned iteration = 0; iteration < iterations; iteration++)
		{
			auto start = std::chrono::steady_clock::now();
			(void)import();
			seconds.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		}
		std::ranges::sort(seconds);
		std::println("  {:>10.2f} ms  {}", seconds[seconds.size() / 2] * 1e3, file.string());
	}
	return 0;
}

} // namespace filebench

int main(int argc, char* argv[])
{
	using namespace filebench;

	unsigned iterations = 7;
	std::filesystem::path work = std::filesystem::temp_directory_path() / "filebench";
	std::vector<size_t> sizesMiB{1, 16, 256};
	std::vector<std::filesystem::path> files;
	bool importFiles = false;
	for (int argIt = 1; argIt < argc; argIt++)
	{
		std::string_view arg(argv[argIt]);
		if (arg == "--iterations" && argIt + 1 < argc)
			iterations = std::max(1U, static_cast<unsigned>(std::strtoul(argv[++argIt], nullptr, 10)));
		else if (arg == "--work" && argIt + 1 < argc)
			work = argv[++argIt];
		else if (arg == "--sizes" && argIt + 1 < argc)
		{
			sizesMiB.clear();
			for (std::string_view list(argv[++argIt]); !list.empty();)
			{
				auto end = list.find(',');
				sizesMiB.push_back(std::strtoull(std::string(list.substr(0, end)).c_str(), nullptr, 10));
				list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
			}
		}
		else if (arg == "--import")
			importFiles = true;
		else if (arg == "--help" || arg == "-h")
		{
			std::println("usage: filebench [--iterations <n>] [--work <dir>] [--sizes <MiB>,...] [--import] [files...]");
			return 0;
		}
		else
			files.emplace_back(arg);
	}

	if (importFiles)
		return ImportFiles(files, iterations);

	std::error_code error;
	std::filesystem::create_directories(work, error);
	if (files.empty())
		for (auto sizeMiB : sizesMiB)
		{
			auto path = work / std::format("random-{}MiB.bin", sizeMiB);
			if (!std::filesystem::exists(path) || std::filesystem::file_size(path) != (sizeMiB << 20))
				if (!WriteRandom(path, sizeMiB << 20))
				{
					std::println(stderr, "failed to write {}", path.string());
					return 1;
				}
			files.push_back(path);
		}

	const std::vector<Method> methods{
		{"ifstream istreambuf_iterator", StreamIterator},
		{"ifstream read", StreamRead},
		{"stdio fread", Stdio},
		{"mmap", Mapped},
#if FILEBENCH_COLD
		{"mmap + MADV_SEQUENTIAL", MappedAdvised<MADV_SEQUENTIAL>},
		{"mmap + MADV_WILLNEED", MappedAdvised<MADV_WILLNEED>},
#endif
#if defined(__APPLE__)
		{"mmap + F_RDADVISE", MappedReadAdvised},
#endif
		{"mmap + copy", MappedCopy},
	};

	std::println("{} iterations, medians", iterations);
	for (const auto& file : files)
	{
		auto size = std::filesystem::file_size(file, error);
		if (error)
		{
			std::println(stderr, "{}: {}", file.string(), error.message());
			return 1;
		}
		std::println("\n{} ({:.1f} MiB)", file.string(), static_cast<double>(size) / (1 << 20));
		std::println("  {:<30} {:>10} {:>10}   {:>10} {:>10}", "", "warm ms", "GB/s", "cold ms", "GB/s");
		auto coldPath = work / "cold.bin";
		std::optional<uint64_t> expected;
		for (const auto& method : methods)
		{
			auto warm = Run(method, file, coldPath, false, iterations);
			auto cold = Run(method, file, coldPath, true, iterations);
			for (const auto& result : {warm, cold})
				if (result)
				{
					if (expected && result->hash != *expected)
					{
						std::println(stderr, "{}: {} read different bytes than the others", file.string(), method.name);
						return 1;
					}
					expected = result->hash;
				}
			auto column = [size](const std::optional<Result>& result)
			{
				return result ? std::format("{:>10.2f} {:>10.2f}", result->medianSeconds * 1e3, static_cast<double>(size) / result->medianSeconds / 1e9)
							  : std::format("{:>10} {:>10}", "-", "-");
			};
			std::println("  {:<30} {}   {}", method.name, column(warm), column(cold));
		}
		std::filesystem::remove(coldPath, error);
	}
	return 0;
}
