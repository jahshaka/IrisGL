#include "document/scenegraph/shadowmap.h"

namespace iris
{

ShadowMap::ShadowMap()
{
    shadowType = ShadowMapType::Soft;
    resolution = 1024*2;
    bias = 0.01f;
}

void ShadowMap::setResolution(int size)
{
    resolution = size;
}

}
