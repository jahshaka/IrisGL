#ifndef PROPERTYTYPE_H
#define PROPERTYTYPE_H

#include <QVariant>
#include <QColor>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

namespace iris
{

enum class PropertyType
{
    None,
    Bool,
    Int,
    Float,
    Vec2,
	Vec3,
	Vec4,
    Color,
    Texture,
    File,
    List
};

struct Property
{
    /// The panel's widget index (PropertyWidget stores it on every row it
    /// builds). Only the MATERIAL property lists ever assigned it —
    /// PbrMaterial::createProperties numbers its rows — so a scene node's
    /// properties handed to the same widget carried an indeterminate index.
    /// Zero-initialised: an unnumbered row is row 0, not garbage.
    unsigned            id = 0;
    QString             displayName;
    QString             name;
    QString             uniform;
    PropertyType        type = PropertyType::None;

    /// Reflected, but not writable through setPropertyValue: the field behind
    /// it needs an operation the reflection layer cannot perform (reloading a
    /// mesh, resolving an asset guid to bytes). Declared HERE, at the one place
    /// that is always in step with the row itself, because the alternative is a
    /// second list of names in the scripting layer that silently rots — and a
    /// surface that tells an agent a row is writable when it is not is exactly
    /// the silent-success defect class AI_SURFACE_AUDIT catalogued.
    bool                readOnly = false;

    virtual QVariant    getValue() = 0;
    virtual void        setValue(QVariant val) = 0;

    virtual ~Property() = default;
};

class PropertyListener
{
public:
    virtual void onPropertyChanged(Property*) = 0;
    virtual void onPropertyChangeStart(Property*) = 0;
    virtual void onPropertyChangeEnd(Property*) = 0;

    virtual ~PropertyListener() = default;
};

struct BoolProperty : public Property
{
    bool value;

    BoolProperty () {
        type = PropertyType::Bool;
        value = false;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.toBool();
    }
};

// min/max are ZERO-INITIALISED, and that is load-bearing. The ctors used to set
// only `type`, and no getProperties() in irisgl/document/scenegraph/ has ever
// assigned minValue/maxValue — so every scene-node int/float row carried two
// indeterminate numbers. Nothing read them while the only consumer was the
// material panel (PbrMaterial DOES set real ranges), but a verb that reports
// them would hand a model uninitialised memory as fact
// (AI_SURFACE_PROGRAM_SPEC §3.A). The convention that follows from zeroing them:
// a row has a declared range only when max > min, and readers must emit min/max
// on exactly that condition.
struct IntProperty : public Property
{
    int value = 0;
    int minValue = 0;
    int maxValue = 0;

    IntProperty() {
        type = PropertyType::Int;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.toInt();
    }
};

struct FloatProperty : public Property
{
    float value = 0.0f;
    float minValue = 0.0f;
    float maxValue = 0.0f;

    FloatProperty() {
        type = PropertyType::Float;
    }

    QVariant getValue() {
        return QVariant::fromValue(value);
    }

    void setValue(QVariant val) {
        value = val.toFloat();
    }
};

struct ColorProperty : public Property
{
    QColor value;

    ColorProperty () {
        type = PropertyType::Color;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.value<QColor>();
    }
};

struct TextureProperty : public Property
{
    QString value;
    QString toggleValue;
    bool toggle;

    TextureProperty () {
        type = PropertyType::Texture;
        toggle = false;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.toString();
        toggle = !value.isEmpty();
    }
};

struct FileProperty : public Property
{
    QString value;
    QString suffix;

    FileProperty () {
        type = PropertyType::File;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.toString();
    }
};

struct ListProperty : public Property
{
    QStringList value;
    int index = 0;      // same uninitialised-member class as min/max above

    ListProperty () {
        type = PropertyType::List;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.toStringList();
    }
};

/* more abstract types without a physical widget */

struct Vec2Property : public Property
{
    QVector2D value;

    Vec2Property() {
        type = PropertyType::Vec2;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.value<QVector2D>();
    }
};

struct Vec3Property : public Property
{
    QVector3D value;

    Vec3Property() {
        type = PropertyType::Vec3;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.value<QVector3D>();
    }
};

struct Vec4Property : public Property
{
	QVector4D value;

	Vec4Property() {
		type = PropertyType::Vec4;
	}

	QVariant getValue() {
		return value;
	}

	void setValue(QVariant val) {
        value = val.value<QVector3D>().toVector4D();
	}
};

}

#endif // PROPERTYTYPE_H
