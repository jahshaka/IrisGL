/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORTSETTINGS_H
#define IRIS_IMPORTSETTINGS_H

// IMPORT SETTINGS — what the user decided about a model file, ONCE, at import
// (SPECS/IMPORT_DIALOG_SPEC.md §3).
//
// THE DECISION (owner + lead, MASTER_QUEUE §527/§528): an asset's scale,
// orientation and origin are decided at IMPORT and BAKED INTO THE ASSET, so
// every instance of it is placed at scale 1. The record below is what the
// dialog writes, what `assets.import(path, {...})` accepts, and what the asset
// carries forever in `properties.import.settings`. It is also HALF THE BAKE
// KEY: two imports of one source file with different settings are different
// geometry and must not share a bake (import/meshbake.h).
//
// MISSING OR `{}` IS IDENTITY. Every row imported before this existed, every
// shipped sample and every `.jaf` archive carries no `settings` object at all,
// and must key exactly as a default-settings import does — so an absent record
// and a fully-defaulted one produce the SAME hash, `identityHash()`.
//
// THE HASH is over a CANONICAL JSON rendering written by hand
// (canonicalJson()), not by QJsonDocument: keys in a fixed sorted order, every
// key present whether the caller wrote it or not, and numbers formatted by one
// explicit rule. A bake's file name carries this hash, so a Qt release that
// changed QJsonDocument's number formatting would otherwise rename — and
// silently invalidate — every bake in every library.
//
// UNITS. Jahshaka's world is metres. `units: "auto"` means "the file's own
// declaration is right" (FBX's UnitScaleFactor; every other format at this pin
// declares nothing and reads 1 metre per unit — import/importflags.h). Any
// other value OVERRIDES that declaration: `units: "cm"` on an FBX that wrongly
// says metres means "treat its numbers as centimetres". The effective uniform
// factor is therefore
//
//     k = scale x unitFactor(units) / declaredUnitScale        (an override)
//     k = scale                                                ("auto")
//
// and `k` is what reaches assimp's GLOBAL_SCALE_FACTOR property, which it
// composes with the file's own scale (thirdparty/assimp ScaleProcess.cpp:70-78).
// An override therefore needs the file's declaration BEFORE the parse — see
// ImportTransform::declaredUnitScale and import/scenesource.h.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "core/math/quat.h"
#include "core/math/vec.h"

namespace iris
{

/// THE RESOLVED EFFECT OF AN ASSET'S IMPORT SETTINGS ON ONE PARSE — the form
/// the choke point (readSceneFile) and the two builders that consume a parse
/// (MeshBake::buildFromScene, MeshNode::loadAsSceneFragment) take.
///
/// It has two halves, and they are applied in different places, which is why
/// they travel together: the GEOMETRY half (scale, rotation, origin) is handed
/// to assimp and to the parsed scene's root node at READ time, and the TUNING
/// half (§4.3 — skeleton, clips, materials) decides what is BUILT from that
/// parse. Both are keyed into the bake hash, so a change to either produces a
/// different bake rather than quietly replacing one.
///
/// Default-constructed = identity and everything built: no scale, no rotation,
/// no translation, the file's own unit declaration honoured, the skeleton, all
/// the clips and the materials imported. That is what a raw path with no
/// library row behind it gets, and what every row imported before the import
/// dialog carries.
/// Epic's own default card budget per mesh ("Max Lumen Mesh Cards"), and ours.
/// ONE definition, read by the settings record, by the transform and by every
/// caller of MeshBake::buildCards that has no record to read.
constexpr int kDefaultMaxCards = 12;
/// The format's and the generator's ceiling — ONE definition, read by the
/// settings record's validation, by MeshBake::buildCards's clamp and by the
/// .jmb reader that refuses a blob above it. It had three copies for a day.
constexpr int kMaxCardsCeiling = 64;

struct ImportTransform
{
    /// The user's uniform scale (> 0). Composed with the unit terms below.
    double scale = 1.0;

    /// Metres per source unit the USER declares, OVERRIDING the file.
    /// 0 = "auto": whatever the file says is right.
    double unitOverride = 0.0;

    /// What the file declares, when the caller already knows it (the asset's
    /// metadata block records it as `unitScale`, and the dialog's pre-read
    /// measures it). 0 = unknown: the choke point pays a light probe parse,
    /// and only when `unitOverride` is set.
    double declaredUnitScale = 0.0;

    /// The axes fix followed by the free rotation, as ONE rotation. Applied at
    /// the parsed scene's ROOT NODE, above both the mesh nodes and every bone,
    /// so bone offset matrices (mesh space -> bone space) are untouched and a
    /// skeleton rotates as a whole.
    Quat rotation;

    /// Metres, applied LAST (post scale, post rotation) at the same root node.
    Vec3 translation;

    // ---- the TUNING half (§4.3): what to BUILD from the parse -------------
    //
    // These act on OUR products, never on assimp's flag word: the bake's
    // producer key hashes ImportFlags::Canonical as a constant, so the parse
    // is byte-for-byte the same parse whatever these say.

    /// false: no skeleton is extracted and no bone index/weight vertex arrays
    /// are built — a rigged file bakes as STATIC geometry.
    bool skeleton = true;
    /// false: no animation clip is carried at all.
    bool clips = true;
    /// Non-empty: only these clips, matched case-insensitively against the
    /// names Mesh::extractAnimations produces (the names assets.metadata and
    /// the clip list show).
    QStringList clipNames;
    /// false: no material data is read from the file — the fragment gets the
    /// consumer's default material, and the bake records no material record.
    bool materials = true;

    /// SURFACE CARDS (SPECS/SURFACE_CACHE_ASSESSMENT.md §7 phase 1): how many
    /// axis-aligned capture cards the bake may author per mesh. 12 is Epic's
    /// own default ("Max Lumen Mesh Cards"); 0 means no cards at all. Clamped
    /// to 64 by the generator and by the bake format.
    ///
    /// It is a BUILD knob like `skeleton` and `clips` — it changes what is
    /// built out of a parse, never the parse — and it is in the bake key,
    /// because two imports of one file at different card budgets are two
    /// different bakes.
    int maxCards = kDefaultMaxCards;

    /// True when `name` survives the clip filter.
    bool wantsClip(const QString &name) const;

    bool hasRotation() const { return !rotation.isIdentity(); }
    bool hasTranslation() const
    {
        return translation.x() != 0.0f || translation.y() != 0.0f || translation.z() != 0.0f;
    }
    bool overridesUnit() const { return unitOverride > 0.0; }
    bool buildsEverything() const
    {
        return skeleton && clips && clipNames.isEmpty() && materials
               && maxCards == kDefaultMaxCards;
    }
    bool isIdentity() const
    {
        return scale == 1.0 && !overridesUnit() && !hasRotation() && !hasTranslation()
               && buildsEverything();
    }

    /// The number assimp's GLOBAL_SCALE_FACTOR gets, given what the file
    /// declares. `declared` <= 0 reads as 1 (a format that declares nothing).
    double globalScaleFactor(double declared) const;

    /// THE FORM A SEPARATE CLIP FILE IS READ WITH (§10's second half): the
    /// UNIFORM FACTOR only. A clip's position keys are in its own file's units
    /// and have to land on a rig that was baked under the CHARACTER's settings,
    /// so the rig's k applies — while its rotation and origin do NOT: those are
    /// a transform of the character's root node, and a clip has no geometry for
    /// them to move. Applying them would rotate a rig twice.
    ImportTransform keysOnly() const
    {
        ImportTransform out;
        out.scale = scale;
        out.unitOverride = unitOverride;
        out.declaredUnitScale = declaredUnitScale;
        return out;
    }
};

/// The import-settings RECORD: the JSON the asset carries, parsed.
class ImportSettings
{
public:
    /// The record's own version. Bumped only when a KEY's meaning changes;
    /// adding a key with an identity default does not move the identity hash
    /// only because the default is written into the canonical form — read the
    /// note on canonicalJson() before adding one.
    static constexpr int kVersion = 1;

    enum class Units { Auto, Metres, Centimetres, Millimetres, Inches, Feet };
    enum class MaterialMode { Import, None };

    /// Parses a record. An unknown key, a non-positive scale, an unknown unit
    /// or axis name is REFUSED: `errorOut` is set and the default (identity)
    /// settings come back, so a caller that ignores the error still gets a
    /// usable, honest object rather than a half-applied one.
    static ImportSettings fromJson(const QJsonObject &record, QString *errorOut = nullptr);

    /// The record as stored — every key present, so a reader never has to know
    /// this class's defaults.
    QJsonObject toJson() const;

    /// The bytes the hash is taken over (see the header note). Stable across
    /// Qt versions by construction.
    QByteArray canonicalJson() const;

    /// First 16 hex of the SHA256 of canonicalJson() — the `h` term of the
    /// bake key and of the bake's file name (import/meshbake.h).
    QString hash() const;

    /// The hash of default (identity) settings — what an absent or `{}`
    /// record keys as.
    static QString identityHash();

    /// Convenience: parse `record` and hash it. A record that fails to parse
    /// hashes as IDENTITY, because that is what the pipeline will actually
    /// apply to it.
    static QString hashOf(const QJsonObject &record);

    bool isIdentity() const;

    /// The resolved transform. `declaredUnitScale` is passed through for a
    /// caller that already knows the file's declaration (0 = let the choke
    /// point find out).
    ImportTransform transform(double declaredUnitScale = 0.0) const;

    /// Metres per source unit for an explicit unit; 0 for Units::Auto.
    static double unitFactor(Units units);
    static const char *unitName(Units units);
    static bool unitFromName(const QString &name, Units *out);

    /// Is `name` one of the six signed axis names (+X..-Z)?
    static bool axisFromName(const QString &name, Vec3 *out);

    /// True when this clip name survives the `clips` filter.
    bool wantsClip(const QString &name) const;

    // ---- the record ------------------------------------------------------
    double scale = 1.0;
    Units units = Units::Auto;
    QString up = QStringLiteral("+Y");
    QString forward = QStringLiteral("-Z");
    double rotate[3] = { 0.0, 0.0, 0.0 };      ///< degrees, XYZ, after the axes fix
    double translate[3] = { 0.0, 0.0, 0.0 };   ///< metres, last
    bool skeleton = true;
    bool clips = true;                          ///< false = import no clips at all
    QStringList clipNames;                      ///< non-empty = only these clips
    MaterialMode materials = MaterialMode::Import;
    int maxCards = kDefaultMaxCards;            ///< surface cards per mesh (0..64); see ImportTransform::maxCards
};

}   // namespace iris

#endif   // IRIS_IMPORTSETTINGS_H
