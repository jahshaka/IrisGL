/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/graphicshelper.h"

#include <QRegularExpression>

#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/mesh.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"

#include <QFile>
#include <QFileInfo>

#include "document/assets/vertexlayout.h"

namespace iris
{

QString GraphicsHelper::loadAndProcessShader(QString shaderPath)
{
    QRegularExpression internalFileInclude("\\<(.+\\\\)*((.+)\\.(.+))\\>");
    QRegularExpression externalFileInclude("\\\"(.+\\\\)*((.+)\\.(.+))\\\"");

    QFile file(shaderPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        qWarning("GraphicsHelper::loadAndProcessShader: failed to open %s", qUtf8Printable(shaderPath));

    auto text = file.readAll();
    auto lines = text.split('\n');

    for (int i = 0; i < lines.count(); ++i) {
        if (lines[i].startsWith("#pragma include")) {
            QString includeFile = "";
            QRegularExpressionMatch match;
            if ((match = internalFileInclude.match(lines[i])) .hasMatch()) {
                auto filename = match.captured(2);
                includeFile = ":assets/shaders/" + filename;
            } else if ((match = externalFileInclude.match(lines[i])).hasMatch()) {
                auto filename = match.captured(2);
                includeFile = QFileInfo(shaderPath).absolutePath() + "/" + filename;
            }

            // remove line with pragma
            lines.removeAt(i);

            auto included = loadAndProcessShader(includeFile);
            lines.insert(i, included.toUtf8());

            // todo: include file index in line directive?
            auto lineDirective = QString("#line %1").arg(i + 2);
            lines.insert(i + 1, lineDirective.toUtf8());
        }
    }

    return lines.join('\n');
}

QList<iris::MeshPtr> GraphicsHelper::loadAllMeshesFromFile(QString filePath)
{
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(filePath.toStdString().c_str(), aiProcessPreset_TargetRealtime_Fast);
    return loadAllMeshesFromAssimpScene(scene);
}

void GraphicsHelper::loadAllMeshesAndAnimationsFromFile(
    QString filePath,
    QList<MeshPtr> &meshes,
    QMap<QString, SkeletalAnimationPtr> &animations)
{
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(filePath.toStdString().c_str(), aiProcessPreset_TargetRealtime_Fast);

    if (scene != nullptr) {
        meshes = loadAllMeshesFromAssimpScene(scene);
        animations = Mesh::extractAnimations(scene, filePath);
    }
}

QList<MeshPtr> GraphicsHelper::loadAllMeshesFromAssimpScene(const aiScene *scene)
{
    QList<MeshPtr> meshes;

    if (scene) {
        for (unsigned i = 0; i < scene->mNumMeshes; i++) {
            auto m = scene->mMeshes[i];
            auto mesh = iris::MeshPtr(new Mesh(m));
            if (m->HasBones()) {
                auto skel = Mesh::extractSkeleton(m, scene);
                mesh->setSkeleton(skel);
            }
            meshes.append(mesh);
        }
    }

    return meshes;
}

}
