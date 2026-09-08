#ifndef FXAAPOSTPROCESS_H
#define FXAAPOSTPROCESS_H

#include "document/materials/postprocess.h"

namespace iris
{

class FxaaPostProcess;
typedef QSharedPointer<FxaaPostProcess> FxaaPostProcessPtr;

// Anti-aliasing post-process settings (document half; the GL passes died with
// the legacy renderer).
class FxaaPostProcess : public PostProcess
{
public:
    // between 1 and 5
    int quality;

    FxaaPostProcess();

    QList<Property *> getProperties() override;
    void setProperty(Property *prop) override;

    void setQuality(int quality);
    int getQuality();

    static FxaaPostProcessPtr create();
};

}


#endif // FXAAPOSTPROCESS_H
