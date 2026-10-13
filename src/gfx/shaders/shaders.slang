module speedo;

__include types;

// the main layout's shaders, in parts by area, included into one translation unit (so capi.h's declarations exist
// once): a part may use what any other declares

#include "capi.h"
#include "sky.h"

#include "bindings.slang"
#include "common.slang"
#include "packing.slang"
#include "vertex.slang"
#include "texturing.slang"
#include "ibl.slang"
#include "brdf.slang"
#include "shading.slang"
#include "shadows.slang"
#include "lights.slang"
#include "transmission.slang"
#include "scatter.slang"
#include "fragment.slang"
#include "tonemap.slang"
#include "composite.slang"
#include "uiblur.slang"
