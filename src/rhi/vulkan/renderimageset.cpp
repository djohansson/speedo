#include <rhi/renderimageset.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>

namespace rhi
{

template <>
void RenderImageSet<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	SuperType::End(cmd);

	for (uint32_t imageIt = 0ul; imageIt < GetImageCount(); imageIt++)
	{
		auto& image = *myImages[imageIt];
		image.InternalSetImageLayout(vk::FromVk(this->GetAttachmentDescs()[imageIt].finalLayout));
	}
}

IMPLEMENT_OBJECT_GETINSTANCE(RenderImageSet<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(RenderImageSet<kVk>);

} // namespace rhi
