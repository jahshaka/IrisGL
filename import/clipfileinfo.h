/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_CLIPFILEINFO_H
#define IRIS_CLIPFILEINFO_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector3D>

namespace iris
{

/// An animation CLIP FILE read for its names and numbers (ImportFlags::
/// ClipNamesOnly — no geometry post-processing, the file's unit factor still
/// applied so a clip's translation keys match a character parsed with the
/// canonical preset). What Studio's clip reader (services/animationfile.cpp)
/// builds its clip table, rig signature and POSE STRIP thumbnail from, with
/// no importer type in sight (ENGINEERING_DEBT L4 part 3).
///
/// The strip needs the skeleton's shape at a few moments of the first clip,
/// so the reader can SAMPLE it: `poseFractions` names the moments (fractions
/// of the first clip's duration) and `poses` carries one evaluated pose per
/// fraction — world-space joint positions in FILE space (x right, y up), one
/// per scene node, with `nodeParents` giving the bone the joint hangs from.
/// The interpolation is the importer's own (quaternion slerp, linear
/// vectors), evaluated here so the drawing on the Studio side is only a
/// projection. Which channels are pivot bookkeeping, and what a clip is
/// called on a tile, are Studio's rules (services/rigsignature.h) and stay
/// there.
struct ClipFileInfo
{
    bool parsed = false;    ///< the importer read the file at all
    QString error;          ///< why not, when it did not
    int meshes = 0;         ///< a clip file has none (a .bvh reports a synthesised stick figure)
    int animations = 0;

    struct Clip
    {
        QString name;                   ///< as the file names it ("mixamo.com", "Take 001", …)
        double ticksPerSecond = 0.0;    ///< raw; 0 when the file states none
        double durationTicks = 0.0;     ///< raw
        double lengthSeconds = 0.0;     ///< durationTicks / tps, 25 fps when unstated
        QStringList channelNames;       ///< one per channel, the node it drives
    };
    QVector<Clip> clips;

    /// The node hierarchy, pre-order: `nodeParents[i]` indexes `nodeNames`
    /// (-1 for the root).
    QStringList nodeNames;
    QVector<int> nodeParents;

    /// One evaluated pose of the FIRST clip per requested fraction, in the
    /// order requested; `positions` is indexed like `nodeNames`. Empty when
    /// no fraction was requested, or when the first clip has no channels or
    /// no duration to sample.
    struct Pose
    {
        QVector<QVector3D> positions;
    };
    QVector<Pose> poses;

    /// ONE parse. `poseFractions` empty = names and numbers only.
    static ClipFileInfo read(const QString &filePath,
                             const QVector<double> &poseFractions = QVector<double>());
};

} // namespace iris

#endif // IRIS_CLIPFILEINFO_H
