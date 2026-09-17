#ifndef SHADOWMAP_H
#define SHADOWMAP_H

namespace iris
{

enum class ShadowMapType : int
{
    None = 0,
    Hard = 1,
    Soft = 2,
    VerySoft = 3
};

// Document-side shadow settings per light. The GL depth texture and shadow
// matrix died with the legacy renderer; the engine reads type and resolution.
//
// (`bias` is GONE — render audit I-6, CRUD law. It was serialized as
// `shadowBias`, reflected as a property and written by the glTF exporter, and
// the renderer never read it: the depth bias that makes shadows work on this
// engine is a macroblock constant on the shadow node, and on Vulkan it does
// not even apply to line primitives. One number nobody could use.)
class ShadowMap
{
public:
    ShadowMapType shadowType;
    int resolution;

    ShadowMap();

    void setResolution(int size);
};


}

#endif // SHADOWMAP_H
