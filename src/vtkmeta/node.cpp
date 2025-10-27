#include "node.h"
#include "mesh.h"
#include "material.h"
#include <vtkProperty.h>
#include <vtkTransform.h>

namespace vtkmeta {

Node::Node()
    : visible_(true)
{
    actor_ = vtkSmartPointer<vtkActor>::New();
    transform_ = vtkSmartPointer<vtkTransform>::New();
}

Node::~Node() = default;

void Node::setMesh(std::shared_ptr<Mesh> mesh) {
    mesh_ = mesh;

    if (mesh_) {
        actor_->SetMapper(mesh_->getVTKMapper());
    }
}

std::shared_ptr<Mesh> Node::getMesh() const {
    return mesh_;
}

void Node::setMaterial(std::shared_ptr<Material> material) {
    material_ = material;

    if (material_) {
        actor_->SetProperty(material_->getProperty());

        if (auto tex = material_->getTexture()) {
            actor_->SetTexture(tex);
        }
    }
}

std::shared_ptr<Material> Node::getMaterial() const {
    return material_;
}

void Node::setVisible(bool visible) {
    visible_ = visible;
    actor_->SetVisibility(visible_ ? 1 : 0);
}

bool Node::isVisible() const {
    return visible_;
}

void Node::setTransform(vtkSmartPointer<vtkTransform> transform) {
    if (transform) {
        actor_->SetUserMatrix(transform->GetMatrix());
    }
}

vtkSmartPointer<vtkTransform> Node::getTransform() const {
    auto matrix = actor_->GetUserMatrix();
    if (!matrix) {
        return nullptr;
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);

    return transform;
}

vtkSmartPointer<vtkActor> Node::getVTKActor() const {
    return actor_;
}

void Node::setPosition(double x, double y, double z)
{
    transform_->Identity();
    transform_->Translate(x, y, z);
    actor_->SetUserTransform(transform_);
}

void Node::setRotation(double rx, double ry, double rz)
{
    transform_->Identity();
    transform_->RotateX(rx);
    transform_->RotateY(ry);
    transform_->RotateZ(rz);
    actor_->SetUserTransform(transform_);
}

void Node::setScale(double sx, double sy, double sz)
{
    transform_->Identity();
    transform_->Scale(sx, sy, sz);
    actor_->SetUserTransform(transform_);
}

} // namespace vtkmeta
