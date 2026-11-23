#ifndef PROPERTYTYPE_H
#define PROPERTYTYPE_H

#include <QVariant>
#include <QColor>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <QList>

#include <memory>
#include <vector>

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

    /// A DEEP COPY of the row, of its own type — every field, the value, the
    /// range, the labels, a texture row's asset identity. What an owner that
    /// copies itself (iris::Material, LEAK-1) clones its rows with, so two
    /// owners never hold the same row.
    virtual std::unique_ptr<Property> clone() const = 0;

    virtual ~Property() = default;
};

/// A LIST OF ROWS THAT OWNS THEM (LEAK-1). Every editor-facing row list is one:
/// a material's (its copy IS the deep copy, each row cloned — the member-wise
/// copy a material's duplicate() starts from, so two materials never hold the
/// same row) and the fresh list a scene node reflects itself into
/// (SceneNode::getProperties, which used to hand back bare `new`'d rows that
/// nearly every caller leaked). `view()` is the raw, ordered view readers walk;
/// a pointer in it lives exactly as long as the list. Assignment by copy is
/// deleted; a list moves.
class PropertyRows
{
public:
    PropertyRows() = default;
    PropertyRows(const PropertyRows &other)
    {
        mOwned.reserve(other.mOwned.size());
        mView.reserve(other.mView.size());
        for (const auto &row : other.mOwned) add(row->clone());
    }
    PropertyRows(PropertyRows &&) noexcept = default;
    PropertyRows &operator=(PropertyRows &&) noexcept = default;
    PropertyRows &operator=(const PropertyRows &) = delete;

    /// Takes `row` and appends it; returns the row (now owned here).
    Property *add(std::unique_ptr<Property> row)
    {
        Property *raw = row.get();
        mOwned.push_back(std::move(row));
        mView.append(raw);
        return raw;
    }
    const QList<Property *> &view() const { return mView; }

private:
    std::vector<std::unique_ptr<Property>> mOwned;
    QList<Property *> mView;   // mOwned's pointers, same order
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
    std::unique_ptr<Property> clone() const override { return std::make_unique<BoolProperty>(*this); }

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
    std::unique_ptr<Property> clone() const override { return std::make_unique<IntProperty>(*this); }

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
    std::unique_ptr<Property> clone() const override { return std::make_unique<FloatProperty>(*this); }

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
    std::unique_ptr<Property> clone() const override { return std::make_unique<ColorProperty>(*this); }

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
    std::unique_ptr<Property> clone() const override { return std::make_unique<TextureProperty>(*this); }

    /// The file this session renders from (a store object's path, or a
    /// "live://" reference) — what the panel shows and material.get reports.
    QString value;
    /// THE ROW'S IDENTITY (TEX-REF-1): the library asset `value` is, kept from
    /// the moment the row is bound until it is written. A saved scene persists
    /// THIS, never a guid derived back from the path; a reference the store
    /// could not resolve keeps its guid with an empty `value`
    /// (iris::Material::textureRef says how a row is bound).
    QString assetGuid;
    QString toggleValue;
    bool toggle = false;

    TextureProperty () {
        type = PropertyType::Texture;
        toggle = false;
    }

    QVariant getValue() {
        return value;
    }

    /// A {path, guid} map binds both; a bare path binds a file nobody named,
    /// so the identity goes — unless it is the path already held.
    void setValue(QVariant val) {
        if (val.typeId() == QMetaType::QVariantMap) {
            const QVariantMap ref = val.toMap();
            value = ref.value(QStringLiteral("path")).toString();
            assetGuid = ref.value(QStringLiteral("guid")).toString();
        } else {
            const QString path = val.toString();
            if (path != value) assetGuid.clear();
            value = path;
        }
        toggle = !value.isEmpty();
    }
};

struct FileProperty : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<FileProperty>(*this); }

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

/// THE GENERIC ENUM ROW (HLMS_ADOPTION P1, spec correction C3).
///
/// It used to be a QStringList with an unused `index`, had ZERO producers in
/// the whole tree, and PropertyWidget's `case List:` was empty — so an enum was
/// only renderable by hardcoding its name in the panel. Exactly one row ever
/// did (alphaMode), and the comment there records what that costs: a material
/// set to a mode with no label showed a BLANK combo and any pick silently
/// downgraded it (PUBLISH_AUDIT #4).
///
/// Now: `value` is the STORED ENUM VALUE and it is an int, so an enum row
/// serializes, reads back and reports exactly like an IntProperty — no new
/// on-disk shape, no new scripting type coercion. `labels` is the vocabulary,
/// index == value, and it travels WITH the row so the picker cannot disagree
/// with it. A row whose labels do not cover its whole value range is the same
/// defect as before; the labels being the only source of the range is what
/// makes that unrepresentable.
struct ListProperty : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<ListProperty>(*this); }

    int value = 0;          ///< the selected enum value == index into `labels`
    QStringList labels;     ///< one label per value, in value order

    ListProperty () {
        type = PropertyType::List;
    }

    QVariant getValue() {
        return value;
    }

    void setValue(QVariant val) {
        value = val.toInt();
    }
};

/* more abstract types without a physical widget */

struct Vec2Property : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<Vec2Property>(*this); }

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
    std::unique_ptr<Property> clone() const override { return std::make_unique<Vec3Property>(*this); }

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
    std::unique_ptr<Property> clone() const override { return std::make_unique<Vec4Property>(*this); }

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
