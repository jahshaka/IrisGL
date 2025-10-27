#ifndef MATERIAL_H
#define MATERIAL_H

#include <vtkSmartPointer.h>
#include <vtkProperty.h>
#include <vtkTexture.h>
#include <QString>

namespace vtkmeta {

class Material {
public:
    Material();
    ~Material();

    void setColor(double r,double g,double b);
    void setTexture(const QString& path);
    vtkSmartPointer<vtkProperty> getProperty() const;
    vtkSmartPointer<vtkTexture> getTexture() const;

private:
    vtkSmartPointer<vtkProperty> property_;
    vtkSmartPointer<vtkTexture> texture_;
};

using MaterialPtr = std::shared_ptr<Material>;

}

#endif // MATERIAL_H
