#ifndef RENDERSTATES_H
#define RENDERSTATES_H

#include "document/materials/rasterizerstate.h"

namespace iris{

// Neutral blend description. The GL enum plumbing died with the legacy
// renderer; what the document needs is only which of the three modes a
// material asked for (.effect "blendMode", particle additive flag).
enum class BlendType
{
    Opaque,
    AlphaBlend,
    Additive
};

struct BlendState
{
    BlendType type = BlendType::Opaque;

    BlendState() {}
    BlendState(BlendType t) : type(t) {}

    static BlendState AlphaBlend;
    static BlendState Opaque;
    static BlendState Additive;

    static BlendState createAlphaBlend() { return BlendState(BlendType::AlphaBlend); }
    static BlendState createOpaque()     { return BlendState(BlendType::Opaque); }
    static BlendState createAdditive()   { return BlendState(BlendType::Additive); }
};

struct DepthState
{
    bool depthBufferEnabled;
    bool depthWriteEnabled;

    DepthState()
    {
        depthBufferEnabled = true;
        depthWriteEnabled = true;
    }

    DepthState(bool bufferEnabled, bool writeEnabled)
    {
        depthBufferEnabled = bufferEnabled;
        depthWriteEnabled = writeEnabled;
    }

    static DepthState Default;
    static DepthState None;
};

struct RenderStates
{
    int renderLayer;
    BlendState blendState;
    DepthState depthState;
    RasterizerState rasterState;

    bool fogEnabled;
    bool castShadows;
    bool receiveShadows;
    bool receiveLighting;

	RenderStates();
};

}

#endif // RENDERSTATES_H
