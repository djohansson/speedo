// #pragma once

// #include <filesystem>

// #include <GLTFSDK/Document.h>
// #include <GLTFSDK/IStreamReader.h>

// class GlTFStreamReader : public Microsoft::glTF::IStreamReader
// {
// public:
// 	GlTFStreamReader(std::filesystem::path&& pathBase);
// 	std::shared_ptr<std::istream> GetInputStream(const std::string& filename) const override;

// private:
// 	std::filesystem::path myPathBase;
// };

// namespace gltfstream
// {

// void PrintInfo(const std::filesystem::path& path);

// } // namespace gltfstream
