/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_CLIPNAMING_H
#define IRIS_CLIPNAMING_H

// THE ONE RULE THAT NAMES AN ANIMATION CLIP (SKELETAL_PLAYBACK_SPEC S1).
//
// A clip's name is what the user sees in the clip list, what a saved
// {source, name} reference resolves through, and — since the import dialog —
// what the `clips` import setting FILTERS ON (import/importsettings.h:
// ImportTransform::wantsClip matches these names case-insensitively). The
// filter and the list must therefore agree exactly, and they are produced in
// two different places: Mesh::extractAnimations builds the real clips at
// import, and the dialog's light pre-read (ModelPreRead) lists the names
// before anything is imported at all. A checklist offering a name the filter
// cannot match would silently import nothing.
//
// Hence one inline rule, called by both:
//   * a clip keeps the name the file gave it;
//   * a file that named it nothing gets "clip <index>" (the old code collapsed
//     those to "", so several clips overwrote one map key and no saved
//     reference to them could ever resolve);
//   * a duplicate gets " 2", " 3", … appended, in file order.

#include <QString>

namespace iris
{

/// The name clip number `index` gets, given the name the FILE carries
/// (possibly empty) and `taken`, the names already produced for the clips
/// before it. `Taken` is anything with a `contains(const QString &)` — a
/// QMap of clips, a QSet or a QStringList all work.
template <typename Taken>
inline QString clipNameFor(const QString &fileName, unsigned index, const Taken &taken)
{
    QString name = fileName;
    if (name.isEmpty()) name = QStringLiteral("clip %1").arg(index);
    const QString base = name;
    for (int suffix = 2; taken.contains(name); ++suffix)
        name = base + QStringLiteral(" %1").arg(suffix);
    return name;
}

} // namespace iris

#endif // IRIS_CLIPNAMING_H
