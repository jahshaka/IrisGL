#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLShaderProgram>
#include <QColor>

#include "fxaapostprocess.h"
#include "../graphics/postprocessmanager.h"
#include "../graphics/postprocess.h"
#include "../graphics/graphicshelper.h"
#include "../graphics/graphicsdevice.h"
#include "../graphics/texture2d.h"
#include "../graphics/shader.h"
#include "../core/property.h"


namespace iris
{

FxaaPostProcess::FxaaPostProcess(iris::GraphicsDevicePtr graphics)
{
	this->graphics = graphics;
    name = "fxaa";
    displayName = "Fxaa Post Processing";

    tonemapShader = iris::Shader::load(":assets/shaders/postprocesses/default.vs",
                                        ":assets/shaders/postprocesses/tonemapping.fs");

    fxaaShader = iris::Shader::load(":assets/shaders/postprocesses/default.vs",
                                        ":assets/shaders/postprocesses/aa.fs");

    tonemapTex = Texture2D::create(100, 100);
    fxaaTex = Texture2D::create(100, 100);

    quality = 1;
}

QList<Property *> FxaaPostProcess::getProperties()
{
    auto props = QList<Property*>();

    auto prop = new IntProperty();
    prop->displayName = "Quality";
    prop->name = "quality";
    prop->value = quality;
    props.append(prop);

    return props;
}

void FxaaPostProcess::setProperty(Property *prop)
{
    if(prop->name == "quality")
        this->setQuality(prop->getValue().toInt());
}

void FxaaPostProcess::setQuality(int quality)
{
    this->quality = quality;
}

int FxaaPostProcess::getQuality()
{
    return quality;
}

void FxaaPostProcess::process(PostProcessContext *ctx)
{
    auto screenWidth = ctx->sceneTexture->texture->width();
    auto screenHeight = ctx->sceneTexture->texture->height();
    tonemapTex->resize(screenWidth, screenHeight);
    fxaaTex->resize(screenWidth, screenHeight);

	graphics->setShader(tonemapShader);
	graphics->setShaderUniform("u_screenTex", 0);
    ctx->manager->blit(ctx->sceneTexture, tonemapTex, tonemapShader);
	graphics->setShader(iris::ShaderPtr());

    tonemapTex->texture->generateMipMaps();


	graphics->setShader(fxaaShader);
	graphics->setShaderUniform("u_screenTex", 0);
    graphics->setShaderUniform("u_screenSize", QVector2D(1.0f/screenWidth, 1.0/screenHeight));
    ctx->manager->blit(tonemapTex, fxaaTex, fxaaShader);
	graphics->setShader(iris::ShaderPtr());

    ctx->manager->blit(fxaaTex, ctx->finalTexture);
}

FxaaPostProcessPtr FxaaPostProcess::create(iris::GraphicsDevicePtr graphics)
{
    return FxaaPostProcessPtr(new FxaaPostProcess(graphics));
}



}
