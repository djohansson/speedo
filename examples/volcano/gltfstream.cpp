#include "gltfstream.h"

#include <sstream>

#include <GLTFSDK/GLTFResourceReader.h>
#include <GLTFSDK/GLBResourceReader.h>
#include <GLTFSDK/Deserialize.h>

#include <mio/mmap_iostream.hpp>

namespace gltfstream
{

// Uses the Document class to print some basic information about various top-level glTF entities
void PrintDocumentInfo(const Microsoft::glTF::Document& document)
{
    using namespace Microsoft::glTF;

    // Asset Info
    std::cout << "Asset Version:    " << document.asset.version << "\n";
    std::cout << "Asset MinVersion: " << document.asset.minVersion << "\n";
    std::cout << "Asset Generator:  " << document.asset.generator << "\n";
    std::cout << "Asset Copyright:  " << document.asset.copyright << "\n\n";

    // Scene Info
    std::cout << "Scene Count: " << document.scenes.Size() << "\n";

    if (document.scenes.Size() > 0U)
    {
        std::cout << "Default Scene Index: " << document.GetDefaultScene().id << "\n\n";
    }
    else
    {
        std::cout << "\n";
    }

    // Entity Info
    std::cout << "Node Count:     " << document.nodes.Size() << "\n";
    std::cout << "Camera Count:   " << document.cameras.Size() << "\n";
    std::cout << "Material Count: " << document.materials.Size() << "\n\n";

    // Mesh Info
    std::cout << "Mesh Count: " << document.meshes.Size() << "\n";
    std::cout << "Skin Count: " << document.skins.Size() << "\n\n";

    // Texture Info
    std::cout << "Image Count:   " << document.images.Size() << "\n";
    std::cout << "Texture Count: " << document.textures.Size() << "\n";
    std::cout << "Sampler Count: " << document.samplers.Size() << "\n\n";

    // Buffer Info
    std::cout << "Buffer Count:     " << document.buffers.Size() << "\n";
    std::cout << "BufferView Count: " << document.bufferViews.Size() << "\n";
    std::cout << "Accessor Count:   " << document.accessors.Size() << "\n\n";

    // Animation Info
    std::cout << "Animation Count: " << document.animations.Size() << "\n\n";

    for (const auto& extension : document.extensionsUsed)
    {
        std::cout << "Extension Used: " << extension << "\n";
    }

    if (!document.extensionsUsed.empty())
    {
        std::cout << "\n";
    }

    for (const auto& extension : document.extensionsRequired)
    {
        std::cout << "Extension Required: " << extension << "\n";
    }

    if (!document.extensionsRequired.empty())
    {
        std::cout << "\n";
    }
}

// Uses the Document and GLTFResourceReader classes to print information about various glTF binary resources
void PrintResourceInfo(const Microsoft::glTF::Document& document, const Microsoft::glTF::GLTFResourceReader& resourceReader)
{
    using namespace Microsoft::glTF;

    // Use the resource reader to get each mesh primitive's position data
    for (const auto& mesh : document.meshes.Elements())
    {
        std::cout << "Mesh: " << mesh.id << "\n";

        for (const auto& meshPrimitive : mesh.primitives)
        {
            std::string accessorId;

            if (meshPrimitive.TryGetAttributeAccessorId(ACCESSOR_POSITION, accessorId))
            {
                const Accessor& accessor = document.accessors.Get(accessorId);

                const auto data = resourceReader.ReadBinaryData<float>(document, accessor);
                const auto dataByteLength = data.size() * sizeof(float);

                std::cout << "MeshPrimitive: " << dataByteLength << " bytes of position data\n";
            }
        }

        std::cout << "\n";
    }

    // Use the resource reader to get each image's data
    for (const auto& image : document.images.Elements())
    {
        std::string filename;

        std::cout << "Image: " << image.id << "\n";
        std::cout << "Image: " << image.mimeType << "\n";

        if (image.uri.empty())
        {
            assert(!image.bufferViewId.empty());

            auto& bufferView = document.bufferViews.Get(image.bufferViewId);
            auto& buffer = document.buffers.Get(bufferView.bufferId);

            filename += buffer.uri; //NOTE: buffer uri is empty if image is stored in GLB binary chunk
        }
        else if (IsUriBase64(image.uri))
        {
            filename = "Data URI";
        }
        else
        {
            filename = image.uri;
        }

        if (!filename.empty())
        {
            std::cout << "Image filename: " << filename << "\n\n";
        }

        auto data = resourceReader.ReadBinaryData(document, image);
        
        std::cout << "Image: " << data.size() << " bytes of image data\n";
    }
}

void PrintInfo(const std::filesystem::path& path)
{
    using namespace Microsoft::glTF;

    // Pass the absolute path, without the filename, to the stream reader
    auto streamReader = std::make_unique<GlTFStreamReader>(path.parent_path());

    std::filesystem::path pathFile = path.filename();
    std::filesystem::path pathFileExt = pathFile.extension();

    std::string manifest;

    auto MakePathExt = [](const std::string& ext)
    {
        return "." + ext;
    };

    std::unique_ptr<GLTFResourceReader> resourceReader;

    // If the file has a '.gltf' extension then create a GLTFResourceReader
    if (pathFileExt == MakePathExt(GLTF_EXTENSION))
    {
        auto gltfStream = streamReader->GetInputStream(pathFile.string()); // Pass a UTF-8 encoded filename to GetInputString
        auto gltfResourceReader = std::make_unique<GLTFResourceReader>(std::move(streamReader));

        std::stringstream manifestStream;

        // Read the contents of the glTF file into a string using a std::stringstream
        manifestStream << gltfStream->rdbuf();
        manifest = manifestStream.str();

        resourceReader = std::move(gltfResourceReader);
    }

    // If the file has a '.glb' extension then create a GLBResourceReader. This class derives
    // from GLTFResourceReader and adds support for reading manifests from a GLB container's
    // JSON chunk and resource data from the binary chunk.
    if (pathFileExt == MakePathExt(GLB_EXTENSION))
    {
        auto glbStream = streamReader->GetInputStream(pathFile.string()); // Pass a UTF-8 encoded filename to GetInputString
        auto glbResourceReader = std::make_unique<GLBResourceReader>(std::move(streamReader), std::move(glbStream));

        manifest = glbResourceReader->GetJson(); // Get the manifest from the JSON chunk

        resourceReader = std::move(glbResourceReader);
    }

    if (!resourceReader)
    {
        throw std::runtime_error("Command line argument path filename extension must be .gltf or .glb");
    }

    Document document;

    try
    {
        document = Deserialize(manifest);
    }
    catch (const GLTFException& ex)
    {
        std::stringstream ss;

        ss << "Microsoft::glTF::Deserialize failed: ";
        ss << ex.what();

        throw std::runtime_error(ss.str());
    }

    std::cout << "### glTF Info - " << pathFile << " ###\n\n";

    PrintDocumentInfo(document);
    PrintResourceInfo(document, *resourceReader);

    std::cout << std::endl;
}

}

GlTFStreamReader::GlTFStreamReader(std::filesystem::path pathBase)
 : myPathBase(std::move(pathBase))
{
    assert(myPathBase.has_root_path());
}

std::shared_ptr<std::istream> GlTFStreamReader::GetInputStream(const std::string& filename) const
{
    auto streamPath = myPathBase / std::filesystem::path(filename);
    auto stream = std::make_shared<mio::mmap_istream>(streamPath.string());

    if (!stream || !(*stream))
        throw std::runtime_error("Unable to create a valid input stream for uri: " + filename);
    
    return stream;
}
