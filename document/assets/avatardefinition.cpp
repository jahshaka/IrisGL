/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/avatardefinition.h"

#include <QJsonArray>
#include <QSet>

namespace iris
{

const AvatarClipEntry *AvatarDefinition::findClip(const QString &displayName) const
{
    for (const auto &clip : clips)
        if (clip.name == displayName) return &clip;
    return nullptr;
}

QStringList AvatarDefinition::dependencyGuids() const
{
    QStringList out;
    QSet<QString> seen;
    const auto add = [&](const QString &guid) {
        if (guid.isEmpty() || seen.contains(guid)) return;
        seen.insert(guid);
        out.append(guid);
    };
    add(modelAsset);
    for (const auto &clip : clips) add(clip.asset);
    return out;
}

QJsonObject avatarDefinitionToJson(const AvatarDefinition &def)
{
    QJsonObject obj;
    obj[QStringLiteral("format")] = QStringLiteral("jah-avatar");
    obj[QStringLiteral("version")] = AvatarDefinition::kFormatVersion;
    obj[QStringLiteral("name")] = def.name;

    QJsonObject model;
    model[QStringLiteral("asset")] = def.modelAsset;
    obj[QStringLiteral("model")] = model;

    QJsonObject rig;
    rig[QStringLiteral("rigId")] = def.rig.rigId;
    rig[QStringLiteral("bones")] = def.rig.bones;
    rig[QStringLiteral("boneNames")] = QJsonArray::fromStringList(def.rig.boneNames);
    if (def.rig.hasUnitScale) rig[QStringLiteral("unitScale")] = def.rig.unitScale;
    else rig[QStringLiteral("unitScale")] = QJsonValue();   // the SLOT, written as null
    obj[QStringLiteral("rig")] = rig;

    QJsonArray clips;
    for (const auto &clip : def.clips) {
        QJsonObject entry;
        entry[QStringLiteral("asset")] = clip.asset;
        entry[QStringLiteral("rawName")] = clip.rawName;
        entry[QStringLiteral("name")] = clip.name;
        entry[QStringLiteral("looping")] = clip.looping;
        entry[QStringLiteral("rootMotion")] = clip.rootMotion;
        clips.append(entry);
    }
    obj[QStringLiteral("clips")] = clips;

    QJsonObject defaults;
    defaults[QStringLiteral("activeClip")] = def.defaultClip;
    obj[QStringLiteral("defaults")] = defaults;

    if (def.hasLocomotion) {
        QJsonObject loco;
        QJsonObject roles;
        for (int i = 0; i < kClipRoleCount; ++i) {
            const auto role = ClipRole(i);
            if (def.locomotionRoles.has(role))
                roles[QLatin1String(clipRoleName(role))] = def.locomotionRoles.get(role);
        }
        loco[QStringLiteral("roles")] = roles;
        loco[QStringLiteral("asset")] = locomotionAssetToJson(def.locomotion);
        obj[QStringLiteral("locomotion")] = loco;
    } else {
        obj[QStringLiteral("locomotion")] = QJsonValue();
    }

    if (def.hasMovement) {
        const auto &p = def.movement;
        QJsonObject move;
        move[QStringLiteral("walkSpeed")] = p.walkSpeed;
        move[QStringLiteral("runSpeed")] = p.runSpeed;
        move[QStringLiteral("maxAcceleration")] = p.maxAcceleration;
        move[QStringLiteral("brakingDeceleration")] = p.brakingDeceleration;
        move[QStringLiteral("groundFriction")] = p.groundFriction;
        move[QStringLiteral("jumpVelocity")] = p.jumpVelocity;
        move[QStringLiteral("jumpCount")] = p.jumpCount;
        move[QStringLiteral("coyoteTime")] = p.coyoteTime;
        move[QStringLiteral("jumpReArm")] = p.jumpReArm;
        move[QStringLiteral("airControl")] = p.airControl;
        move[QStringLiteral("gravityScale")] = p.gravityScale;
        move[QStringLiteral("maxStepHeight")] = p.maxStepHeight;
        move[QStringLiteral("walkableFloorAngle")] = p.walkableFloorAngle;
        move[QStringLiteral("orientRotationToMovement")] = p.orientRotationToMovement;
        move[QStringLiteral("rotationRate")] = p.rotationRate;
        move[QStringLiteral("capsuleAuto")] = p.capsuleAuto;
        move[QStringLiteral("capsuleRadius")] = p.capsuleRadius;
        move[QStringLiteral("capsuleHeight")] = p.capsuleHeight;
        obj[QStringLiteral("movement")] = move;
    } else {
        obj[QStringLiteral("movement")] = QJsonValue();
    }

    obj[QStringLiteral("sockets")] = def.hasSockets ? QJsonValue(def.sockets) : QJsonValue();
    obj[QStringLiteral("blendshapes")] =
        def.hasBlendshapes ? QJsonValue(def.blendshapes) : QJsonValue();
    return obj;
}

bool avatarDefinitionFromJson(const QJsonObject &obj, AvatarDefinition &out, QString *error)
{
    const auto refuse = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };

    const QString format = obj.value(QStringLiteral("format")).toString();
    if (format != QStringLiteral("jah-avatar"))
        return refuse(QStringLiteral("not an avatar definition: format is '%1', expected "
                                     "'jah-avatar'").arg(format));
    const int version = obj.value(QStringLiteral("version")).toInt();
    // FORWARD refusal, not a silent partial read: a v2 file this build cannot
    // understand must say so rather than load as an avatar missing whatever v2
    // added.
    if (version <= 0 || version > AvatarDefinition::kFormatVersion)
        return refuse(QStringLiteral("avatar definition version %1 is not readable by this "
                                     "build (it understands up to %2)")
                          .arg(version).arg(AvatarDefinition::kFormatVersion));

    AvatarDefinition def;
    def.name = obj.value(QStringLiteral("name")).toString();
    def.modelAsset = obj.value(QStringLiteral("model")).toObject()
                         .value(QStringLiteral("asset")).toString();
    if (def.modelAsset.isEmpty())
        return refuse(QStringLiteral("the avatar definition names no model asset"));

    const QJsonObject rig = obj.value(QStringLiteral("rig")).toObject();
    def.rig.rigId = rig.value(QStringLiteral("rigId")).toString();
    def.rig.bones = rig.value(QStringLiteral("bones")).toInt();
    for (const auto &v : rig.value(QStringLiteral("boneNames")).toArray())
        def.rig.boneNames.append(v.toString());
    const QJsonValue unitScale = rig.value(QStringLiteral("unitScale"));
    if (unitScale.isDouble()) {
        def.rig.hasUnitScale = true;
        def.rig.unitScale = unitScale.toDouble();
    }

    QSet<QString> names;
    for (const auto &v : obj.value(QStringLiteral("clips")).toArray()) {
        const QJsonObject entry = v.toObject();
        AvatarClipEntry clip;
        clip.asset = entry.value(QStringLiteral("asset")).toString();
        clip.rawName = entry.value(QStringLiteral("rawName")).toString();
        clip.name = entry.value(QStringLiteral("name")).toString();
        clip.looping = entry.value(QStringLiteral("looping")).toBool(true);
        clip.rootMotion = entry.value(QStringLiteral("rootMotion")).toBool(false);
        if (clip.name.isEmpty())
            return refuse(QStringLiteral("a clip entry has no name"));
        if (clip.asset.isEmpty())
            return refuse(QStringLiteral("clip '%1' names no asset").arg(clip.name));
        // The display name is the KEY every consumer joins on (the scene plays
        // clips by name, roles bind by name) — two entries sharing one would
        // make "remove the walk" ambiguous forever.
        if (names.contains(clip.name))
            return refuse(QStringLiteral("two clips are both called '%1'").arg(clip.name));
        names.insert(clip.name);
        def.clips.append(clip);
    }

    def.defaultClip = obj.value(QStringLiteral("defaults")).toObject()
                          .value(QStringLiteral("activeClip")).toString();
    if (!def.defaultClip.isEmpty() && !names.contains(def.defaultClip))
        return refuse(QStringLiteral("the default clip '%1' is not one of this avatar's clips")
                          .arg(def.defaultClip));

    const QJsonValue loco = obj.value(QStringLiteral("locomotion"));
    if (loco.isObject()) {
        const QJsonObject locoObj = loco.toObject();
        QString locoError;
        if (locoObj.contains(QStringLiteral("asset"))
            && !locomotionAssetFromJson(locoObj.value(QStringLiteral("asset")).toObject(),
                                        def.locomotion, &locoError))
            return refuse(QStringLiteral("the locomotion asset is not readable: %1").arg(locoError));
        const QJsonObject roles = locoObj.value(QStringLiteral("roles")).toObject();
        for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
            ClipRole role;
            if (!clipRoleFromName(it.key(), role))
                return refuse(QStringLiteral("'%1' is not a clip role").arg(it.key()));
            def.locomotionRoles.set(role, it.value().toString());
        }
        def.hasLocomotion = true;
    }

    const QJsonValue move = obj.value(QStringLiteral("movement"));
    if (move.isObject()) {
        const QJsonObject m = move.toObject();
        AvatarMovementParams p;
        const auto f = [&m](const char *key, float fallback) {
            const QJsonValue v = m.value(QLatin1String(key));
            return v.isDouble() ? float(v.toDouble()) : fallback;
        };
        p.walkSpeed = f("walkSpeed", p.walkSpeed);
        p.runSpeed = f("runSpeed", p.runSpeed);
        p.maxAcceleration = f("maxAcceleration", p.maxAcceleration);
        p.brakingDeceleration = f("brakingDeceleration", p.brakingDeceleration);
        p.groundFriction = f("groundFriction", p.groundFriction);
        p.jumpVelocity = f("jumpVelocity", p.jumpVelocity);
        p.jumpCount = m.value(QStringLiteral("jumpCount")).toInt(p.jumpCount);
        p.coyoteTime = f("coyoteTime", p.coyoteTime);
        p.jumpReArm = f("jumpReArm", p.jumpReArm);
        p.airControl = f("airControl", p.airControl);
        p.gravityScale = f("gravityScale", p.gravityScale);
        p.maxStepHeight = f("maxStepHeight", p.maxStepHeight);
        p.walkableFloorAngle = f("walkableFloorAngle", p.walkableFloorAngle);
        p.orientRotationToMovement =
            m.value(QStringLiteral("orientRotationToMovement")).toBool(p.orientRotationToMovement);
        p.rotationRate = f("rotationRate", p.rotationRate);
        p.capsuleAuto = m.value(QStringLiteral("capsuleAuto")).toBool(p.capsuleAuto);
        p.capsuleRadius = f("capsuleRadius", p.capsuleRadius);
        p.capsuleHeight = f("capsuleHeight", p.capsuleHeight);
        def.movement = p;
        def.hasMovement = true;
    }

    const QJsonValue sockets = obj.value(QStringLiteral("sockets"));
    if (sockets.isObject()) { def.sockets = sockets.toObject(); def.hasSockets = true; }
    const QJsonValue blendshapes = obj.value(QStringLiteral("blendshapes"));
    if (blendshapes.isObject()) {
        def.blendshapes = blendshapes.toObject();
        def.hasBlendshapes = true;
    }

    out = def;
    return true;
}

}   // namespace iris
