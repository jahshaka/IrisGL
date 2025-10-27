#include "interactorstyle.h"

namespace vtkmeta {

InteractorStyle::InteractorStyle() {
    style_ = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();
}

InteractorStyle::~InteractorStyle() {}

void InteractorStyle::setInteractor(vtkRenderWindowInteractor* interactor) {
    if (interactor) {
        interactor->SetInteractorStyle(style_);
    }
}

vtkSmartPointer<vtkInteractorStyleTrackballCamera> InteractorStyle::getVTKStyle() const {
    return style_;
}

}
