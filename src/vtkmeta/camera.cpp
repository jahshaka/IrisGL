#include "camera.h"

namespace vtkmeta {

Camera::Camera() {
    camera_ = vtkSmartPointer<vtkCamera>::New();
}

Camera::~Camera() {}

vtkSmartPointer<vtkCamera> Camera::getVTKCamera() const {
    return camera_;
}

void Camera::setPosition(double x, double y, double z) {
    camera_->SetPosition(x, y, z);
}

void Camera::setFocalPoint(double x, double y, double z) {
    camera_->SetFocalPoint(x, y, z);
}

void Camera::setViewUp(double x, double y, double z) {
    camera_->SetViewUp(x, y, z);
}

}
