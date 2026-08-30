#include "rasterizerstate.h"

namespace iris
{

RasterizerState RasterizerState::CullCounterClockwise = RasterizerState(CullMode::CullCounterClockwise);
RasterizerState RasterizerState::CullClockwise = RasterizerState(CullMode::CullClockwise);
RasterizerState RasterizerState::CullNone = RasterizerState(CullMode::None);

RasterizerState RasterizerState::createCullCounterClockwise()
{
	return RasterizerState::CullCounterClockwise;
}

RasterizerState RasterizerState::createCullClockwise()
{
	return RasterizerState::CullClockwise;
}

RasterizerState RasterizerState::createCullNone()
{
	return RasterizerState::CullNone;
}

}
