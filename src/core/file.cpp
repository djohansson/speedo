#include "file.h"

#include <stduuid/uuid.h>

std::string getFileTimeStamp(const std::filesystem::path& filePath)
{
	ZoneScoped;

	std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::last_write_time(filePath).time_since_epoch());
	std::chrono::seconds s = std::chrono::duration_cast<std::chrono::seconds>(ms);
	std::time_t timestamp = s.count();

	return std::string(std::asctime(std::localtime(&timestamp)));
}

// 	if (!saveOp(fileStream))
// 		return std::make_tuple(FileState::Stale, std::move(outFileInfo));

// 	fileStream.sync();
// 	fileStream.seekg(0, std::ios_base::beg);

// 	picosha2::hash256(
// 		std::istreambuf_iterator(&fileStreamBuf),
// 		std::istreambuf_iterator<decltype(fileStreamBuf)::char_type>(),
// 		outFileInfo.sha2.begin(),
// 		outFileInfo.sha2.end());

// outFileInfo.size = std::filesystem::file_size(outFileInfo.path);
// outFileInfo.timeStamp = getFileTimeStamp(outFileInfo.path);