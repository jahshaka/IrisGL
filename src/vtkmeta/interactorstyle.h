#ifndef INTERACTORSTYLE_H
#define INTERACTORSTYLE_H

#include <vtkSmartPointer.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkRenderWindowInteractor.h>

namespace vtkmeta {

class InteractorStyle {
public:
    InteractorStyle();
    ~InteractorStyle();

    void setInteractor(vtkRenderWindowInteractor* interactor);
    vtkSmartPointer<vtkInteractorStyleTrackballCamera> getVTKStyle() const;

private:
    vtkSmartPointer<vtkInteractorStyleTrackballCamera> style_;
};

}


#endif // INTERACTORSTYLE_H
