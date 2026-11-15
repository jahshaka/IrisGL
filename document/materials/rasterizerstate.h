#ifndef RASTERIZERSTATE_H
#define RASTERIZERSTATE_H

namespace iris
{

enum class CullMode
{
    None,
    CullClockwise,
    CullCounterClockwise
};

enum class FillMode
{
    Solid,
    Wireframe
};

struct RasterizerState
{
    CullMode cullMode;
    FillMode fillMode;

	float depthScaleBias;
	float depthBias;

    RasterizerState()
    {
        cullMode= CullMode::CullCounterClockwise;
        fillMode = FillMode::Solid;
		depthScaleBias = 0;
		depthBias = 0;
    }

    RasterizerState(CullMode cullMode, FillMode fillMode = FillMode::Solid):
        cullMode(cullMode),
        fillMode(fillMode)
    {
		depthScaleBias = 0;
		depthBias = 0;
    }

    static RasterizerState CullCounterClockwise;
    static RasterizerState CullClockwise;
    static RasterizerState CullNone;

	static RasterizerState createCullCounterClockwise();
	static RasterizerState createCullClockwise();
	static RasterizerState createCullNone();
};

}

#endif // RASTERIZERSTATE_H
