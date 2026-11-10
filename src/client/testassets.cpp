#include "testassets.h"

#include <core/application.h>
#include <core/file.h>
#include <core/mio_extra.h>
#include <core/profiling.h>
#include <core/task.h>
#include <core/taskexecutor.h>
#include <core/zip.h>

#include <platform/capi.h>

#include <curl/curl.h>
#include <imgui.h>
#include <picosha2.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace client::testassets
{

namespace
{

// a manifest (resources/test-asset-manifests, see scripts/fetch-test-assets.ps1): the set's directory under the test asset
// directory, the url its files are under, the directories of zips extracted (each into a directory of its name, side by
// side: Bistro's parts), and its files, each its size, sha256, path under the directory and path under the url
struct Entry
{
	uint64_t size = 0;
	std::string sha256;
	std::string path;
	std::string source;
};
struct Manifest
{
	std::string dir;
	std::string url;
	std::vector<std::pair<std::string, std::string>> extracts; // <zips' directory> <their directory>
	std::vector<Entry> entries;
};

constexpr std::array kManifestNames{"mcguire.txt", "gltf.txt", "environments.txt"};
// transfers at once (curl's handles, each its own connection: the port has no HTTP/2), and tries of a file
constexpr size_t kTransferCount = 8;
constexpr uint32_t kAttemptCount = 3;
constexpr const char* kExtractedMarker = ".extracted";

[[nodiscard]] std::filesystem::path Variable(const char* name)
{
	return std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables[name]);
}

// a path of the manifests' (UTF-8): std::filesystem::path(std::string) reads the ANSI code page on Windows
[[nodiscard]] std::filesystem::path Utf8Path(std::string_view path)
{
	return {std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size())};
}

// calls fn with each line of a text file that isn't empty or a comment (#), without its \r. false if it can't be read
template <typename Fn>
bool ForEachLine(const std::filesystem::path& file, Fn&& fn)
{
	auto mapped = core::file::Map(file);
	if (!mapped)
		return false;
	std::string_view text(reinterpret_cast<const char*>(mapped->data()), mapped->size());
	for (auto line : text | std::views::split('\n'))
	{
		std::string_view row(line.begin(), line.end());
		if (row.ends_with('\r'))
			row.remove_suffix(1);
		if (!row.empty() && !row.starts_with('#'))
			fn(row);
	}
	return true;
}

[[nodiscard]] std::optional<Manifest> ReadManifest(const std::filesystem::path& file)
{
	Manifest manifest;
	bool valid = true;
	bool read = ForEachLine(
		file,
		[&manifest, &valid](std::string_view row)
		{
			std::vector<std::string_view> fields;
			for (auto field : row | std::views::split('\t'))
				fields.emplace_back(field.begin(), field.end());
			if (fields[0] == "dir" && fields.size() >= 2)
				manifest.dir = fields[1];
			else if (fields[0] == "url" && fields.size() >= 2)
				manifest.url = fields[1];
			else if (fields[0] == "extract" && fields.size() >= 3)
				manifest.extracts.emplace_back(fields[1], fields[2]);
			else if (fields.size() >= 3)
			{
				Entry entry;
				auto [end, error] = std::from_chars(fields[0].data(), fields[0].data() + fields[0].size(), entry.size);
				valid = valid && error == std::errc{};
				entry.sha256 = fields[1];
				entry.path = fields[2];
				entry.source = fields.size() > 3 ? fields[3] : fields[2];
				manifest.entries.push_back(std::move(entry));
			}
			else
				valid = false;
		});
	if (!read || !valid || manifest.dir.empty() || manifest.url.empty())
	{
		std::println(stderr, "test assets: {} is missing or malformed", file.string());
		return std::nullopt;
	}
	return manifest;
}

// a set's files: the manifests, and of each the entries the set has (the quick set's, those under quick.txt's paths)
struct Selection
{
	std::vector<Manifest> manifests;
	std::vector<std::vector<size_t>> entries; // by manifest
};

[[nodiscard]] Selection Select(Set set)
{
	auto manifests = Variable("ResourcePath") / "test-asset-manifests";
	std::vector<std::string> quick;
	if (set == Set::kQuick)
		ForEachLine(
			manifests / "quick.txt",
			[&quick](std::string_view row)
			{
				while (!row.empty() && (row.back() == ' ' || row.back() == '\t'))
					row.remove_suffix(1);
				quick.emplace_back(row);
			});
	auto inQuick = [&quick](std::string_view path)
	{
		return std::ranges::any_of(
			quick, [path](const std::string& entry) { return path == entry || (path.starts_with(entry) && path.size() > entry.size() && path[entry.size()] == '/'); });
	};

	Selection selection;
	for (const char* name : kManifestNames)
	{
		auto manifest = ReadManifest(manifests / name);
		if (!manifest)
			continue;
		auto& entries = selection.entries.emplace_back();
		for (size_t entryIt = 0; entryIt < manifest->entries.size(); entryIt++)
			if (set == Set::kFull || inQuick(std::format("{}/{}", manifest->dir, manifest->entries[entryIt].path)))
				entries.push_back(entryIt);
		selection.manifests.push_back(std::move(*manifest));
	}
	return selection;
}

[[nodiscard]] std::filesystem::path FileOf(const Manifest& manifest, const Entry& entry)
{
	return Variable("TestAssetsPath") / Utf8Path(manifest.dir) / Utf8Path(entry.path);
}

// there at its size: a download writes the file only once its sha256 matched (a .part until then)
[[nodiscard]] bool Present(const std::filesystem::path& file, uint64_t size)
{
	std::error_code error;
	auto fileSize = std::filesystem::file_size(file, error);
	return !error && fileSize == size;
}

// a file to download, and the zips to extract into a directory (each into one of its name) once they are all there
struct Job
{
	std::string url;
	std::filesystem::path file;
	uint64_t size = 0;
	std::string sha256;
	std::string name; // for the ui and errors: <set's directory>/<path>
};
struct Extract
{
	std::vector<std::pair<std::filesystem::path, uint64_t>> zips;
	std::filesystem::path directory;
	std::string name;
};

// curl's global state, made before its first use (not thread safe), and cleaned up at exit
void InitCurl()
{
	static const struct Global
	{
		Global() { curl_global_init(CURL_GLOBAL_DEFAULT); }
		~Global() { curl_global_cleanup(); }
		Global(const Global&) = delete;
		Global& operator=(const Global&) = delete;
	} kGlobal;
}

// the url of a path under url, each segment escaped (some glTF paths have spaces, one UTF-8)
[[nodiscard]] std::string UrlOf(std::string_view url, std::string_view path)
{
	std::string result(url);
	for (auto segment : path | std::views::split('/'))
	{
		std::string_view part(segment.begin(), segment.end());
		char* escaped = curl_easy_escape(nullptr, part.data(), static_cast<int>(part.size()));
		result += '/';
		result += escaped;
		curl_free(escaped);
	}
	return result;
}

#if defined(__linux__)
// the system's certificate bundle, by the distributions' paths: the OpenSSL curl is built with looks in its own prefix,
// which an AppImage's distribution doesn't have (macOS verifies with the system's trust store, Windows' Schannel too)
[[nodiscard]] const char* CertificateBundle()
{
	static const char* const kBundle = []() -> const char*
	{
		for (const char* path :
			 {"/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu, Arch
			  "/etc/pki/tls/certs/ca-bundle.crt", // Fedora, RHEL
			  "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
			  "/etc/ssl/ca-bundle.pem", // openSUSE
			  "/etc/ssl/cert.pem"}) // Alpine
			if (std::error_code error; std::filesystem::exists(path, error))
				return path;
		return nullptr;
	}();
	return kBundle;
}
#endif

// a file being downloaded: written into its .part as it comes, mapped at its size, and hashed
struct Transfer
{
	Download* download = nullptr;
	const Job* job = nullptr;
	size_t jobIndex = 0;
	uint32_t attempt = 0;
	CURL* easy = nullptr;
	std::filesystem::path part;
	core::mio_extra::resizeable_mmap_sink<std::byte> out;
	uint64_t written = 0;
	bool overflow = false;
	picosha2::hash256_one_by_one hasher;
	std::array<char, CURL_ERROR_SIZE> error{};
};

size_t WriteBody(char* data, size_t size, size_t count, void* user)
{
	auto& transfer = *static_cast<Transfer*>(user);
	size_t byteCount = size * count;
	if (transfer.written + byteCount > transfer.job->size)
	{
		transfer.overflow = true; // more than the manifest says: a changed file, or an error page
		return 0;
	}
	std::memcpy(transfer.out.data() + transfer.written, data, byteCount);
	transfer.hasher.process(data, data + byteCount);
	transfer.written += byteCount;
	transfer.download->bytesDone.fetch_add(byteCount, std::memory_order_relaxed);
	return byteCount;
}

void Fail(Download& download, std::string message)
{
	std::println(stderr, "test assets: {}", message);
	download.failedCount.fetch_add(1, std::memory_order_relaxed);
	std::scoped_lock lock(download.mutex);
	if (download.error.empty())
		download.error = std::move(message);
}

// the transfer's .part made at the job's size, and its handle; or why not
[[nodiscard]] std::expected<std::unique_ptr<Transfer>, std::string> BeginTransfer(Download& download, const Job& job, size_t jobIndex, uint32_t attempt)
{
	auto transfer = std::make_unique<Transfer>();
	transfer->download = &download;
	transfer->job = &job;
	transfer->jobIndex = jobIndex;
	transfer->attempt = attempt;
	transfer->part = job.file;
	transfer->part += ".part";

	std::error_code error;
	std::filesystem::create_directories(job.file.parent_path(), error);
	transfer->out.map(transfer->part.native(), error); // creates it
	if (error)
		return std::unexpected(std::format("{}: can't write {}: {}", job.name, transfer->part.string(), error.message()));
	if (auto resized = transfer->out.resize(job.size); !resized)
		return std::unexpected(std::format("{}: can't write {}: {}", job.name, transfer->part.string(), resized.error().message()));

	auto* easy = transfer->easy = curl_easy_init();
	curl_easy_setopt(easy, CURLOPT_URL, job.url.c_str());
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &WriteBody);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
	curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());
	curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, transfer->error.data());
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L); // an http error status fails the transfer
	curl_easy_setopt(easy, CURLOPT_USERAGENT, "speedo");
	constexpr long kConnectSeconds = 30;
	constexpr long kLowSpeedBytesPerSecond = 1024;
	constexpr long kLowSpeedSeconds = 60;
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, kConnectSeconds);
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, kLowSpeedSeconds);
#if defined(__linux__)
	if (const char* bundle = CertificateBundle())
		curl_easy_setopt(easy, CURLOPT_CAINFO, bundle);
#endif
	{
		std::scoped_lock lock(download.mutex);
		download.current = job.name;
	}
	return transfer;
}

// gives up a transfer: its .part removed, its bytes taken back from the progress
void Abandon(Transfer& transfer)
{
	transfer.out.unmap();
	curl_easy_cleanup(transfer.easy);
	std::error_code error;
	std::filesystem::remove(transfer.part, error);
	transfer.download->bytesDone.fetch_sub(transfer.written, std::memory_order_relaxed);
}

// a transfer curl is done with: its file checked and moved into place. returns whether to try it again
[[nodiscard]] bool Finish(Transfer& transfer, CURLcode result)
{
	auto& download = *transfer.download;
	const auto& job = *transfer.job;
	if (result != CURLE_OK || transfer.overflow || transfer.written != job.size)
	{
		std::string reason = transfer.overflow ? std::format("more than its {} bytes", job.size)
							 : result != CURLE_OK ? (transfer.error[0] != '\0' ? std::string(transfer.error.data()) : curl_easy_strerror(result))
												  : std::format("{} of its {} bytes", transfer.written, job.size);
		Abandon(transfer);
		if (!transfer.overflow && transfer.attempt + 1 < kAttemptCount)
		{
			std::println(stderr, "test assets: {} ({}), trying again", job.name, reason);
			return true;
		}
		Fail(download, std::format("{} failed: {} ({})", job.name, reason, job.url));
		return false;
	}

	transfer.out.unmap();
	curl_easy_cleanup(transfer.easy);
	std::error_code error;
	// mio makes a new file a page long and only grows it (see core::zip::ExtractAll)
	if (std::filesystem::file_size(transfer.part, error) != job.size && !error)
		std::filesystem::resize_file(transfer.part, job.size, error);
	std::array<uint8_t, picosha2::k_digest_size> digest{};
	transfer.hasher.finish();
	transfer.hasher.get_hash_bytes(digest.begin(), digest.end());
	auto sha256 = picosha2::bytes_to_hex_string(digest.begin(), digest.end());
	if (sha256 != job.sha256)
	{
		auto unverified = job.file;
		unverified += ".unverified";
		std::filesystem::rename(transfer.part, unverified, error);
		Fail(download, std::format("{} doesn't match its manifest (sha256 {}, expected {}): kept as {}", job.name, sha256, job.sha256, unverified.string()));
		return false;
	}
	std::filesystem::rename(transfer.part, job.file, error);
	if (error)
	{
		Fail(download, std::format("{}: can't move it into place: {}", job.name, error.message()));
		return false;
	}
	download.filesDone.fetch_add(1, std::memory_order_relaxed);
	return false;
}

void Run(const std::shared_ptr<Download>& download, const std::vector<Job>& jobs, const std::vector<Extract>& extracts)
{
	ZoneScopedN("testassets::Run");

	auto stopping = [&download] { return download->cancel.load(std::memory_order_relaxed) || core::Application::Get()->IsExitRequested(); };

	CURLM* multi = curl_multi_init();
	std::vector<std::unique_ptr<Transfer>> active;
	std::vector<std::pair<size_t, uint32_t>> queue; // the jobs, then the ones tried again: <job> <attempt>
	for (size_t jobIt = 0; jobIt < jobs.size(); jobIt++)
		queue.emplace_back(jobIt, 0);
	size_t next = 0;
	while (!stopping() && (!active.empty() || next < queue.size()))
	{
		while (active.size() < kTransferCount && next < queue.size())
		{
			auto [jobIt, attempt] = queue[next++];
			if (jobs[jobIt].size == 0)
			{
				if (auto written = core::file::Write(jobs[jobIt].file, {}); written)
					download->filesDone.fetch_add(1, std::memory_order_relaxed);
				else
					Fail(*download, std::format("{}: {}", jobs[jobIt].name, written.error().message()));
				continue;
			}
			auto transfer = BeginTransfer(*download, jobs[jobIt], jobIt, attempt);
			if (!transfer)
			{
				Fail(*download, transfer.error());
				continue;
			}
			curl_multi_add_handle(multi, (*transfer)->easy);
			active.push_back(std::move(*transfer));
		}

		int running = 0;
		curl_multi_perform(multi, &running);
		int queued = 0;
		while (CURLMsg* message = curl_multi_info_read(multi, &queued))
		{
			if (message->msg != CURLMSG_DONE)
				continue;
			Transfer* done = nullptr;
			curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &done);
			auto result = message->data.result;
			curl_multi_remove_handle(multi, message->easy_handle);
			auto found = std::ranges::find_if(active, [done](const auto& transfer) { return transfer.get() == done; });
			auto transfer = std::move(*found);
			active.erase(found);
			if (Finish(*transfer, result))
				queue.emplace_back(transfer->jobIndex, transfer->attempt + 1);
		}
		constexpr int kPollMilliseconds = 100; // how soon it sees a cancel or the exit
		curl_multi_poll(multi, nullptr, 0, kPollMilliseconds, nullptr);
	}
	for (auto& transfer : active) // stopped
	{
		curl_multi_remove_handle(multi, transfer->easy);
		Abandon(*transfer);
	}
	curl_multi_cleanup(multi);

	// the zips that are all there, extracted unless they were (their directory's marker)
	auto cancelled = [&stopping] { return stopping(); };
	for (const auto& extract : extracts)
	{
		std::error_code error;
		if (stopping() || std::filesystem::exists(extract.directory / kExtractedMarker, error) ||
			!std::ranges::all_of(extract.zips, [](const auto& zip) { return Present(zip.first, zip.second); }))
			continue;
		download->extracting.store(true, std::memory_order_relaxed);
		{
			std::scoped_lock lock(download->mutex);
			download->current = extract.name;
		}
		std::filesystem::remove_all(extract.directory, error);
		bool extracted = true;
		for (const auto& [zip, size] : extract.zips)
			if (auto result = core::zip::ExtractAll(zip, extract.directory / zip.stem(), &core::Application::Get()->GetExecutor(), nullptr, cancelled); !result)
			{
				Fail(*download, std::format("{}: {}", zip.string(), result.error()));
				extracted = false;
				break;
			}
		if (extracted)
			(void)core::file::Write(extract.directory / kExtractedMarker, {});
		download->extracting.store(false, std::memory_order_relaxed);
	}

	download->done.store(true, std::memory_order_release);
}

} // namespace

SetSize Measure(Set set)
{
	auto selection = Select(set);
	SetSize size;
	for (size_t manifestIt = 0; manifestIt < selection.manifests.size(); manifestIt++)
	{
		const auto& manifest = selection.manifests[manifestIt];
		for (size_t entryIt : selection.entries[manifestIt])
		{
			const auto& entry = manifest.entries[entryIt];
			size.byteCount += entry.size;
			size.fileCount++;
			if (!Present(FileOf(manifest, entry), entry.size))
			{
				size.missingByteCount += entry.size;
				size.missingFileCount++;
			}
		}
	}
	return size;
}

std::shared_ptr<Download> Start(Set set)
{
	InitCurl();

	auto download = std::make_shared<Download>();
	download->set = set;
	download->start = std::chrono::steady_clock::now();

	auto selection = Select(set);
	std::vector<Job> jobs;
	std::vector<Extract> extracts;
	for (size_t manifestIt = 0; manifestIt < selection.manifests.size(); manifestIt++)
	{
		const auto& manifest = selection.manifests[manifestIt];
		for (size_t entryIt : selection.entries[manifestIt])
		{
			const auto& entry = manifest.entries[entryIt];
			if (auto file = FileOf(manifest, entry); !Present(file, entry.size))
				jobs.push_back(Job{
					.url = UrlOf(manifest.url, entry.source),
					.file = std::move(file),
					.size = entry.size,
					.sha256 = entry.sha256,
					.name = std::format("{}/{}", manifest.dir, entry.path)});
		}
		// a directory of zips the set has
		for (const auto& [from, into] : manifest.extracts)
		{
			Extract extract{.directory = Variable("TestAssetsPath") / Utf8Path(manifest.dir) / Utf8Path(into), .name = std::format("{}/{}", manifest.dir, into)};
			for (size_t entryIt : selection.entries[manifestIt])
				if (const auto& entry = manifest.entries[entryIt]; entry.path.starts_with(from + "/"))
					extract.zips.emplace_back(FileOf(manifest, entry), entry.size);
			if (!extract.zips.empty())
				extracts.push_back(std::move(extract));
		}
	}

	uint64_t byteCount = 0;
	for (const auto& job : jobs)
		byteCount += job.size;
	download->byteCount.store(byteCount, std::memory_order_relaxed);
	download->fileCount.store(static_cast<uint32_t>(jobs.size()), std::memory_order_relaxed);

	auto task = core::CreateTask([download, jobs = std::move(jobs), extracts = std::move(extracts)] { Run(download, jobs, extracts); });
	core::Application::Get()->GetExecutor().Submit(std::span(&task.handle, 1));
	return download;
}

namespace
{

// what the user chose (the user profile's test-assets.txt): a set (resumed at startup until it is all there), or not now
enum class Choice : uint8_t
{
	kNone,
	kQuick,
	kFull,
	kDeclined,
};
constexpr std::array<std::string_view, 4> kChoiceNames{"", "quick", "full", "declined"};

[[nodiscard]] std::filesystem::path ChoicePath()
{
	return Variable("UserProfilePath") / "test-assets.txt";
}

[[nodiscard]] Choice ReadChoice()
{
	Choice choice = Choice::kNone;
	ForEachLine(
		ChoicePath(),
		[&choice](std::string_view row)
		{
			if (auto found = std::ranges::find(kChoiceNames, row); found != kChoiceNames.end() && choice == Choice::kNone)
				choice = static_cast<Choice>(found - kChoiceNames.begin());
		});
	return choice;
}

void WriteChoice(Choice choice)
{
	auto text = std::format("{}\n", kChoiceNames[static_cast<size_t>(choice)]);
	if (auto written = core::file::Write(ChoicePath(), std::as_bytes(std::span(text))); !written)
		std::println(stderr, "Failed to write {}: {}", ChoicePath().string(), written.error().message());
}

// a size for people: decimal megabytes or gigabytes, as the sets are described
[[nodiscard]] std::string Bytes(uint64_t byteCount)
{
	constexpr double kMegabyte = 1e6;
	constexpr double kGigabyte = 1e9;
	auto bytes = static_cast<double>(byteCount);
	return bytes >= kGigabyte ? std::format("{:.2f} GB", bytes / kGigabyte) : std::format("{:.0f} MB", std::ceil(bytes / kMegabyte));
}

// the ui's (ui thread): the download in progress or last, and whether to offer the sets
std::shared_ptr<Download> gDownload;
bool gOfferRequested = false;

void StartDownload(Set set)
{
	WriteChoice(set == Set::kQuick ? Choice::kQuick : Choice::kFull);
	gDownload = Start(set);
}

} // namespace

void DrawMenuItem()
{
	bool running = gDownload && !gDownload->done.load(std::memory_order_acquire);
	if (ImGui::MenuItem("Download Test Assets...", nullptr, false, !running))
		gOfferRequested = true;
	ImGui::SetItemTooltip(
		"Models and panoramas to try the renderer on, downloaded into %s (the file browser opens there): a model or two per feature, or all of them.",
		Variable("TestAssetsPath").string().c_str());
}

void Draw()
{
	using namespace ImGui;

	// a first launch: the sets offered, unless the quick set is there (a checkout's fetched assets) or the run is automated
	// (the regression's clients: they would wait for the choice); a set chosen before, resumed until it is all there.
	// SPEEDO_DOWNLOAD_TEST_ASSETS (tests): offer opens the offer, quick or full downloads that set and exits once done,
	// printing what it did
	static std::string_view gTest;
	if (static bool gStarted = false; !std::exchange(gStarted, true))
	{
		const char* test = std::getenv("SPEEDO_DOWNLOAD_TEST_ASSETS");
		gTest = test != nullptr ? test : "";
		const char* exitFrames = std::getenv("SPEEDO_AUTOLOAD_EXIT");
		const char* background = std::getenv("SPEEDO_BACKGROUND");
		bool automated = (exitFrames != nullptr && *exitFrames != '\0') || (background != nullptr && std::string_view(background) == "1") ||
						 PlatformIsHeadless();
		auto choice = ReadChoice();
		if (gTest == "offer")
			gOfferRequested = true;
		else if (gTest == "quick" || gTest == "full")
			gDownload = Start(gTest == "quick" ? Set::kQuick : Set::kFull);
		else if (automated || choice == Choice::kDeclined)
		{
		}
		else if (choice == Choice::kQuick || choice == Choice::kFull)
		{
			auto set = choice == Choice::kQuick ? Set::kQuick : Set::kFull;
			if (Measure(set).missingFileCount > 0)
				gDownload = Start(set);
		}
		else if (Measure(Set::kQuick).missingFileCount > 0)
			gOfferRequested = true;
	}

	static SetSize gQuickSize;
	static SetSize gFullSize;
	if (std::exchange(gOfferRequested, false))
	{
		gQuickSize = Measure(Set::kQuick);
		gFullSize = Measure(Set::kFull);
		OpenPopup("Test Assets");
	}
	if (BeginPopupModal("Test Assets", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		TextUnformatted("Models and panoramas to try the renderer on, downloaded into");
		TextDisabled("%s", Variable("TestAssetsPath").string().c_str());
		TextUnformatted("where the file browser opens (File > Open...).");
		Spacing();
		constexpr float kButtonWidth = 110.0F;
		auto offer = [](const char* label, const char* what, const SetSize& size)
		{
			bool chosen = Button(label, ImVec2(kButtonWidth, 0.0F));
			SameLine();
			if (size.missingFileCount == 0)
				Text("%s: %u files, all there", what, size.fileCount);
			else if (size.missingFileCount == size.fileCount)
				Text("%s: %u files, %s", what, size.fileCount, Bytes(size.byteCount).c_str());
			else
				Text("%s: %u files, %s (%s of them there)", what, size.fileCount, Bytes(size.missingByteCount).c_str(), Bytes(size.byteCount - size.missingByteCount).c_str());
			return chosen;
		};
		std::optional<Set> chosen;
		if (offer("Quick Set", "A model or two per feature", gQuickSize))
			chosen = Set::kQuick;
		if (offer("Everything", "Every model and panorama", gFullSize))
			chosen = Set::kFull;
		Spacing();
		TextUnformatted("Each model's license comes with it (its LICENSE.md, or in its archive). From:");
		Bullet();
		TextLinkOpenURL("Morgan McGuire's Computer Graphics Archive", "https://casual-effects.com/data");
		Bullet();
		TextLinkOpenURL("The Khronos glTF Sample Assets", "https://github.com/KhronosGroup/glTF-Sample-Assets");
		Bullet();
		TextLinkOpenURL("The Khronos glTF Sample Environments", "https://github.com/KhronosGroup/glTF-Sample-Environments");
		Spacing();
		if (Button("Not Now", ImVec2(kButtonWidth, 0.0F)))
		{
			WriteChoice(Choice::kDeclined);
			CloseCurrentPopup();
		}
		SetItemTooltip("File > Download Test Assets... offers them again.");
		if (chosen)
		{
			StartDownload(*chosen);
			CloseCurrentPopup();
		}
		EndPopup();
	}

	if (!gDownload)
		return;

	// the download's progress, at the bottom right, without taking the focus
	auto& download = *gDownload;
	if (static bool gReported = false; (gTest == "quick" || gTest == "full") && !gReported && download.done.load(std::memory_order_acquire))
	{
		gReported = true;
		std::println(
			"test assets: {} of {} files, {} of {} bytes, {} failed, in {:.1f} s",
			download.filesDone.load(std::memory_order_relaxed),
			download.fileCount.load(std::memory_order_relaxed),
			download.bytesDone.load(std::memory_order_relaxed),
			download.byteCount.load(std::memory_order_relaxed),
			download.failedCount.load(std::memory_order_relaxed),
			std::chrono::duration<double>(std::chrono::steady_clock::now() - download.start).count());
		core::Application::Get()->RequestExit();
	}
	// at its width (the file names would change it), its height fitted: placed where it is wanted from its first frame,
	// before an auto resizing window knows its size
	constexpr float kPadding = 10.0F;
	constexpr float kWidth = 400.0F;
	const auto* viewport = GetMainViewport();
	SetNextWindowPos(
		ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kPadding, viewport->WorkPos.y + viewport->WorkSize.y - kPadding),
		ImGuiCond_Always,
		ImVec2(1.0F, 1.0F));
	SetNextWindowSizeConstraints(ImVec2(kWidth, 0.0F), ImVec2(kWidth, std::numeric_limits<float>::max()));
	bool open = true;
	if (Begin(
			"Test Assets Download",
			&open,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove))
	{
		bool done = download.done.load(std::memory_order_acquire);
		auto byteCount = download.byteCount.load(std::memory_order_relaxed);
		auto bytesDone = download.bytesDone.load(std::memory_order_relaxed);
		auto failedCount = download.failedCount.load(std::memory_order_relaxed);
		std::string current;
		std::string error;
		{
			std::scoped_lock lock(download.mutex);
			current = download.current;
			error = download.error;
		}
		auto overlay = std::format("{} of {}", Bytes(bytesDone), Bytes(byteCount));
		ProgressBar(
			byteCount > 0 ? static_cast<float>(bytesDone) / static_cast<float>(byteCount) : (done ? 1.0F : 0.0F),
			ImVec2(-std::numeric_limits<float>::min(), 0.0F),
			overlay.c_str());
		auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - download.start).count();
		auto rate = seconds > 0.0 ? static_cast<double>(bytesDone) / seconds : 0.0;
		auto files = std::format("{} of {} files", download.filesDone.load(std::memory_order_relaxed), download.fileCount.load(std::memory_order_relaxed));
		if (!done)
		{
			constexpr double kMegabyte = 1e6;
			constexpr uint64_t kMinute = 60;
			if (rate > 0.0 && bytesDone < byteCount)
			{
				auto left = static_cast<uint64_t>(static_cast<double>(byteCount - bytesDone) / rate);
				files += std::format(", {:.1f} MB/s, {}:{:02} left", rate / kMegabyte, left / kMinute, left % kMinute);
			}
			TextUnformatted(files.c_str());
			// the end of a long path (what tells the files apart), clipped by the window
			constexpr size_t kShownLength = 64;
			auto shown = current.size() > kShownLength ? "..." + current.substr(current.size() - kShownLength) : current;
			TextDisabled("%s%s", download.extracting.load(std::memory_order_relaxed) ? "extracting " : "", shown.c_str());
			if (Button("Cancel"))
			{
				download.cancel.store(true, std::memory_order_relaxed);
				WriteChoice(Choice::kDeclined); // not resumed at startup
			}
			SetItemTooltip("Stops it, keeping the files done. File > Download Test Assets... goes on.");
		}
		else
		{
			TextUnformatted(files.c_str());
			if (download.cancel.load(std::memory_order_relaxed))
				TextUnformatted("Cancelled.");
			else if (failedCount == 0)
				TextUnformatted("Done: File > Open... browses them.");
			if (failedCount > 0)
			{
				constexpr ImVec4 kFailureColor{1.0F, 0.45F, 0.4F, 1.0F};
				PushTextWrapPos(0.0F); // at the window's edge
				TextColored(kFailureColor, "%u failed: %s", failedCount, error.c_str());
				PopTextWrapPos();
				if (Button("Retry"))
					StartDownload(download.set);
				SameLine();
			}
			if (Button("Close"))
				open = false;
		}
	}
	End();
	if (!open)
	{
		if (!gDownload->done.load(std::memory_order_acquire))
		{
			gDownload->cancel.store(true, std::memory_order_relaxed);
			WriteChoice(Choice::kDeclined);
		}
		gDownload.reset();
	}
}

} // namespace client::testassets
