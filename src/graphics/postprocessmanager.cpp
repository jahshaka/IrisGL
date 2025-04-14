#include <QOpenGLContext>
#include <QOpenGLFunctions_3_2_Core>
#include <QSharedPointer>

#include "postprocessmanager.h"
#include "rendertarget.h"
#include "utils/fullscreenquad.h"
#include "texture2d.h"
#include "postprocess.h"

#include "../postprocesses/coloroverlaypostprocess.h"
#include "../postprocesses/radialblurpostprocess.h"
#include "../postprocesses/bloompostprocess.h"
#include "../postprocesses/greyscalepostprocess.h"
#include "../postprocesses/ssaopostprocess.h"
#include "../postprocesses/fxaapostprocess.h"

namespace iris
{

PostProcessManager::PostProcessManager(GraphicsDevicePtr device)
{
    this->device = device;
    this->gl = new QOpenGLFunctions_3_2_Core();
    this->gl->initializeOpenGLFunctions();
    rtInitialized = false;
    fsQuad = new FullScreenQuad();

    //postProcesses.append(new ColorOverlayPostProcess());
    //postProcesses.append(new RadialBlurPostProcess());
    //postProcesses.append(QSharedPointer<PostProcess>(new BloomPostProcess()));
    //postProcesses.append(new GreyscalePostProcess());
    //postProcesses.append(new SSAOPostProcess());
    //postProcesses.append(FxaaPostProcess::create());
    postProcesses.append(FxaaPostProcess::create(device));
}

PostProcessManagerPtr PostProcessManager::create(GraphicsDevicePtr device)
{
    return PostProcessManagerPtr(new PostProcessManager(device));
}

void PostProcessManager::addPostProcess(PostProcessPtr process)
{
    postProcesses.append(process);
}

void PostProcessManager::setPostProcesses(QList<PostProcessPtr> processes)
{
    this->postProcesses = processes;
}

QList<PostProcessPtr> PostProcessManager::getPostProcesses()
{
    return postProcesses;
}

void PostProcessManager::clearPostProcesses()
{
    postProcesses.clear();
}

void PostProcessManager::blit(iris::Texture2DPtr source, iris::Texture2DPtr dest, iris::ShaderPtr shader)
{
    initRenderTarget();

    renderTarget->clearTextures();
    renderTarget->addTexture(dest);

    renderTarget->bind();

    gl->glViewport(0, 0, dest->texture->width(), dest->texture->height());
    gl->glClearColor(0, 0, 0, 0);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!!source)
        source->bind(0);

    if (!!shader)
        fsQuad->draw(device, shader);
    else
        fsQuad->draw(device);

    renderTarget->unbind();
    renderTarget->clearTextures();
}

void PostProcessManager::process(PostProcessContext *context)
{
    context->manager = this;

    blit(context->sceneTexture, context->finalTexture);

    for (auto process : postProcesses) {
        process->process(context);
    }
}

void PostProcessManager::initRenderTarget()
{
    if (!rtInitialized)
    {
        // the size shouldnt matter
        renderTarget = RenderTarget::create(100, 100);
        renderTarget->clearRenderBuffer();

        rtInitialized = true;
    }
}

}
