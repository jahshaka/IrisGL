#include "material.h"

#include "material.h"
#include <vtkPNGReader.h>

namespace vtkmeta {

Material::Material() {
    property_ = vtkSmartPointer<vtkProperty>::New();
}

void Material::setColor(double r, double g, double b) {
    property_->SetColor(r, g, b);
}

void Material::setTexture(const QString &path) {
    if (path.isEmpty()) return;

    auto reader = vtkSmartPointer<vtkPNGReader>::New();
    reader->SetFileName(path.toStdString().c_str());
    reader->Update();

    texture_ = vtkSmartPointer<vtkTexture>::New();
    texture_->SetInputConnection(reader->GetOutputPort());
}

vtkSmartPointer<vtkProperty> Material::getProperty() const {
    return property_;
}

vtkSmartPointer<vtkTexture> Material::getTexture() const {
    return texture_;
}

}
