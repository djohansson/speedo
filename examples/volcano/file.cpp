#include "file.h"
#include "profiling.h"

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

	auto outFileInfo = FileInfo{filePath, std::filesystem::file_size(filePath), getFileTimeStamp(filePath)};

	if (sha2Enable)
	{
		ZoneScopedN("getFileInfo::sha2");

		mio::mmap_source file(filePath.string());
		picosha2::hash256(
			file.begin(),
			file.end(),
			outFileInfo.sha2.begin(),
			outFileInfo.sha2.end());
	}

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

std::tuple<FileState, FileInfo> loadBinaryFile(
	const std::filesystem::path& filePath,
	LoadFileFn loadOp,
	bool sha2Enable)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(FileState::Missing, FileInfo{});

	auto outFileInfo = FileInfo{filePath, std::filesystem::file_size(filePath), getFileTimeStamp(filePath)};

	{
		ZoneScopedN("loadBinaryFile::loadOp");

		auto fileStream = mio::mmap_istream(filePath.string());

		if (!loadOp(fileStream))
			return std::make_tuple(FileState::Stale, std::move(outFileInfo));

		if (sha2Enable)
		{
			ZoneScopedN("loadBinaryFile::sha2");

			fileStream.clear();
			fileStream.seekg(0, std::ios_base::beg);
			
			picosha2::hash256(
				std::istreambuf_iterator(fileStream),
				std::istreambuf_iterator<mio::mmap_istreambuf::char_type>(),
				outFileInfo.sha2.begin(),
				outFileInfo.sha2.end());
		}
	}

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}

std::tuple<FileState, FileInfo> saveBinaryFile(
	const std::filesystem::path& filePath,
	SaveFileFn saveOp,
	bool sha2Enable)
{
	ZoneScoped;

	auto outFileInfo = FileInfo{filePath};

	// intended scope - fileStreamBuf needs to be destroyed before we call std::filesystem::file_size
	{
		ZoneScopedN("saveBinaryFile::saveOp");

		mio::mmap_iostreambuf fileStreamBuf(filePath.string());
		std::iostream fileStream(&fileStreamBuf);

		if (!saveOp(fileStream))
			return std::make_tuple(FileState::Stale, std::move(outFileInfo));

		fileStream.sync();

		if (sha2Enable)
		{
			fileStream.seekg(0, std::ios_base::beg);

			picosha2::hash256(
				std::istreambuf_iterator(&fileStreamBuf),
				std::istreambuf_iterator<decltype(fileStreamBuf)::char_type>(),
				outFileInfo.sha2.begin(),
				outFileInfo.sha2.end());
		}
	}

	outFileInfo.size = std::filesystem::file_size(filePath);
	outFileInfo.timeStamp = getFileTimeStamp(filePath);

	return std::make_tuple(FileState::Valid, std::move(outFileInfo));
}


