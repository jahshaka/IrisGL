#ifndef NODE_H
#define NODE_H

#include <vtkSmartPointer.h>
#include <vtkActor.h>
#include <vtkAssembly.h>
#include <memory>

namespace vtkmeta {

class Mesh;
class Material;

class Node {
public:
    Node();
    ~Node();

    void setName(const std::string& n);
    const std::string& name() const;

    void setMesh(std::shared_ptr<Mesh> mesh);
    std::shared_ptr<Mesh> getMesh() const;

    void setMaterial(std::shared_ptr<Material> material);
    std::shared_ptr<Material> getMaterial() const;

    void setVisible(bool visible);
    bool isVisible() const;

    void setTransform(vtkSmartPointer<vtkTransform> transform);
    vtkSmartPointer<vtkTransform> getTransform() const;

    vtkSmartPointer<vtkActor> getVTKActor() const;

    void setPosition(double x,double y,double z);
    void setRotation(double rx,double ry,double rz);
    void setScale(double sx,double sy,double sz);

private:
    std::string name_;
    std::shared_ptr<Mesh> mesh_;
    std::shared_ptr<Material> material_;
    vtkSmartPointer<vtkActor> actor_;
    vtkSmartPointer<vtkTransform> transform_;
    bool visible_;
};

using NodePtr = std::shared_ptr<Node>;

} // namespace vtkmeta

#endif // NODE_H
