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

/// The RESOLVED geometric transform one parse applies — the form the choke
/// point (readSceneFile) consumes. Default-constructed = identity: no scale,
/// no rotation, no translation, the file's own unit declaration honoured.
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

    bool hasRotation() const { return !rotation.isIdentity(); }
    bool hasTranslation() const
    {
        return translation.x() != 0.0f || translation.y() != 0.0f || translation.z() != 0.0f;
    }
    bool overridesUnit() const { return unitOverride > 0.0; }
    bool isIdentity() const
    {
        return scale == 1.0 && !overridesUnit() && !hasRotation() && !hasTranslation();
    }

    /// The number assimp's GLOBAL_SCALE_FACTOR gets, given what the file
    /// declares. `declared` <= 0 reads as 1 (a format that declares nothing).
    double globalScaleFactor(double declared) const;
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
};

}   // namespace iris

#endif   // IRIS_IMPORTSETTINGS_H
