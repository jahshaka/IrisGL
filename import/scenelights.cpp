/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/scenelights.h"

#include "assimp/scene.h"
#include "core/color.h"
#include "core/math/quat.h"
#include "document/scenegraph/cameralens.h"
#include "document/scenegraph/cameranode.h"
#include "document/scenegraph/lightnode.h"
#include "document/scenegraph/scenenode.h"
#include "import/scenesource.h"

#include <QHash>
#include <QPair>
#include <QVector>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

namespace iris
{

namespace {

Vec3 vec(const aiVector3D &v) { return Vec3(v.x, v.y, v.z); }

/// The fragment's nodes by name, first in depth order (assimp names every node a light
/// or camera binds to uniquely). Mesh nodes are named after their MESH, never the node,
/// so only non-mesh nodes can carry a light or a camera.
QHash<QString, SceneNodePtr> nodesByName(const SceneNodePtr &root)
{
    QHash<QString, SceneNodePtr> out;
    std::function<void(const SceneNodePtr &)> walk = [&](const SceneNodePtr &n) {
        if (n->getSceneNodeType() != SceneNodeType::Mesh && !out.contains(n->getName()))
            out.insert(n->getName(), n);
        for (const auto &c : n->children()) walk(c);
    };
    if (root) walk(root);
    return out;
}

/// The document's light for one aiLight, or null for a kind the document has no light
/// for (assimp's AMBIENT and UNDEFINED).
LightNodePtr lightFor(const aiLight *l)
{
    LightType type;
    switch (l->mType) {
    case aiLightSource_DIRECTIONAL: type = LightType::Directional; break;
    case aiLightSource_POINT:       type = LightType::Point;       break;
    case aiLightSource_SPOT:        type = LightType::Spot;        break;
    case aiLightSource_AREA:        type = LightType::Area;        break;
    default: return LightNodePtr();
    }
    auto light = LightNode::create();
    light->setName(QString::fromUtf8(l->mName.C_Str()));
    light->setLightType(type);
    const float r = l->mColorDiffuse.r, g = l->mColorDiffuse.g, b = l->mColorDiffuse.b;
    const float peak = std::max({ r, g, b, 0.0f });
    light->intensity = peak;
    light->color = peak > 0.0f ? srgbOf(r / peak, g / peak, b / peak) : QColor(255, 255, 255);
    if (type == LightType::Point || type == LightType::Spot) {
        // A file lamp states no range; it gets the editor's new lamp's (sceneeditservice).
        light->distance = 40.0f;
    }
    if (type == LightType::Spot && l->mAngleOuterCone > 0.0f) {
        // assimp's cone angles are FULL angles; the document's cutoff is the HALF angle.
        light->spotCutOff = qBound(1.0f, float(qRadiansToDegrees(l->mAngleOuterCone)) * 0.5f, 85.0f);
        const float inner = qBound(0.0f, l->mAngleInnerCone, l->mAngleOuterCone);
        light->spotCutOffSoftness = qBound(0.0f, 1.0f - inner / l->mAngleOuterCone, 1.0f);
    }
    if (type == LightType::Area) {
        light->rectWidth = l->mSize.x > 0.0f ? l->mSize.x : 1.0f;
        light->rectHeight = l->mSize.y > 0.0f ? l->mSize.y : 1.0f;
    }
    // Placed in its node's frame: the document's lights shine down local -Y.
    light->setLocalPos(vec(l->mPosition));
    Vec3 dir = vec(l->mDirection);
    if (dir.lengthSquared() <= 0.0f) dir = Vec3(0, -1, 0);
    light->setLocalRot(Quat::rotationTo(Vec3(0, -1, 0), dir));
    return light;
}

CameraNodePtr cameraFor(const aiCamera *c)
{
    auto camera = CameraNode::create();
    camera->setName(QString::fromUtf8(c->mName.C_Str()));
    const float aspect = c->mAspect > 0.0f ? c->mAspect : 16.0f / 9.0f;
    camera->setAspectRatio(aspect);
    if (c->mHorizontalFOV > 0.0f) {
        // assimp's mHorizontalFOV is the HALF horizontal angle; the document stores the
        // full VERTICAL angle (CAMERAS_SPEC §1).
        const float vertical = 2.0f * std::atan(std::tan(c->mHorizontalFOV) / aspect);
        camera->setFieldOfViewDegrees(float(qRadiansToDegrees(vertical)));
    }
    if (c->mClipPlaneNear > 0.0f) camera->nearClip = c->mClipPlaneNear;
    if (c->mClipPlaneFar > camera->nearClip) camera->farClip = c->mClipPlaneFar;
    // The document's cameras look down local -Z with +Y up.
    camera->setLocalPos(vec(c->mPosition));
    Vec3 look = vec(c->mLookAt), up = vec(c->mUp);
    if (look.lengthSquared() <= 0.0f) look = Vec3(0, 0, -1);
    if (up.lengthSquared() <= 0.0f) up = Vec3(0, 1, 0);
    camera->setLocalRot(Quat::fromDirection(-look, up));
    return camera;
}

}   // namespace

AdoptedSceneNodes adoptLightsAndCameras(const SceneSource &source, const SceneNodePtr &fragment)
{
    AdoptedSceneNodes out;
    const aiScene *scene = source.scene();
    if (!scene || !fragment) return out;
    if (scene->mNumLights == 0 && scene->mNumCameras == 0) return out;
    const auto byName = nodesByName(fragment);
    QVector<QPair<SceneNodePtr, LightNodePtr>> adopted;
    float sunPeak = 0.0f, lampPeak = 0.0f;
    for (unsigned i = 0; i < scene->mNumLights; ++i) {
        const auto host = byName.value(QString::fromUtf8(scene->mLights[i]->mName.C_Str()));
        auto light = lightFor(scene->mLights[i]);
        if (!host || !light) continue;
        if (light->lightType == LightType::Directional) sunPeak = std::max(sunPeak, light->intensity);
        else lampPeak = std::max(lampPeak, light->intensity);
        adopted.append({ host, light });
    }
    // THE FILE'S UNIT IS NOT OURS, ITS RATIOS ARE. An FBX light's Intensity is its
    // DCC's percent-of-a-renderer-unit and carries no photometric unit at all (the
    // Bistro Exterior's sun is 15000 -> 110 x the engine's sun; the Interior's lamps
    // 280): taken literally the street rendered 6.8 stops over the default exposure,
    // a white frame. So the file's lights keep their RATIOS and their colours, and the
    // file's key light is anchored to the engine's unit: its brightest directional
    // becomes a sun of 1 (the default sun the exposure is built for); a file with lamps
    // only anchors its brightest lamp at the editor's new lamp (lens::kDefaultLampIntensity).
    const float anchor = sunPeak > 0.0f ? 1.0f / sunPeak
                       : lampPeak > 0.0f ? lens::kDefaultLampIntensity / lampPeak : 1.0f;
    out.anchor = anchor;
    if (!adopted.isEmpty())
        out.anchorNote = sunPeak > 0.0f
            ? QStringLiteral("the file's lights keep their ratios and colours; its brightest "
                             "directional (%1 in the file's own units) is the engine's sun of 1")
                  .arg(double(sunPeak), 0, 'g', 4)
            : QStringLiteral("the file's lights keep their ratios and colours; its brightest lamp "
                             "(%1 in the file's own units) is the editor's new lamp of %2")
                  .arg(double(lampPeak), 0, 'g', 4).arg(double(lens::kDefaultLampIntensity));
    for (const auto &entry : adopted) {
        entry.second->intensity *= anchor;
        entry.first->addChild(entry.second, false);
        ++out.lights;
    }
    for (unsigned i = 0; i < scene->mNumCameras; ++i) {
        const auto host = byName.value(QString::fromUtf8(scene->mCameras[i]->mName.C_Str()));
        if (!host) continue;
        host->addChild(cameraFor(scene->mCameras[i]), false);
        ++out.cameras;
    }
    return out;
}

}   // namespace iris
