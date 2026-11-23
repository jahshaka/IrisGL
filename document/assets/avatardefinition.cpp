/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/avatardefinition.h"

#include "document/properties/valueshapes.h"

#include <QSet>

#include <algorithm>
#include <vector>

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

Value avatarDefinitionToJson(const AvatarDefinition &def)
{
    const auto S = [](const QString &q) { return Value(q.toStdString()); };
    Value::Array boneNames;
    for (const QString &b : def.rig.boneNames) boneNames.push_back(S(b));
    // the SLOT, written as null when there is no unit scale
    const Value unitScale = def.rig.hasUnitScale ? Value(def.rig.unitScale) : Value();

    Value::Array clips;
    for (const auto &clip : def.clips)
        clips.push_back(Value(Value::Object{{"asset", S(clip.asset)}, {"rawName", S(clip.rawName)},
                                            {"name", S(clip.name)}, {"looping", Value(clip.looping)},
                                            {"rootMotion", Value(clip.rootMotion)}}));

    Value locomotion;
    if (def.hasLocomotion) {
        Value::Object roles;
        for (int i = 0; i < kClipRoleCount; ++i) {
            const auto role = ClipRole(i);
            if (def.locomotionRoles.has(role))
                roles.emplace_back(clipRoleName(role), S(def.locomotionRoles.get(role)));
        }
        locomotion = Value(Value::Object{{"roles", Value(std::move(roles))},
                                         {"asset", locomotionAssetToJson(def.locomotion)}});
    }

    Value movement;
    if (def.hasMovement) {
        const auto &p = def.movement;
        const auto F = [](float f) { return Value(double(f)); };
        movement = Value(Value::Object{
            {"walkSpeed", F(p.walkSpeed)}, {"runSpeed", F(p.runSpeed)},
            {"maxAcceleration", F(p.maxAcceleration)}, {"brakingDeceleration", F(p.brakingDeceleration)},
            {"groundFriction", F(p.groundFriction)}, {"jumpVelocity", F(p.jumpVelocity)},
            {"jumpCount", Value(p.jumpCount)}, {"coyoteTime", F(p.coyoteTime)}, {"jumpReArm", F(p.jumpReArm)},
            {"airControl", F(p.airControl)}, {"gravityScale", F(p.gravityScale)},
            {"maxStepHeight", F(p.maxStepHeight)}, {"walkableFloorAngle", F(p.walkableFloorAngle)},
            {"orientRotationToMovement", Value(p.orientRotationToMovement)}, {"rotationRate", F(p.rotationRate)},
            {"capsuleAuto", Value(p.capsuleAuto)}, {"capsuleRadius", F(p.capsuleRadius)},
            {"capsuleHeight", F(p.capsuleHeight)}});
    }

    return Value(Value::Object{
        {"format", Value("jah-avatar")},
        {"version", Value(AvatarDefinition::kFormatVersion)},
        {"name", S(def.name)},
        {"model", Value(Value::Object{{"asset", S(def.modelAsset)}})},
        {"rig", Value(Value::Object{{"rigId", S(def.rig.rigId)}, {"bones", Value(def.rig.bones)},
                                    {"boneNames", Value(std::move(boneNames))}, {"unitScale", unitScale}})},
        {"clips", Value(std::move(clips))},
        {"defaults", Value(Value::Object{{"activeClip", S(def.defaultClip)}})},
        {"locomotion", locomotion},
        {"movement", movement},
        {"sockets", def.hasSockets ? def.sockets : Value()},
        {"blendshapes", def.hasBlendshapes ? def.blendshapes : Value()}});
}

bool avatarDefinitionFromJson(const Value &obj, AvatarDefinition &out, QString *error)
{
    const auto refuse = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    // A member that is absent or of another kind reads as QJsonValue's default did (valueshapes.h).
    const auto Q = [](const Value &o, const char *key) { return QString::fromStdString(stringOr(o, key)); };

    const QString format = Q(obj, "format");
    if (format != QStringLiteral("jah-avatar"))
        return refuse(QStringLiteral("not an avatar definition: format is '%1', expected "
                                     "'jah-avatar'").arg(format));
    const int version = intOr(obj, "version", 0);
    // FORWARD refusal, not a silent partial read: a v2 file this build cannot
    // understand must say so rather than load as an avatar missing whatever v2
    // added.
    if (version <= 0 || version > AvatarDefinition::kFormatVersion)
        return refuse(QStringLiteral("avatar definition version %1 is not readable by this "
                                     "build (it understands up to %2)")
                          .arg(version).arg(AvatarDefinition::kFormatVersion));

    AvatarDefinition def;
    def.name = Q(obj, "name");
    def.modelAsset = Q(obj["model"], "asset");
    if (def.modelAsset.isEmpty())
        return refuse(QStringLiteral("the avatar definition names no model asset"));

    const Value &rig = obj["rig"];
    def.rig.rigId = Q(rig, "rigId");
    def.rig.bones = intOr(rig, "bones", 0);
    const Value boneNamesIn = arrayOr(rig, "boneNames");   // bound: a temporary's asArray() would not outlive the loop
    for (const Value &v : boneNamesIn.asArray())
        def.rig.boneNames.append(v.kind() == Kind::String ? QString::fromStdString(v.asString()) : QString());
    const Value &unitScale = rig["unitScale"];
    if (unitScale.kind() == Kind::Number || unitScale.kind() == Kind::Int) {
        def.rig.hasUnitScale = true;
        def.rig.unitScale = unitScale.asNumber();
    }

    QSet<QString> names;
    const Value clipsIn = arrayOr(obj, "clips");   // bound: a temporary's asArray() would not outlive the loop
    for (const Value &entry : clipsIn.asArray()) {
        AvatarClipEntry clip;
        clip.asset = Q(entry, "asset");
        clip.rawName = Q(entry, "rawName");
        clip.name = Q(entry, "name");
        clip.looping = boolOr(entry, "looping", true);
        clip.rootMotion = boolOr(entry, "rootMotion", false);
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

    def.defaultClip = Q(obj["defaults"], "activeClip");
    if (!def.defaultClip.isEmpty() && !names.contains(def.defaultClip))
        return refuse(QStringLiteral("the default clip '%1' is not one of this avatar's clips")
                          .arg(def.defaultClip));

    const Value &loco = obj["locomotion"];
    if (loco.kind() == Kind::Object) {
        QString locoError;
        if (loco.find("asset")
            && !locomotionAssetFromJson(objectOr(loco, "asset"), def.locomotion, &locoError))
            return refuse(QStringLiteral("the locomotion asset is not readable: %1").arg(locoError));
        // In the order a QJsonObject held them (sorted by name): the first unknown role is the one named.
        const Value roles = objectOr(loco, "roles");
        std::vector<const Value::Member *> sorted;
        for (const Value::Member &m : roles.asObject()) sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [](const Value::Member *a, const Value::Member *b) {
            return QString::fromStdString(a->first) < QString::fromStdString(b->first);
        });
        for (const Value::Member *m : sorted) {
            ClipRole role;
            const QString key = QString::fromStdString(m->first);
            if (!clipRoleFromName(key, role))
                return refuse(QStringLiteral("'%1' is not a clip role").arg(key));
            def.locomotionRoles.set(role, m->second.kind() == Kind::String ? QString::fromStdString(m->second.asString())
                                                                           : QString());
        }
        def.hasLocomotion = true;
    }

    const Value &m = obj["movement"];
    if (m.kind() == Kind::Object) {
        AvatarMovementParams p;
        const auto f = [&m](const char *key, float fallback) { return float(numberOr(m, key, fallback)); };
        p.walkSpeed = f("walkSpeed", p.walkSpeed);
        p.runSpeed = f("runSpeed", p.runSpeed);
        p.maxAcceleration = f("maxAcceleration", p.maxAcceleration);
        p.brakingDeceleration = f("brakingDeceleration", p.brakingDeceleration);
        p.groundFriction = f("groundFriction", p.groundFriction);
        p.jumpVelocity = f("jumpVelocity", p.jumpVelocity);
        p.jumpCount = intOr(m, "jumpCount", p.jumpCount);
        p.coyoteTime = f("coyoteTime", p.coyoteTime);
        p.jumpReArm = f("jumpReArm", p.jumpReArm);
        p.airControl = f("airControl", p.airControl);
        p.gravityScale = f("gravityScale", p.gravityScale);
        p.maxStepHeight = f("maxStepHeight", p.maxStepHeight);
        p.walkableFloorAngle = f("walkableFloorAngle", p.walkableFloorAngle);
        p.orientRotationToMovement = boolOr(m, "orientRotationToMovement", p.orientRotationToMovement);
        p.rotationRate = f("rotationRate", p.rotationRate);
        p.capsuleAuto = boolOr(m, "capsuleAuto", p.capsuleAuto);
        p.capsuleRadius = f("capsuleRadius", p.capsuleRadius);
        p.capsuleHeight = f("capsuleHeight", p.capsuleHeight);
        def.movement = p;
        def.hasMovement = true;
    }

    const Value &sockets = obj["sockets"];
    if (sockets.kind() == Kind::Object) { def.sockets = sockets; def.hasSockets = true; }
    const Value &blendshapes = obj["blendshapes"];
    if (blendshapes.kind() == Kind::Object) {
        def.blendshapes = blendshapes;
        def.hasBlendshapes = true;
    }

    out = def;
    return true;
}

}   // namespace iris
