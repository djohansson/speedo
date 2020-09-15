// render-d3d11.h
#pragma once

#include <slang.h>

namespace gfx {
class Renderer;
namespace dx11 {
SLANG_API Renderer* createD3D11Renderer();
} // dx11
} // gfx
