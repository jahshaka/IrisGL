#ifndef CAMERA_H
#define CAMERA_H

#include <vtkSmartPointer.h>
#include <vtkCamera.h>

namespace vtkmeta {

class Camera {
public:
    Camera();
    ~Camera();

    vtkSmartPointer<vtkCamera> getVTKCamera() const;

    void setPosition(double x, double y, double z);
    void setFocalPoint(double x, double y, double z);
    void setViewUp(double x, double y, double z);

private:
    vtkSmartPointer<vtkCamera> camera_;
};

}
;

#endif // CAMERA_H
