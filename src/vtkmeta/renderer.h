#ifndef RENDERER_H
#define RENDERER_H

#include <memory>
#include <vtkSmartPointer.h>

class vtkRenderer;
class vtkRenderWindow;

namespace vtkmeta {
class Scene;

class Renderer {
public:
    Renderer();
    ~Renderer();

    vtkSmartPointer<vtkRenderer> getVTKRenderer();
    void setScene(std::shared_ptr<Scene> scene);
    void renderFrame(vtkSmartPointer<vtkRenderWindow> window);
    void resetCamera();
    void renderOnce(vtkRenderWindow* rw);
    void update(float dt);

private:
    vtkSmartPointer<vtkRenderer> renderer_;
    std::shared_ptr<Scene> scene_;
};

using RendererPtr = std::shared_ptr<Renderer>;

} // namespace vtkmeta

#endif // RENDERER_H
