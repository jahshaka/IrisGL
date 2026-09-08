#include "document/scenegraph/shadowmap.h"

namespace iris
{

ShadowMap::ShadowMap()
{
    shadowType = ShadowMapType::Soft;
    resolution = 1024*2;
    bias = 0.01f;
    // Opt-in: a new light behaves exactly as every light did before static
    // shadow maps existed (owner decision D2).
    staticMap = false;
}

void ShadowMap::setResolution(int size)
{
    resolution = size;
}

}
