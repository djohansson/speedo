#include <rhi/rhiapplication.h>

namespace rhi
{

std::mutex RHIApplication::gDrawMutex{};
core::LoadQueue RHIApplication::gLoads{};
bool RHIApplication::gShowAbout = false;
bool RHIApplication::gShowDemoWindow = false;

} // namespace rhi
