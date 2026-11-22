// THE SHADER CACHE's FOUR LAYER KEYS AS ONE FUNCTION OF THEIR INPUTS (SHADER-SYSTEM-1 stage 5; the lead's rulings 1-3, the
// start preflight's F3 and F5). A layer's key names only its PRODUCER - the container's format and code, the compile
// path's content, glslang, the build type, the render system, and the GPU where the bytes are the driver's - and nothing
// else: no fork commit, no media tree, no build id, no tree path. The definition is OgreShaderCache.cpp's
// (composeLayerKeys); engine.shader_cache_keys holds each key moving on its terms, and the pipeline key's text
// byte-identical to the one it replaced (no kCacheFormat bump: a bump would wipe the driver-validated pipeline cache too).
#pragma once

#include <string>

namespace jahshaka::engine::detail {

struct LayerKeyInputs {
    int         format = 0;              ///< kCacheFormat
    std::string code;                    ///< JAHSHAKA_SHADER_CACHE_CODE_ID: the container's own bytes (ruling 3)
    std::string microcodePath;           ///< JAHSHAKA_COMPILE_PATH_MICROCODE_ID (ruling 1)
    std::string preprocessorPath;        ///< JAHSHAKA_COMPILE_PATH_PREPROCESSOR_ID (F3: the compute layer's; the Hlms
                                         ///< layer's guard is the template checksum's salt, ruling 2)
    std::string computePath;             ///< JAHSHAKA_COMPILE_PATH_COMPUTE_ID (F3)
    std::string glslang;                 ///< the SPIR-V compiler's version
    bool        debugBuild = false;
    std::string dbgstr;                  ///< OGRE_DEBUG_STR_SIZE ("0" in a Release-built Ogre)
    int         hashBits = 0;            ///< OGRE_HASH_BITS
    /// THE GPU TERMS, known once the render system has a device (ShaderCache::load); `haveGpu` false before.
    bool        haveGpu = false;
    std::string rs, vendor, device, driver;
};

struct LayerKeys {
    std::string pipeline, microcode, hlms, compute;   ///< the keys' TEXT (the manifest stores each one's hash)
};

LayerKeys composeLayerKeys(const LayerKeyInputs &in);

/// THE BUILD's TERMS, NO DEVICE (SHADER-SYSTEM-1 stage 6a): every input but the GPU's, exactly as this build sets them -
/// ShaderCache::configure() starts from it and shaderIdentity() (Engine.h) prints it: one definition, so the printed
/// identity IS the cache's own before its first view's load adds the GPU.
LayerKeyInputs buildLayerKeyInputs();
/// The four keys composed as the cache's fingerprint TEXT (ShaderCacheStats::fingerprint is its hex).
std::string fingerprintText(const LayerKeys &k);

}  // namespace jahshaka::engine::detail
