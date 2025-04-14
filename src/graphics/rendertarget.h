#ifndef RENDERTARGET_H
#define RENDERTARGET_H

#include "../irisglfwd.h"
#include "qopengl.h"
#include <QVector>

class QOpenGLFunctions_3_2_Core;
namespace iris
{

struct RenderTargetTexture
{
    TexturePtr texture;
    int cubeFace = 0;
	bool isCubeMap = false;

	RenderTargetTexture()
	{

	}

    RenderTargetTexture(TexturePtr texture, int cubeFace = 0, bool isCubeMap = false):
        texture(texture), cubeFace(cubeFace), isCubeMap(isCubeMap)
    {

    }

};

//todo: clear fbo bindings, textures are still left bound
// to fbo even tho they're removed from the texture list
class RenderTarget
{
    GLuint fboId;

    // depth render buffer
    GLuint renderBufferId;

    QOpenGLFunctions_3_2_Core* gl;
    int width;
    int height;

    //QList<Texture2DPtr> textures;
    QVector<RenderTargetTexture> textures;
    Texture2DPtr depthTexture;

    RenderTarget(int width, int height);
    void checkStatus();
public:

    // resized renderbuffer
    // if resizeTextures is true, all attached textures will be resized
    void resize(int width, int height, bool resizeTextures);

    void addTexture(TextureCubePtr tex, int index = 0);
    void addTexture(Texture2DPtr tex);
    void setDepthTexture(Texture2DPtr depthTex);

    void clearTextures();
    void clearDepthTexture();
    void clearRenderBuffer();

    void bind();
    void unbind();

    static RenderTargetPtr create(int width, int height);
    int getWidth() const;
    int getHeight() const;

    QImage toImage();
};

}

#endif // RENDERTARGET_H
