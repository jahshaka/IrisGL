#include "renderer.h"

#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include "scene.h"
#include "node.h"

#include <QDebug>

namespace vtkmeta {

Renderer::Renderer()
{
    renderer_=vtkSmartPointer<vtkRenderer>::New();
}
Renderer::~Renderer(){}

vtkSmartPointer<vtkRenderer> Renderer::getVTKRenderer()
{
    return renderer_;
}

void Renderer::setScene(std::shared_ptr<Scene> scene)
{
    scene_ = scene;
    if (!scene_) return;

    // renderer_->RemoveAllViewProps();

    // for (auto &n: scene_->getNodes())
    // {
    //     if (n->getVTKActor()) {
    //         renderer_->AddActor(n->getVTKActor());
    //     }
    // }
}

void Renderer::renderFrame(vtkSmartPointer<vtkRenderWindow> window)
{
    if (scene_) {
        window->AddRenderer(scene_->getRenderer());
        window->Render();
    }
}

void Renderer::resetCamera()
{
    renderer_->ResetCamera();
}

void Renderer::renderOnce(vtkRenderWindow* rw)
{
    if (rw) {
        rw->Render();
        return;
    }

    renderer_->GetRenderWindow()->Render();
}

void Renderer::update(float dt){ Q_UNUSED(dt); }

}
