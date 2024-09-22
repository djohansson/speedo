#include "rhiapplication.h"

UpgradableSharedMutex RHIApplication::gDrawMutex{};
std::atomic_uint8_t RHIApplication::gProgress = 0;
std::atomic_bool RHIApplication::gShowProgress = false;
bool RHIApplication::gShowAbout = false;
bool RHIApplication::gShowDemoWindow = false;
