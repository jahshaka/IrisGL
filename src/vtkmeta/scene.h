#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include <memory>
#include <vtkSmartPointer.h>
#include <vtkLight.h>
#include "node.h"
#include "camera.h"

namespace vtkmeta {

class Scene {
public:
    Scene();
    ~Scene();

    void addNode(std::shared_ptr<Node> node);
    void addLight(vtkSmartPointer<vtkLight> light);
    void setCamera(vtkSmartPointer<vtkCamera> camera);

    void update(double dt);
    void resetCamera();

    vtkSmartPointer<vtkRenderer> getRenderer() const { return renderer_; }

private:
    std::vector<std::shared_ptr<Node>> nodes_;
    std::vector<vtkSmartPointer<vtkLight>> lights_;
    vtkSmartPointer<vtkRenderer> renderer_;
    vtkSmartPointer<vtkCamera> camera_;
};

} // namespace vtkmeta


#endif // SCENE_H
