/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_LOOKS_H
#define IRIS_LOOKS_H

// THE LOOKS CATALOGUE, on the DOCUMENT (SPECS/POST_LOOKS_SPEC.md §4.1).
//
// A "look" is one LDR image filter — desaturate, posterize, a film grade — and
// a scene carries an ORDERED STACK of them (Scene::looks) that the renderer
// applies to the finished picture, after tonemapping and after anti-aliasing.
//
// WHY THE TABLE LIVES HERE and not in Studio's services/ with the World-Mode
// rows it otherwise resembles: THREE consumers need it and only one of them is
// Studio. SceneMirror (irisgl) resolves the stack into the engine's PostFxDesc
// every frame; the scene reader validates authored JSON against it; and the
// panel, the verbs and the generated docs read it from Studio. A table in
// src/services/ would be invisible to the first two, exactly as the per-camera
// post-override key table had to live on CameraNode rather than beside the
// panel that draws it (cameranode.cpp's kPostKeys, and the test that asserts
// Studio's rows agree with it).
//
// WHAT IS HERE AND WHAT IS NOT. This table is the CONTRACT: which looks exist,
// what each one is called in a document and in a script, which parameters it
// takes, what they default to and the range outside which a value is a lie.
// The human labels, the scrub sensitivities and the tooltips are presentation
// and live in Studio (src/services/looks.h), which reads its ranges from here
// rather than restating them — so there is nothing for the two to disagree
// about (services/looks.cpp's agreement test proves it).
//
// THE ONE RULE THAT IS NOT OBVIOUS: a look id may appear AT MOST ONCE in a
// stack. Every look's parameters live on one process-global Ogre material, so a
// second instance would be handed the first one's numbers (§7 R2).
// normalizeLookStack drops the duplicate, and every write path goes through it.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace iris
{

/// The looks the renderer implements. The VALUES ARE THE WIRE FORMAT in one
/// direction only — SceneMirror maps them onto the engine's own LookKind with
/// an explicit switch, because the document must not depend on the engine's
/// header. Documents store the string `id`, never this.
enum class LookKind
{
    Desaturate = 0,
    GlassWarp,
    RadialBlur,
    OldMovie,
    Posterize,
    Sharpen,
    FilmGrade,
    Count
};

/// One parameter of one look. `defaultValue` is what an entry that does not
/// mention the parameter means, and the range is enforced on every read and
/// every write — an authored file cannot inject a value the shader was not
/// written for.
struct LookParamDef
{
    const char *id;
    float       defaultValue;
    float       minValue;
    float       maxValue;
};

/// One look. `params[0]` is ALWAYS the amount, and every look is an exact
/// identity at amount 0 — that is the property the pixel gates assert and the
/// reason "a look at zero" and "no look" are the same picture.
struct LookDef
{
    const char         *id;       ///< stable, script-facing and stored ("desaturate")
    LookKind            kind;
    const LookParamDef *params;
    int                 paramCount;   ///< <= 8 (the engine's LookDesc::p)
};

/// The catalogue, in the order the panel offers looks in (not the order of any
/// stack). Never null; `count` receives the entry count.
const LookDef *lookCatalogue(int &count);

/// The look with this id, or null.
const LookDef *lookDef(const QString &id);

/// The engine's parameter vector for one stack entry: the entry's values in the
/// look's own canonical order, clamped, defaulted, and zero-filled to 8.
void lookParamValues(const LookDef &def, const QJsonObject &entry, float out[8]);

/// THE ONE VALIDATOR every write path shares (verbs, reader, panel).
///
/// Takes an authored array and returns the canonical form of it: unknown ids
/// dropped, duplicate ids dropped (see the header), `enabled` normalised to a
/// bool, and every parameter present, in the look's own order, clamped to its
/// range. Entries keep the order they arrived in — the array order IS the frame
/// order and is the whole authoring model.
QJsonArray normalizeLookStack(const QJsonArray &in);

/// One canonical entry for `def`, taking whatever `params` names and defaulting
/// the rest. The building block the add verb uses.
QJsonObject makeLookEntry(const LookDef &def, const QJsonObject &params = QJsonObject(),
                          bool enabled = true);

}   // namespace iris

#endif   // IRIS_LOOKS_H
