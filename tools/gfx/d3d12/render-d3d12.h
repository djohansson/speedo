// render-d3d12.h
#pragma once

#include <slang.h>

namespace gfx {
class Renderer;
namespace dx12 {
SLANG_API Renderer* createD3D12Renderer();
} // dx12
} // gfx
