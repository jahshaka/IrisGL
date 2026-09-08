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
// matrix died with the legacy renderer; the engine reads type/resolution/bias.
class ShadowMap
{
public:
    ShadowMapType shadowType;
    int resolution;
    float bias;

    // STATIC SHADOW MAP (SPECS/SHADOW_TOOLING_SPEC.md §4.3). The light's map is
    // rendered ONCE and then kept until something invalidates it, instead of
    // being re-rendered every frame — which for a POINT light is six cube-face
    // passes plus a copy, every frame, for a lamp that never moves over
    // geometry that never moves.
    //
    // It is a SHADOW flag, not a memory-manager class: lights can never be
    // SCENE_STATIC (there is no static twin for a light), and this says nothing
    // about the node's transform being frozen — it says the renderer may reuse
    // the picture until told otherwise. The renderer dirties it when the light
    // moves, when any of its parameters change, when geometry moves or is
    // attached, and on world.refreshShadows().
    //
    // IGNORED for directional lights (their PSSM splits follow the camera, so
    // there is nothing static about them) and for area lights (which never
    // cast). Default OFF for new lights — owner decision D2.
    bool staticMap;

    ShadowMap();

    void setResolution(int size);
};


}

#endif // SHADOWMAP_H
