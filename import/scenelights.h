/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_SCENELIGHTS_H
#define IRIS_IMPORT_SCENELIGHTS_H

#include "irisglfwd.h"

namespace iris
{

class SceneSource;

/// THE FILE'S LIGHTS AND CAMERAS (BISTRO-IMPORT-1). A model file can carry lights and
/// cameras (an FBX scene: the Bistro Exterior's sun, the Interior's four lamps, a shot
/// camera in each); assimp reads them as aiLight / aiCamera, each bound to the aiNode of
/// the same NAME, and nothing read them — the import kept the empty node and dropped the
/// light. This adopts them into the import's fragment, as document nodes:
///
///   * one LightNode / CameraNode per aiLight / aiCamera, a CHILD of the fragment node
///     that carries its name — so the file's own transform chain (pivots, the unit
///     scale, a Take that animates it) places it, and the fragment's existing node
///     order is untouched (the bake's fragment pairs its children with the blob's by
///     index: AssetHelper::updateNodeMaterial);
///   * the light's colour x intensity is the file's product exactly (assimp folds FBX
///     Intensity into mColorDiffuse): intensity = the brightest channel, colour = the
///     rest, both linear -> the document's sRGB colour. The units are the file's DCC
///     units, which are ours (a light of intensity I lights a facing surface as I x PI;
///     Maya's directional and its quadratic-decay lamp read the same way);
///   * a directional light keeps the constructor's forward shading priority 0: it is a
///     SUN, and a second sun in the scene it is placed in is the `sun.tie` issue line
///     (services/sceneissues.cpp) — the one-sun rule, never a silent demotion;
///   * a camera's angle is the file's horizontal field of view converted to the
///     document's VERTICAL angle through the file's aspect, its clip planes the file's.
///
/// Returns how many of each were adopted. The fragment must be the one built from
/// `source`'s scene (MeshNode::loadAsSceneFragment over the same SceneSource).
struct AdoptedSceneNodes
{
    int lights = 0;
    int cameras = 0;
};
AdoptedSceneNodes adoptLightsAndCameras(const SceneSource &source, const SceneNodePtr &fragment);

}   // namespace iris

#endif   // IRIS_IMPORT_SCENELIGHTS_H
