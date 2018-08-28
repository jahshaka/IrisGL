#ifndef CONTENTMANAGER_H
#define CONTENTMANAGER_H

#include "../irisglfwd.h"


namespace iris
{

class ModelLoader;

// this class is in charge of loading and caching all assets
class ContentManager
{
    GraphicsDevicePtr graphics;

	ModelLoader* modelLoader;

    ContentManager(GraphicsDevicePtr graphics);
public:
    MeshPtr loadMesh(QString meshPath);
    Texture2DPtr loadTexture(QString texturePath, bool flipY = false);
    FontPtr loadDefaultFont(int size = 15);
    FontPtr loadFont(QString fontPath, int size = 15);
    ShaderPtr loadShader(QString vertexShaderPath, QString fragmentShaderPath);
	ModelPtr loadModel(QString modelPath);

    static ContentManagerPtr create(GraphicsDevicePtr graphics);
};


}

#endif // CONTENTMANAGER_H
