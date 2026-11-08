# Third-party notices

Speedo's own code is proprietary (see LICENSE). The client includes, links or loads the third-party components below,
each under its own license. A package holds each component's license text in its `licenses` directory (vcpkg's copyright
file of each port, as `licenses/<port>.txt`), and the fonts' beside them in `resources/fonts`. Where a component offers a
choice of licenses, the one Speedo uses is named.

## Shared libraries shipped with the client

| Component | License | Notes |
| --- | --- | --- |
| Slang (shader-slang) | Apache-2.0 WITH LLVM-exception | the shader compiler, and its glslang module |
| glslang | BSD-3-Clause, MIT, Apache-2.0; GPL-3.0 with the Bison special exception (its generated parser) | inside slang's glslang module |
| SPIRV-Tools | Apache-2.0 | inside slang's glslang module |
| Vulkan-Loader | Apache-2.0 | |
| Mesa (KosmicKrisp) | MIT | macOS only: the Vulkan driver |
| GLFW | Zlib | |
| FreeType | FTL (of FTL or GPL-2.0-or-later) | the FTL asks for credit in the documentation: portions of this software are copyright © The FreeType Project (www.freetype.org) |
| libpng | libpng-2.0 | |
| zlib | Zlib | |
| bzip2 | bzip2-1.0.6 | |
| Brotli | MIT | |
| libwebp | BSD-3-Clause | |
| KTX-Software | Apache-2.0 | with its Basis Universal and astc-encoder, Apache-2.0; built without its ETC decoder (etcdec, under an Ericsson license) |
| Draco | Apache-2.0 | |
| meshoptimizer | MIT | |
| libdeflate | MIT | |
| LZ4 | BSD-2-Clause | |
| Zstandard | BSD-3-Clause (of BSD-3-Clause or GPL-2.0-only) | |
| miniz | MIT | |
| mimalloc | MIT | |
| xxHash | BSD-2-Clause | |
| cpptrace | MIT | |
| libdwarf | LGPL-2.1 | macOS and Linux, through cpptrace: a shared library, replaceable |
| Tracy (client) | BSD-3-Clause | |
| ZeroMQ (libzmq) | MPL-2.0 | a shared library, unmodified |
| curl | curl | |
| OpenSSL | Apache-2.0 | macOS and Linux (Windows' curl uses Schannel) |
| cargs | MIT | |

## Compiled into the client

| Component | License |
| --- | --- |
| Dear ImGui | MIT |
| GLM | MIT |
| cgltf | MIT |
| stb | MIT (of MIT or public domain) |
| tinyobjloader | MIT |
| mikktspace | Zlib |
| Vulkan Memory Allocator | MIT |
| Vulkan-Headers | Apache-2.0 (of Apache-2.0 or MIT) |
| Vulkan-Utility-Libraries | Apache-2.0 |
| cppzmq | MIT |
| PicoSHA2 | MIT |
| stduuid | MIT |
| zpp_bits | MIT |
| unordered_dense | MIT |
| concurrentqueue | BSD-2-Clause (of BSD-2-Clause or BSL-1.0) |
| function_ref (tl) | CC0-1.0 |
| bcdec | MIT (of MIT or Unlicense) |
| tinybvh | MIT |
| ppqsort | MIT |
| parallel-hashmap | Apache-2.0 |
| mio | MIT |
| SHA-Intrinsics | public domain |
| LLVM libc++ and libc++abi | Apache-2.0 WITH LLVM-exception |
| AMD FidelityFX Super Resolution 3.1 | MIT (ported into the shaders, `src/gfx/shaders/upscale.slang`, with its notice) |

## Fonts

| Font | License |
| --- | --- |
| Roboto Medium | Apache-2.0 (`resources/fonts/Roboto-LICENSE.txt`) |
| Noto Sans Symbols 2 | OFL-1.1 (`resources/fonts/NotoSansSymbols2-OFL.txt`) |
| Twemoji Mozilla | the code Apache-2.0, the art CC-BY-4.0 (`resources/fonts/Twemoji.Mozilla-LICENSE.md`): emoji graphics by Twitter, Inc. and other contributors, licensed under CC-BY 4.0 |
