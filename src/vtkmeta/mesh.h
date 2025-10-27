#ifndef MESH_H
#define MESH_H

#include <vtkSmartPointer.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyData.h>

namespace vtkmeta {

class Mesh {
public:
    Mesh();
    ~Mesh();

    void setPolyData(vtkSmartPointer<vtkPolyData> polyData);
    vtkSmartPointer<vtkPolyData> getPolyData() const;

    vtkSmartPointer<vtkPolyDataMapper> getVTKMapper() const { return mapper_; }

private:
    vtkSmartPointer<vtkPolyData> poly_data_;
    vtkSmartPointer<vtkPolyDataMapper> mapper_;
};

using MeshPtr = std::shared_ptr<Mesh>;

}

#endif // MESH_H
