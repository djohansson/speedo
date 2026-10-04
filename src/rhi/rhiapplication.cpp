#include <rhi/rhiapplication.h>

namespace rhi
{

core::UpgradableSharedMutex RHIApplication::gDrawMutex{};
core::LoadQueue RHIApplication::gLoads{};
bool RHIApplication::gShowAbout = false;
bool RHIApplication::gShowDemoWindow = false;

} // namespace rhi
