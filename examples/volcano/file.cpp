#include "file.h"

#include <cereal/archives/json.hpp>
#include <cereal/types/vector.hpp>

std::string getFileTimeStamp(const std::filesystem::path &filePath)
{
    std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::filesystem::last_write_time(filePath).time_since_epoch());
    std::chrono::seconds s = std::chrono::duration_cast<std::chrono::seconds>(ms);
    std::time_t timestamp = s.count();

    return std::string(std::asctime(std::localtime(&timestamp)));
}

FileState getFileInfo(
    const std::filesystem::path &filePath,
    FileInfo &outFileInfo,
    bool sha2Enable)
{
    outFileInfo.clear();

    auto fileStatus = std::filesystem::status(filePath);
    if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
        return FileState::Missing;

    if (sha2Enable)
    {
        mio::mmap_source file(filePath.string());
        outFileInfo.sha2.resize(picosha2::k_digest_size);
        picosha2::hash256(file.begin(), file.end(), outFileInfo.sha2.begin(), outFileInfo.sha2.end());
    }

    outFileInfo.path = filePath;
    outFileInfo.size = std::filesystem::file_size(filePath);
    outFileInfo.timeStamp = getFileTimeStamp(filePath);

    return FileState::Valid;
}

FileState getFileInfo(
    const std::filesystem::path &filePath,
    const std::string &id,
    const std::string &loaderType,
    const std::string &loaderVersion,
    std::istream &jsonStream,
    std::function<std::tuple<std::string, std::string, FileInfo>(std::istream&, const std::string&)> loadJSON,
    FileInfo &outFileInfo,
    bool sha2Enable)
{
    outFileInfo.clear();

    auto fileStatus = std::filesystem::status(filePath);
    if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
        return FileState::Missing;

    std::string _loaderType;
    std::string _loaderVersion;
    FileInfo _fileInfo;
    
    std::tie(_loaderType, _loaderVersion, _fileInfo) = loadJSON(jsonStream, id);

    if (loaderType.compare(_loaderType) != 0 ||
        loaderVersion.compare(_loaderVersion) != 0)
        return FileState::Stale;

    int64_t fileSize = std::filesystem::file_size(filePath);
    std::string fileTimeStamp = getFileTimeStamp(filePath);
    std::vector<unsigned char> fileSha2(picosha2::k_digest_size);
    if (sha2Enable)
    {
        mio::mmap_source file(filePath.string());
        picosha2::hash256(file.begin(), file.end(), fileSha2.begin(), fileSha2.end());
    }

    // perhaps add path check as well?
    if (fileSize != _fileInfo.size ||
        fileTimeStamp.compare(_fileInfo.timeStamp) != 0 ||
        sha2Enable ? fileSha2 != _fileInfo.sha2 : false)
        return FileState::Stale;

    outFileInfo.path = filePath;
    outFileInfo.size = fileSize;
    outFileInfo.timeStamp = std::move(fileTimeStamp);
    outFileInfo.sha2 = std::move(fileSha2);

    return FileState::Valid;
}

void loadBinaryFile(
    const std::filesystem::path &filePath,
    FileInfo& outFileInfo,
    std::function<void(std::istream&)> loadOp,
    bool sha2Enable)
{
    outFileInfo.clear();

    auto fileStatus = std::filesystem::status(filePath);
    if (!std::filesystem::exists(fileStatus) || !std::filesystem::is_regular_file(fileStatus))
        throw std::runtime_error("Failed to open file.");

    {
        mio::mmap_istreambuf fileStreamBuf(filePath.string());
        std::istream fileStream(&fileStreamBuf);

        loadOp(fileStream);
    
        if (sha2Enable)
        {
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

    outFileInfo.path = filePath;
    outFileInfo.size = std::filesystem::file_size(filePath);
    outFileInfo.timeStamp = getFileTimeStamp(filePath);
}

void saveBinaryFile(
    const std::filesystem::path &filePath,
    FileInfo& outFileInfo,
    std::function<void(std::iostream&)> saveOp,
    bool sha2Enable)
{
    {
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

    outFileInfo.path = filePath;
    outFileInfo.size = std::filesystem::file_size(filePath);
    outFileInfo.timeStamp = getFileTimeStamp(filePath);
}

void loadCachedSourceFile(
    const std::filesystem::path &sourceFilePath,
    const std::filesystem::path &cacheFilePath,
    const std::string& loaderType,
	const std::string& loaderVersion,
    LoadFileFn loadSourceFileFn,
    LoadFileFn loadBinaryCacheFn,
    SaveFileFn saveBinaryCacheFn)
{
    std::filesystem::path jsonFilePath(sourceFilePath);
    jsonFilePath += ".json";

    std::filesystem::path pbinFilePath(cacheFilePath);
    pbinFilePath += ".pbin";

    bool doImport;
    FileInfo sourceFileInfo, pbinFileInfo;
    FileState sourceFileState, pbinFileState;
    auto jsonFileStatus = std::filesystem::status(jsonFilePath);
    auto pbinFileStatus = std::filesystem::status(pbinFilePath);

    if (std::filesystem::exists(jsonFileStatus) && std::filesystem::is_regular_file(jsonFileStatus) &&
        !std::filesystem::exists(pbinFileStatus))
    {
        std::filesystem::remove(jsonFilePath);

        doImport = true;
    }
    else if (std::filesystem::exists(jsonFileStatus) && std::filesystem::is_regular_file(jsonFileStatus))
    {
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
        
        sourceFileState = getFileInfo(
            sourceFilePath,
            "sourceFileInfo",
            loaderType,
            loaderVersion,
            fileStream,
            loadJSONFn,
            sourceFileInfo,
            false);

        fileStream.clear();
        fileStream.seekg(0, std::ios_base::beg);

        pbinFileState = getFileInfo(
            pbinFilePath,
            "pbinFileInfo",
            loaderType,
            loaderVersion,
            fileStream,
            loadJSONFn,
            pbinFileInfo,
            false);
    }
    else
    {
        doImport = true;
    }

    if (doImport ||
        sourceFileState == FileState::Stale ||
        pbinFileState != FileState::Valid)
    {	
        mio::mmap_ostreambuf streamBuf(jsonFilePath.string());
        std::ostream stream(&streamBuf);
        cereal::JSONOutputArchive json(stream);
    
        json(cereal::make_nvp("loaderType", loaderType));
        json(cereal::make_nvp("loaderVersion", loaderVersion));

        loadBinaryFile(sourceFilePath, sourceFileInfo, loadSourceFileFn, true);
        json(CEREAL_NVP(sourceFileInfo));

        saveBinaryFile(pbinFilePath, pbinFileInfo, saveBinaryCacheFn, true);
        json(CEREAL_NVP(pbinFileInfo));
    }
    else
    {
        loadBinaryFile(pbinFilePath, pbinFileInfo, loadBinaryCacheFn, false);
    }
}
