#ifndef PROPERTYTYPE_H
#define PROPERTYTYPE_H

// THE DOCUMENT's PROPERTY ROWS (NOQT-DOCUMENT-1 stage 4: std types, no Qt). A row's value is carried as an iris::Value
// in the shapes of document/properties/valueshapes.h; its setter is STRICT: it takes its own kind only (a Float row
// an Int too: one JSON number type) and otherwise returns false with NOTHING changed. The editor converts a Qt value
// to the row's kind at its own edge (src/app/qtstdgui.h, qtstd::toRowValue), never here.

#include "core/math/vec.h"
#include "core/value.h"
#include "document/properties/valueshapes.h"
#include "document/srgbcolor.h"

#include <memory>
#include <string>
#include <vector>

namespace iris
{

enum class PropertyType
{
    None,
    Bool,
    Int,
    Float,
    Vec3,
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
    std::string         displayName;
    std::string         name;
    std::string         uniform;
    PropertyType        type = PropertyType::None;

    /// Reflected, but not writable through setPropertyValue: the field behind
    /// it needs an operation the reflection layer cannot perform (reloading a
    /// mesh, resolving an asset guid to bytes). Declared HERE, at the one place
    /// that is always in step with the row itself, because the alternative is a
    /// second list of names in the scripting layer that silently rots — and a
    /// surface that tells an agent a row is writable when it is not is exactly
    /// the silent-success defect class AI_SURFACE_AUDIT catalogued.
    bool                readOnly = false;

    /// The row's value in its Value shape (valueshapes.h).
    virtual Value       getValue() const = 0;
    /// The row takes `value` when its kind is the row's own; false, nothing changed, otherwise.
    virtual bool        setValue(const Value &value) = 0;

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
        mView.push_back(raw);
        return raw;
    }
    const std::vector<Property *> &view() const { return mView; }

private:
    std::vector<std::unique_ptr<Property>> mOwned;
    std::vector<Property *> mView;   // mOwned's pointers, same order
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

    BoolProperty() {
        type = PropertyType::Bool;
        value = false;
    }

    Value getValue() const override {
        return Value(value);
    }

    bool setValue(const Value &v) override {
        if (v.kind() != Kind::Bool) return false;
        value = v.asBool();
        return true;
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

    Value getValue() const override {
        return Value(value);
    }

    /// An Int only (an integral Number is refused: the edge converts by the row's kind).
    bool setValue(const Value &v) override {
        if (v.kind() != Kind::Int) return false;
        value = static_cast<int>(v.asInt());
        return true;
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

    /// The float widened to a Number (the bits a QVariant's toFloat() handed QJsonValue).
    Value getValue() const override {
        return Value(double(value));
    }

    /// A Number, or an Int (one JSON number type).
    bool setValue(const Value &v) override {
        double d = 0.0;
        if (!numberOf(v, d)) return false;
        value = float(d);
        return true;
    }
};

struct ColorProperty : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<ColorProperty>(*this); }

    SrgbColor value;

    ColorProperty() {
        type = PropertyType::Color;
    }

    Value getValue() const override {
        return colourToValue(value);
    }

    /// The colour's Object shape (valueshapes.h); an empty object reads as the invalid colour.
    bool setValue(const Value &v) override {
        if (v.kind() != Kind::Object) return false;
        value = colourFromValue(v);
        return true;
    }
};

struct TextureProperty : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<TextureProperty>(*this); }

    /// The file this session renders from (a store object's path, or a
    /// "live://" reference) — what the panel shows and material.get reports.
    std::string value;
    /// THE ROW'S IDENTITY (TEX-REF-1): the library asset `value` is, kept from
    /// the moment the row is bound until it is written. A saved scene persists
    /// THIS, never a guid derived back from the path; a reference the store
    /// could not resolve keeps its guid with an empty `value`
    /// (iris::Material::textureRef says how a row is bound).
    std::string assetGuid;
    std::string toggleValue;
    bool toggle = false;

    TextureProperty() {
        type = PropertyType::Texture;
    }

    Value getValue() const override {
        return Value(value);
    }

    /// A {path, guid} Object binds both (a member absent reads ""); a bare path
    /// String binds a file nobody named, so the identity goes — unless it is the
    /// path already held. Any other kind, or a member that is not a String: false.
    bool setValue(const Value &v) override {
        if (v.kind() == Kind::Object) {
            const Value *path = v.find("path");
            const Value *guid = v.find("guid");
            if ((path && path->kind() != Kind::String) || (guid && guid->kind() != Kind::String)) return false;
            value = path ? path->asString() : std::string();
            assetGuid = guid ? guid->asString() : std::string();
        } else if (v.kind() == Kind::String) {
            if (v.asString() != value) assetGuid.clear();
            value = v.asString();
        } else {
            return false;
        }
        toggle = !value.empty();
        return true;
    }
};

struct FileProperty : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<FileProperty>(*this); }

    std::string value;
    std::string suffix;

    FileProperty() {
        type = PropertyType::File;
    }

    Value getValue() const override {
        return Value(value);
    }

    bool setValue(const Value &v) override {
        if (v.kind() != Kind::String) return false;
        value = v.asString();
        return true;
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

    int value = 0;                    ///< the selected enum value == index into `labels`
    std::vector<std::string> labels;  ///< one label per value, in value order

    ListProperty() {
        type = PropertyType::List;
    }

    Value getValue() const override {
        return Value(value);
    }

    bool setValue(const Value &v) override {
        if (v.kind() != Kind::Int) return false;
        value = static_cast<int>(v.asInt());
        return true;
    }
};

struct Vec3Property : public Property
{
    std::unique_ptr<Property> clone() const override { return std::make_unique<Vec3Property>(*this); }

    Vec3 value;

    Vec3Property() {
        type = PropertyType::Vec3;
    }

    Value getValue() const override {
        return vec3ToValue(value);
    }

    /// An Array of three numbers.
    bool setValue(const Value &v) override {
        return vec3FromValue(v, value);
    }
};

}

#endif // PROPERTYTYPE_H
