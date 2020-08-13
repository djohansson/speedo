#include "file.h"

#include <tuple>

#include <cereal/types/vector.hpp>

#include <picosha2.h>

std::string getFileTimeStamp(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::last_write_time(filePath).time_since_epoch());
	std::chrono::seconds s = std::chrono::duration_cast<std::chrono::seconds>(ms);
	std::time_t timestamp = s.count();

	return std::string(std::asctime(std::localtime(&timestamp)));
}

std::tuple<FileState, FileInfo> getFileInfo(const std::filesystem::path& filePath, bool sha2Enable)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(FileState::Missing, FileInfo{});

	FileInfo outFileInfo;

	if (sha2Enable)
	{
		ZoneScopedN("getFileInfo::sha2");

		mio::mmap_source file(filePath.string());
		outFileInfo.sha2.resize(picosha2::k_digest_size);
		picosha2::hash256(
			file.begin(), file.end(), outFileInfo.sha2.begin(), outFileInfo.sha2.end());
	}

	outFileInfo.path = filePath.generic_string();
	outFileInfo.size = std::filesystem::file_size(filePath);
	outFileInfo.timeStamp = getFileTimeStamp(filePath);

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

std::tuple<FileState, FileInfo> getFileInfo(
	const std::filesystem::path& filePath,
	const std::string& id,
	const std::string& loaderType,
	const std::string& loaderVersion,
	std::istream& jsonStream,
	std::function<std::tuple<std::string, std::string, FileInfo>(std::istream&, const std::string&)> loadJSON,
	bool sha2Enable)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(FileState::Missing, FileInfo{});

	auto [_loaderType, _loaderVersion, _fileInfo] = loadJSON(jsonStream, id);

	if (loaderType.compare(_loaderType) != 0 || loaderVersion.compare(_loaderVersion) != 0)
		return std::make_tuple(FileState::Stale, FileInfo{});

	int64_t fileSize = std::filesystem::file_size(filePath);
	std::string fileTimeStamp = getFileTimeStamp(filePath);
	std::vector<unsigned char> fileSha2(picosha2::k_digest_size);
	if (sha2Enable)
	{
		ZoneScopedN("getFileInfo::sha2");

		mio::mmap_source file(filePath.string());
		picosha2::hash256(file.begin(), file.end(), fileSha2.begin(), fileSha2.end());
	}

	// perhaps add path check as well?
	if (fileSize != _fileInfo.size || fileTimeStamp.compare(_fileInfo.timeStamp) != 0 || sha2Enable
			? fileSha2 != _fileInfo.sha2 : false)
		return std::make_tuple(FileState::Stale, FileInfo{});

	FileInfo outFileInfo;
	outFileInfo.path = filePath.generic_string();
	outFileInfo.size = fileSize;
	outFileInfo.timeStamp = std::move(fileTimeStamp);
	outFileInfo.sha2 = std::move(fileSha2);

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

std::tuple<FileState, FileInfo> loadBinaryFile(
	const std::filesystem::path& filePath,
	std::function<void(std::istream&)> loadOp,
	bool sha2Enable)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(FileState::Missing, FileInfo{});

	FileInfo outFileInfo;

	// intended scope - fileStreamBuf needs to be destroyed before we call std::filesystem::file_size
	{
		ZoneScopedN("loadBinaryFile::loadOp");

		mio::mmap_istreambuf fileStreamBuf(filePath.string());
		std::istream fileStream(&fileStreamBuf);

		loadOp(fileStream);

		if (sha2Enable)
		{
			ZoneScopedN("loadBinaryFile::sha2");

			fileStream.clear();
			fileStream.seekg(0, std::ios_base::beg);

			outFileInfo.sha2.resize(picosha2::k_digest_size);

			picosha2::hash256(
				std::istreambuf_iterator(&fileStreamBuf),
				std::istreambuf_iterator<decltype(fileStreamBuf)::char_type>(),
				outFileInfo.sha2.begin(),
				outFileInfo.sha2.end());
		}
	}

	outFileInfo.path = filePath.generic_string();
	outFileInfo.size = std::filesystem::file_size(filePath);
	outFileInfo.timeStamp = getFileTimeStamp(filePath);

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

std::tuple<FileState, FileInfo> saveBinaryFile(
	const std::filesystem::path& filePath,
	std::function<void(std::iostream&)> saveOp,
	bool sha2Enable)
{
	ZoneScoped;

	FileInfo outFileInfo;

	// intended scope - fileStreamBuf needs to be destroyed before we call std::filesystem::file_size
	{
		ZoneScopedN("saveBinaryFile::saveOp");

		mio::mmap_iostreambuf fileStreamBuf(filePath.string());
		std::iostream fileStream(&fileStreamBuf);

		saveOp(fileStream);

		fileStream.sync();

		if (sha2Enable)
		{
			fileStream.seekg(0, std::ios_base::beg);

			outFileInfo.sha2.resize(picosha2::k_digest_size);

			picosha2::hash256(
				std::istreambuf_iterator(&fileStreamBuf),
				std::istreambuf_iterator<decltype(fileStreamBuf)::char_type>(),
				outFileInfo.sha2.begin(),
				outFileInfo.sha2.end());
		}
	}

	outFileInfo.path = filePath.generic_string();
	outFileInfo.size = std::filesystem::file_size(filePath);
	outFileInfo.timeStamp = getFileTimeStamp(filePath);

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

void loadCachedSourceFile(
	const std::filesystem::path& sourceFilePath,
	const std::filesystem::path& cacheFilePath,
	const std::string& loaderType,
	const std::string& loaderVersion,
	LoadFileFn loadSourceFileFn,
	LoadFileFn loadBinaryCacheFn,
	SaveFileFn saveBinaryCacheFn)
{
	ZoneScoped;

	std::filesystem::path jsonFilePath(sourceFilePath);
	jsonFilePath += ".pbin.json";

	std::filesystem::path pbinFilePath(cacheFilePath);
	pbinFilePath += ".pbin";

	bool doImport;
	std::tuple<FileState, FileInfo> sourceFile, pbinFile;
	auto jsonFileStatus = std::filesystem::status(jsonFilePath);
	auto pbinFileStatus = std::filesystem::status(pbinFilePath);

	if (std::filesystem::exists(jsonFileStatus) &&
		std::filesystem::is_regular_file(jsonFileStatus) &&
		!std::filesystem::exists(pbinFileStatus))
	{
		ZoneScopedN("loadCachedSourceFile::deleteJson");

		std::filesystem::remove(jsonFilePath);

		doImport = true;
	}
	else if (
		std::filesystem::exists(jsonFileStatus) && std::filesystem::is_regular_file(jsonFileStatus))
	{
		ZoneScopedN("loadCachedSourceFile::readJsonAndFileState");

		auto loadJSONFn = [](std::istream& stream, const std::string& id) {
			cereal::JSONInputArchive json(stream);

			std::string outLoaderType;
			std::string outLoaderVersion;
			FileInfo outFileInfo;

			json(cereal::make_nvp("loaderType", outLoaderType));
			json(cereal::make_nvp("loaderVersion", outLoaderVersion));
			json(cereal::make_nvp(id, outFileInfo));

			return std::make_tuple(outLoaderType, outLoaderVersion, outFileInfo);
		};

		doImport = false;

		mio::mmap_istreambuf fileStreamBuf(jsonFilePath.string());
		std::istream fileStream(&fileStreamBuf);

		sourceFile = getFileInfo(
			sourceFilePath,
			"sourceFileInfo",
			loaderType,
			loaderVersion,
			fileStream,
			loadJSONFn,
			false);

		fileStream.clear();
		fileStream.seekg(0, std::ios_base::beg);

		pbinFile = getFileInfo(
			pbinFilePath,
			"pbinFileInfo",
			loaderType,
			loaderVersion,
			fileStream,
			loadJSONFn,
			false);
	}
	else
	{
		doImport = true;
	}

	auto& [sourceFileState, sourceFileInfo] = sourceFile;
	auto& [pbinFileState, pbinFileInfo] = pbinFile;

	if (doImport || sourceFileState == FileState::Stale || pbinFileState != FileState::Valid)
	{
		ZoneScopedN("loadCachedSourceFile::importSourceFile");

		mio::mmap_ostreambuf streamBuf(jsonFilePath.string());
		std::ostream stream(&streamBuf);
		cereal::JSONOutputArchive json(stream);

		json(cereal::make_nvp("loaderType", loaderType));
		json(cereal::make_nvp("loaderVersion", loaderVersion));

		auto [sourceFileState, sourceFileInfo] = loadBinaryFile(sourceFilePath, loadSourceFileFn, true);
		json(CEREAL_NVP(sourceFileInfo));

		auto [pbinFileState, pbinFileInfo] = saveBinaryFile(pbinFilePath, saveBinaryCacheFn, true);
		json(CEREAL_NVP(pbinFileInfo));
	}
	else
	{
		ZoneScopedN("loadCachedSourceFile::loadPbin");

		auto [pbinFileState, pbinFileInfo] = loadBinaryFile(pbinFilePath, loadBinaryCacheFn, false);
	}
}
