#include "profiling.h"
#include "typeinfo.h"

#include <client/client.h> // TODO: eliminate this dependency

#include <cereal/cereal.hpp>
#include <cereal/types/array.hpp>
#include <cereal/types/string.hpp>

#include <mio/mmap_iostream.hpp>

template <class Archive>
void load(Archive& archive, FileInfo& info)
{
	archive(cereal::make_nvp("path", info.path));
	archive(cereal::make_nvp("size", info.size));
	archive(cereal::make_nvp("timeStamp", info.timeStamp));
	archive.loadBinaryValue(info.sha2.data(), info.sha2.size(), "sha2");
}

template <class Archive>
void save(Archive& archive, const FileInfo& info)
{
	archive(cereal::make_nvp("path", info.path));
	archive(cereal::make_nvp("size", info.size));
	archive(cereal::make_nvp("timeStamp", info.timeStamp));
	archive.saveBinaryValue(info.sha2.data(), info.sha2.size(), "sha2");
}

template <typename T, typename Archive>
T loadObject(std::istream& stream, std::string_view name)
{
	Archive archive(stream);
	T outValue{};
	archive(cereal::make_nvp(std::string(name), outValue));
	return outValue;
};

template <typename T, typename Archive>
std::tuple<std::optional<T>, FileState>
loadObject(const std::filesystem::path& filePath, std::string_view name)
{
	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return std::make_tuple(std::nullopt, FileState::Missing);

	auto fileStream = mio::mmap_istream(filePath.string());

	return std::make_tuple(
		std::make_optional(loadObject<T, Archive>(fileStream, name)), FileState::Valid);
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::FileObject(
	const std::filesystem::path& filePath, T&& defaultObject)
	: T(std::get<0>(loadObject<T, InputArchive>(filePath, getTypeName<std::decay_t<T>>()))
			.value_or(std::forward<T>(defaultObject)))
	, myInfo{filePath}
{}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::FileObject(
	FileObject&& other) noexcept
	: T(std::forward<FileObject>(other))
	, myInfo(std::exchange(other.myInfo, {}))
{}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::~FileObject()
{
	if constexpr (SaveOnClose)
		if (!myInfo.path.empty())
			save();
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>&
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::operator=(
	FileObject&& other) noexcept
{
	myInfo = std::exchange(other.myInfo, {});
	return *this;
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
void FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::swap(FileObject& rhs) noexcept
{
	std::swap<T>(*this, rhs);
	std::swap(myInfo, rhs.myInfo);
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
void FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::reload()
{
	static_cast<T&>(*this) =
		std::get<0>(loadObject<T, InputArchive>(myInfo.path, getTypeName<std::decay_t<T>>()))
			.value_or(std::move(static_cast<T&>(*this)));
}

template <
	typename T,
	FileAccessMode Mode,
	typename InputArchive,
	typename OutputArchive,
	bool SaveOnClose>
template <FileAccessMode M>
typename std::enable_if<M == FileAccessMode::ReadWrite, void>::type
FileObject<T, Mode, InputArchive, OutputArchive, SaveOnClose>::save() const
{
	auto fileStream = mio::mmap_ostream(myInfo.path.string());
	OutputArchive archive(fileStream);
	archive(cereal::make_nvp(std::string(getTypeName<std::decay_t<T>>()), static_cast<const T&>(*this))); 
}

template <bool Sha256ChecksumEnable>
FileInfo getFileInfo(const std::filesystem::path& filePath)
{
	ZoneScoped;

	auto fileStatus = std::filesystem::status(filePath);
	if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
		return FileInfo{filePath, 0, "", {}, FileState::Missing};

	auto outFileInfo = FileInfo{filePath, std::filesystem::file_size(filePath), getFileTimeStamp(filePath), {}, FileState::Valid};

	if constexpr (Sha256ChecksumEnable)
	{
		ZoneScopedN("getFileInfo::sha2");

		mio::mmap_source file(filePath.string());
		picosha2::hash256(file.begin(), file.end(), outFileInfo.sha2.begin(), outFileInfo.sha2.end());
	}

	return outFileInfo;
}

template <const char* Id, const char* LoaderType, const char* LoaderVersion, bool Sha256ChecksumEnable>
FileInfo getAndCheckFileInfoFromManifest(std::istream& stream, LoadManifestFn loadManifestFn)
{
	ZoneScoped;

	auto [loaderTypeStr, loaderVersionStr, manifestFileInfo] = loadManifestFn(stream, std::string_view(Id));

	if (std::string_view(LoaderType).compare(loaderTypeStr) != 0 ||
		std::string_view(LoaderVersion).compare(loaderVersionStr) != 0)
		return FileInfo{manifestFileInfo.path, 0, "", {}, FileState::Stale};

	auto outFileInfo = getFileInfo<Sha256ChecksumEnable>(manifestFileInfo.path);

	if (outFileInfo.size != manifestFileInfo.size ||
		outFileInfo.timeStamp.compare(manifestFileInfo.timeStamp) != 0 ||
		Sha256ChecksumEnable ? outFileInfo.sha2 != manifestFileInfo.sha2 : false)
		return FileInfo{manifestFileInfo.path, 0, "", {}, FileState::Stale};

	return outFileInfo;
}

template <bool Sha256ChecksumEnable>
FileInfo loadBinaryFile(const std::filesystem::path& filePath, LoadFileFn loadOp)
{
	ZoneScoped;

	auto fileInfo = getFileInfo<Sha256ChecksumEnable>(filePath);

	if (fileInfo.state == FileState::Valid)
		loadOp(mio::mmap_istream(fileInfo.path.string()));

	return fileInfo;
}

template <bool Sha256ChecksumEnable>
FileInfo saveBinaryFile(const std::filesystem::path& filePath, SaveFileFn saveOp)
{
	ZoneScoped;

	// todo: check if file exist and prompt for overwrite?
	// intended scope - fileStreamBuf needs to be destroyed before we call std::filesystem::file_size
	{
		mio::mmap_iostreambuf fileStreamBuf(filePath.string());
		saveOp(std::iostream(&fileStreamBuf));
	}

	return getFileInfo<Sha256ChecksumEnable>(filePath);
}

template <const char* LoaderType, const char* LoaderVersion>
void loadCachedSourceFile(
	const std::filesystem::path& sourceFilePath,
	LoadFileFn loadSourceFileFn,
	LoadFileFn loadBinaryCacheFn,
	SaveFileFn saveBinaryCacheFn)
{
	ZoneScoped;
	
	std::filesystem::path manifestFilePath(sourceFilePath);
	manifestFilePath += ".json";
	
	bool doImport;
	FileInfo sourceFileInfo, cacheFileInfo;
	auto manifestFileStatus = std::filesystem::status(manifestFilePath);

	if (std::filesystem::exists(manifestFileStatus) &&
		std::filesystem::is_regular_file(manifestFileStatus))
	{
		ZoneScopedN("loadCachedSourceFile::loadManifest");

		auto loadManifestFn = [](std::istream& stream, std::string_view id)
		{
			cereal::JSONInputArchive archive(stream);

			std::string outLoaderType;
			std::string outLoaderVersion;
			FileInfo outFileInfo;

			archive(cereal::make_nvp("loaderType", outLoaderType));
			archive(cereal::make_nvp("loaderVersion", outLoaderVersion));
			archive(cereal::make_nvp(id.data(), outFileInfo));

			return std::make_tuple(outLoaderType, outLoaderVersion, outFileInfo);
		};

		doImport = false;

		auto fileStream = mio::mmap_istream(manifestFilePath.string());

		static constexpr char sourceFileInfoIdStr[] = "sourceFileInfo";
		sourceFileInfo = getAndCheckFileInfoFromManifest<sourceFileInfoIdStr, LoaderType, LoaderVersion, false>(fileStream, loadManifestFn);

		fileStream.clear();
		fileStream.seekg(0, std::ios_base::beg);

		static constexpr char cacheFileInfoIdStr[] = "cacheFileInfo";
		cacheFileInfo = getAndCheckFileInfoFromManifest<cacheFileInfoIdStr, LoaderType, LoaderVersion, false>(fileStream, loadManifestFn);
	}
	else
	{
		doImport = true;
	}

	if (doImport || sourceFileInfo.state == FileState::Stale || cacheFileInfo.state != FileState::Valid)
	{
		ZoneScopedN("loadCachedSourceFile::importSourceFile");

		auto fileStream = mio::mmap_ostream(manifestFilePath.string());
		
		cereal::JSONOutputArchive archive(fileStream);

		static const std::string loaderTypeStr(LoaderType);
		static const std::string loaderVersionStr(LoaderVersion);

		archive(cereal::make_nvp("loaderType", loaderTypeStr));
		archive(cereal::make_nvp("loaderVersion", loaderVersionStr));

		sourceFileInfo = loadBinaryFile<true>(sourceFilePath, loadSourceFileFn);
		archive(cereal::make_nvp("sourceFileInfo", sourceFileInfo));

		auto cacheDir = std::filesystem::path(client_getUserProfilePath()) / ".cache";
		auto cacheDirStatus = std::filesystem::status(cacheDir);
		if (!std::filesystem::exists(cacheDirStatus) ||
			!std::filesystem::is_directory(cacheDirStatus))
			std::filesystem::create_directory(cacheDir);

		auto id = uuids::uuid_system_generator{}();
		auto idStr = uuids::to_string(id);

		cacheFileInfo = saveBinaryFile<true>(cacheDir / idStr, saveBinaryCacheFn);
		archive(cereal::make_nvp("cacheFileInfo", cacheFileInfo));
	}
	else
	{
		ZoneScopedN("loadCachedSourceFile::load");

		cacheFileInfo = loadBinaryFile<false>(cacheFileInfo.path, loadBinaryCacheFn);
	}
}
