#include "fxaapostprocess.h"
#include "../core/property.h"

namespace iris
{

FxaaPostProcess::FxaaPostProcess()
{
    name = "fxaa";
    displayName = "Fxaa Post Processing";
    quality = 1;
}

QList<Property *> FxaaPostProcess::getProperties()
{
    auto props = QList<Property*>();

    auto prop = new IntProperty();
    prop->displayName = "Quality";
    prop->name = "quality";
    prop->value = quality;
    props.append(prop);

    return props;
}

void FxaaPostProcess::setProperty(Property *prop)
{
    if(prop->name == "quality")
        this->setQuality(prop->getValue().toInt());
}

void FxaaPostProcess::setQuality(int quality)
{
    this->quality = quality;
}

int FxaaPostProcess::getQuality()
{
    return quality;
}

FxaaPostProcessPtr FxaaPostProcess::create()
{
    return FxaaPostProcessPtr(new FxaaPostProcess());
}

}
