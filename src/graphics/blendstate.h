#ifndef BLENDSTATE_H
#define BLENDSTATE_H

#include <qopengl.h>

namespace iris
{
// nice hack to emulate strongly typed enum bit flags
struct ColorMask
{
	enum Enum
	{
		Red		= 1 << 0,
		Green	= 1 << 1,
		Blue	= 1 << 2,
		Alpha	= 1 << 3
	};
};
	

struct BlendState
{
    //bool blendEnabled;
    GLenum colorSourceBlend;
    GLenum alphaSourceBlend;
    GLenum colorDestBlend;
    GLenum alphaDestBlend;

    GLenum colorBlendEquation;
    GLenum alphaBlendEquation;

	int colorMask;

	BlendState()
	{
		colorSourceBlend = GL_ONE;
		alphaSourceBlend = GL_ONE;
		colorDestBlend = GL_ZERO;
		alphaDestBlend = GL_ZERO;

		colorBlendEquation = GL_FUNC_ADD;
		alphaBlendEquation = GL_FUNC_ADD;

		colorMask = ColorMask::Red | ColorMask::Green | ColorMask::Blue | ColorMask::Alpha;
    }

	void setColorMask(bool red, bool green, bool blue, bool alpha)
	{
		colorMask = (red ? ColorMask::Red : 0) |
					(green ? ColorMask::Green : 0) |
					(blue ? ColorMask::Blue : 0) |
					(alpha ? ColorMask::Alpha : 0);
	}

    BlendState(GLenum sourceBlend, GLenum destBlend):
        BlendState()
    {
        colorSourceBlend = sourceBlend;
        alphaSourceBlend = sourceBlend;
        colorDestBlend = destBlend;
        alphaDestBlend = destBlend;

		//colorMask = ColorMask::Red | ColorMask::Green | ColorMask::Blue | ColorMask::Alpha;
    }

	static BlendState createAlphaBlend();
	static BlendState createOpaque();
	static BlendState createAdditive();

    static BlendState AlphaBlend;
    static BlendState Opaque;
    static BlendState Additive;
};

}
#endif // BLENDSTATE_H
