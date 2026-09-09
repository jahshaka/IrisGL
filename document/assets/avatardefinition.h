/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_AVATARDEFINITION_H
#define IRIS_AVATARDEFINITION_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "document/animation/locomotion.h"
#include "document/physics/avatarmovement.h"

namespace iris
{

// THE AVATAR DEFINITION (AVATAR_ASSET_SPEC §3.1, format v1).
//
// An avatar ASSET's `source` file is this document: a small JSON that names a
// rigged model Object and the clips that play on it. It is what the Avatar
// module edits, what `avatar.spawn` instantiates, and — because it is an
// ordinary content-addressed file on an ordinary library row — what the asset
// pipeline pins per project, copies on write, archives and garbage-collects
// with no pipeline changes at all (§4 D1).
//
// It lives in irisgl/document (not in src/services) so the module, the
// services, the scene reader/writer and the tests can all link it without
// linking Studio: pure data + JSON, no Qt widgets, no engine, no database.
//
// EVERY KEY IS WRITTEN, always — including the null slots. A file that says
// what it means is worth the bytes (the locomotion writer's rule); a reader
// tolerates absence so an older file still loads.
//
// The `unitScale`, `locomotion`, `movement`, `sockets` and `blendshapes`
// slots are SLOTS: the format is fixed now so the features that fill them
// (AVATAR_MODULE R0.7's import scale, LOCOMOTION Stage 7's graph tab,
// Stage 8's morph targets) are additive rather than a format break.

/// One clip the avatar can play. `asset` is whatever the ONE import pipeline
/// produced for the file — today a Mixamo download lands as an Object, and
/// Section A's `ModelTypes::Animation` can arrive later without touching this
/// format, because a clip is referenced by GUID and never by type.
struct AvatarClipEntry
{
    QString asset;        ///< the clip asset's guid (the model's own guid for in-file clips)
    QString rawName;      ///< the clip's name INSIDE the file ("mixamo.com" and friends)
    QString name;         ///< the display name the scene plays it under
    bool looping = true;
    bool rootMotion = false;
};

/// The rig this definition is authored against — the identity half of
/// AVATAR_RIG_PERF §3.1, so skeleton sharing and the avatar library can never
/// disagree about what "the same rig" is.
struct AvatarRigInfo
{
    QString rigId;             ///< rigsignature.h's hash of the sorted bone names
    int bones = 0;
    QStringList boneNames;
    /// SLOT (§7 R4): no per-asset import unit scale exists in the tree yet, so
    /// nothing applies this. `hasUnitScale == false` means "the file's own
    /// units", which is what every avatar does today.
    bool hasUnitScale = false;
    double unitScale = 1.0;
};

struct AvatarDefinition
{
    static constexpr int kFormatVersion = 1;

    QString name;
    QString modelAsset;                 ///< the rigged Object's guid
    AvatarRigInfo rig;
    QVector<AvatarClipEntry> clips;
    QString defaultClip;                ///< `defaults.activeClip`; empty = none

    /// SLOT: null = "generate the default asset at spawn, as today" (which is
    /// what keeps every shipped locomotion suite green). Non-null = the
    /// authored controller, applied to every instance.
    bool hasLocomotion = false;
    LocomotionAsset locomotion;
    ClipRoles locomotionRoles;

    /// SLOT: null = the component's own defaults.
    bool hasMovement = false;
    AvatarMovementParams movement;

    /// SLOT: an authored socket table (the built-ins are identity transforms
    /// installed at spawn). Kept as raw JSON until the socket authoring UI
    /// exists, so a file written by a later build round-trips through this one
    /// instead of losing its sockets.
    QJsonObject sockets;
    bool hasSockets = false;

    /// SLOT: LOCOMOTION Stage 8 / morph targets. Raw, same reason.
    QJsonObject blendshapes;
    bool hasBlendshapes = false;

    /// The clip entry with this display name, or nullptr.
    const AvatarClipEntry *findClip(const QString &displayName) const;
    /// Every distinct asset guid this definition depends on (model + clips) —
    /// the dependency rows `AvatarAssets::save` reconciles to.
    QStringList dependencyGuids() const;
    bool isValid() const { return !modelAsset.isEmpty(); }
};

/// Serialization. `toJson` always writes every key (nulls included).
QJsonObject avatarDefinitionToJson(const AvatarDefinition &def);

/// Parses AND validates: on refusal `out` is untouched and `*error` says what
/// is wrong in the file's own vocabulary. A definition with no model is a
/// refusal, not an empty avatar — nothing downstream can do anything with one.
bool avatarDefinitionFromJson(const QJsonObject &obj, AvatarDefinition &out, QString *error);

}   // namespace iris

#endif   // IRIS_AVATARDEFINITION_H
