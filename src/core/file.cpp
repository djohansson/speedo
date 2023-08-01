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
