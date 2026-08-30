#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include "irisglfwd.h"
#include <QEnableSharedFromThis>

namespace iris
{

class Property;

// Document-side post-process settings: a name plus a property bag, serialized
// by SceneWriter. The GL processing passes died with the legacy renderer.
class PostProcess : public QEnableSharedFromThis<PostProcess>
{
public:
    bool enabled;
    QString name;
    QString displayName;

    QString getName()
    {
        return name;
    }

    QString getDisplayName()
    {
        return displayName;
    }

    virtual QList<Property*> getProperties()
    {
        return QList<Property*>();
    }

    virtual void setProperty(Property* prop)
    {

    }

    virtual ~PostProcess()
    {

    }

};

}

#endif // POSTPROCESS_H
