#include <rhi/rhiapplication.h>

namespace rhi
{

core::UpgradableSharedMutex RHIApplication::gDrawMutex{};
std::atomic_uint8_t RHIApplication::gProgress = 0;
std::atomic_bool RHIApplication::gShowProgress = false;
core::ConcurrentAccess<std::string> RHIApplication::gProgressName{};
bool RHIApplication::gShowAbout = false;
bool RHIApplication::gShowDemoWindow = false;

} // namespace rhi
