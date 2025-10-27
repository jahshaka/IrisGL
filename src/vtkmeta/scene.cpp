#include "scene.h"

#include <algorithm>
#include <vtkCamera.h>
#include <vtkLight.h>
#include <vtkRenderer.h>

namespace vtkmeta {

Scene::Scene() {
    renderer_ = vtkSmartPointer<vtkRenderer>::New();
    camera_ = vtkSmartPointer<vtkCamera>::New();
    renderer_->SetActiveCamera(camera_);

    auto light = vtkSmartPointer<vtkLight>::New();
    light->SetLightTypeToSceneLight();
    light->SetPosition(0, 10, 10);
    light->SetFocalPoint(0, 0, 0);
    lights_.push_back(light);
}

Scene::~Scene() {}

void Scene::addNode(std::shared_ptr<Node> node) {
    if (!node) return;

    nodes_.push_back(node);

    if (node->getVTKActor()) {
        renderer_->AddActor(node->getVTKActor());
    }
}


void Scene::addLight(vtkSmartPointer<vtkLight> light) {
    if (!light) return;

    renderer_->AddLight(light);
}

void Scene::setCamera(vtkSmartPointer<vtkCamera> camera)
{
    camera_ = camera;
    renderer_->SetActiveCamera(camera);
}

void Scene::update(double dt)
{
//     for (auto& node : nodes_) {
// //        auto transform = vtkSmartPointer<vtkTransform>::New();
// //        transform->RotateY(20.0 * dt);  // 每秒旋转 20°
// //        node->setTransform(transform);
//     }
}

void Scene::resetCamera()
{
    renderer_->ResetCamera();
}

}
