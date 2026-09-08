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

    ShadowMap();

    void setResolution(int size);
};


}

#endif // SHADOWMAP_H
