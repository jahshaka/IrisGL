#include "document/materials/renderstates.h"

namespace iris
{
	BlendState BlendState::AlphaBlend = BlendState(BlendType::AlphaBlend);
	BlendState BlendState::Opaque = BlendState(BlendType::Opaque);
	BlendState BlendState::Additive = BlendState(BlendType::Additive);

	DepthState DepthState::Default = DepthState(true, true);
	DepthState DepthState::None = DepthState(false, false);

	RenderStates::RenderStates()
	{
		renderLayer = 0;
		blendState = BlendState::Opaque;
		depthState = DepthState::Default;
		rasterState = RasterizerState::CullCounterClockwise;

		fogEnabled = true;
		castShadows = true;
		receiveShadows = true;
		receiveLighting = true;
	}
}
