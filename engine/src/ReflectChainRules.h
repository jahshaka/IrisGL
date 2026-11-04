#pragma once
// THE REFLECTION CHAIN'S RULES AS PURE FUNCTIONS (PHOTON-7-9A; no Ogre, no device): what a chain's reflection reads, the
// one image the stop judges the displayed pixel from, and the stereo shape a recording must find. ChainDesc
// (OgreChain.cpp), OgreView::setStereo and RayQueryTier::recordReflect read them; tests/engine test_chain_reflect.cpp
// holds them (engine.chain_reflect_sources, engine.reflect_chain_shape).

namespace jahshaka::engine::detail {

/// THE SCREEN SOURCES. THE HYBRID IS THE ONE SHAPE (PHOTON-7-9A's amendment, the owner's ruling): a MONO chain with the
/// SSR row on reads the screen first - this view's previous picture, where it vouches for the hit - then the rays (a ray
/// chain), or marches the screen alone (a chain without rays, until NO-RAY-CUT-1). A STEREO chain reads no screen: its
/// sources are not per layer - VR's one explicit difference here (VR on par: no other).
constexpr bool reflectScreenSources(int ssr, bool stereo) { return ssr > 0 && !stereo; }

/// THE RASTER MARCH: the screen sources on a chain that traces no reflection rays.
constexpr bool reflectRasterMarch(int ssr, bool stereo, bool rayReflect)
{
    return reflectScreenSources(ssr, stereo) && !rayReflect;
}

/// The chain's images the two reads below name (OgreChain.cpp's chain::kRt0 / kRefractOut / kDistorted / kAoApplied).
enum class ReflectChainImage { Rt0, RefractOut, Distorted, AoApplied };

/// WHAT THE SCREEN TRACE READS: the last LINEAR stage before the camera-side passes - the refractives' copy where
/// refraction runs (a refractive surface is a real surface), else the opaque and blended composite; never the
/// distortion's warp nor the AO apply.
constexpr ReflectChainImage reflectScreenTraceSource(bool refractions)
{
    return refractions ? ReflectChainImage::RefractOut : ReflectChainImage::Rt0;
}

/// THE COMPOSED PICTURE (the stop's displayed pixel, PHOTON-7-9A stage 2): `build`'s sceneResult, in its order -
/// refraction, then distortion, then the AO apply, each over the one before; the last that runs is the picture.
constexpr ReflectChainImage reflectComposedImage(bool ssao, bool distortion, bool refractions)
{
    return ssao ? ReflectChainImage::AoApplied
                : distortion ? ReflectChainImage::Distorted
                             : refractions ? ReflectChainImage::RefractOut : ReflectChainImage::Rt0;
}

/// THE STEREO SHAPE: a stereo view's target is two layers (one eye each, at one origin), a mono view's one -
/// OgreView::setStereo makes the target so and rebuilds the chain on it inside the call, before any frame records.
constexpr unsigned stereoTargetLayers(bool stereo) { return stereo ? 2u : 1u; }

/// ...so a recording whose chain images are not the view's shape is a DEFECT, refused and counted (recordReflect), never
/// a silent frame with no reflection.
constexpr bool reflectChainMatchesStereo(bool stereo, unsigned layers) { return layers == stereoTargetLayers(stereo); }

}   // namespace jahshaka::engine::detail
