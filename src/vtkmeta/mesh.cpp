#include "mesh.h"

namespace vtkmeta {

Mesh::Mesh() {
    mapper_ = vtkSmartPointer<vtkPolyDataMapper>::New();
}

Mesh::~Mesh() {}

void Mesh::setPolyData(vtkSmartPointer<vtkPolyData> polyData) {
    poly_data_ = polyData;
    mapper_->SetInputData(poly_data_);
}

vtkSmartPointer<vtkPolyData> Mesh::getPolyData() const {
    return poly_data_;
}

}

