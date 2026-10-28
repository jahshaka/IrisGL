#ifndef SHADOWMAP_H
#define SHADOWMAP_H

namespace iris
{

// THE FILTER OF A LIGHT'S SHADOW - every light has one (PHOTON-7 10-4 R9; L9: a light is blocked by geometry on every
// path). `None` ("this light casts no shadow") is DELETED: a lamp the shadow maps do not hold is traced (a ray at the
// ray tier, the voxels' march where the GI is on) - so with GI off and no ray tracing, the lamps beyond the shadow maps
// are unshadowed (the scene issue `lamps.unshadowed` names them). The values keep their numbers; 0 names nothing (the
// reader takes Soft).
enum class ShadowMapType : int
{
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
