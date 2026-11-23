/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/meshmaterial.h"
#include "document/properties/valueshapes.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

#include "import/scenebuilder.h"
#include "document/materials/pbrmaterial.h"

namespace iris
{

namespace {

/// THE SHININESS REMAP, and it is the mirror's, not a new one
/// (SceneMirror::toPbrParams). Legacy Blinn shininess is inverse-sense and
/// assimp encodes glTF roughness into it as (1-r)^2 * 1000, so the 128 clamp
/// and the 0.9 span are load-bearing: without the clamp every imported smooth
/// material came back a near-mirror. Keeping ONE formula is why this function
/// exists rather than an inline expression at each call site.
float roughnessFromShininess(float shininess)
{
    const float s = std::max(0.0f, std::min(shininess, 128.0f));
    return 1.0f - std::sqrt(s / 128.0f) * 0.9f;
}

} // namespace

PbrMaterialPtr materialFromMeshData(const MeshMaterialData &data)
{
    auto mat = PbrMaterial::create();
    // THE SOURCE'S COVERAGE (BISTRO-IMPORT-1): a cutout, a glass and its alpha, read
    // by MaterialHelper from the file's own facts — on both arms below.
    mat->setValue("alphaMode", Value(data.alphaMode));
    mat->setValue("alpha", Value(double(data.alpha)));
    mat->setValue("alphaCutoff", Value(double(data.alphaCutoff)));
    // Every Value below is of its row's own kind (the material's strict setter takes it): the importer's
    // answers are not read. A colour crosses as its Value shape (valueshapes.h), no Qt.
    auto bindIfFile = [&mat](const char *row, const QString &path) {
        if (!path.isEmpty() && QFileInfo(path).isFile()) mat->setValue(row, Value(path.toStdString()));
    };

    const auto isFile = [](const QString &p) {
        return !p.isEmpty() && QFileInfo(p).isFile();
    };

    if (data.hasPbr) {
        // The source really was a PBR one (glTF): use its values verbatim
        // rather than round-tripping through assimp's lossy shininess
        // back-conversion, which is what produced near-mirror imports. A
        // spec-gloss source arrives already converted (MaterialHelper).
        mat->setValue("baseColor", colourToValue(data.baseColorFactor));
        mat->setValue("metallic", Value(double(data.metallicFactor)));
        mat->setValue("roughness", Value(double(data.roughnessFactor)));
        bindIfFile("baseColorMap", data.baseColorTexture);
        bindIfFile("metallicMap",  data.metallicTexture);
        bindIfFile("roughnessMap", data.roughnessTexture);

        // ---- the WORKFLOW (MATERIAL_GAPS_SPEC GAP 1) ----------------------
        // A spec-gloss or KHR_materials_specular source now arrives in its own
        // workflow instead of converted. The IOR rides along on EVERY workflow
        // (inert on metallic, restored by a switch); kS and F0 only mean
        // something on the two specular ones but are stored the same way, for
        // the same reason.
        mat->setValue("workflow", Value(data.workflow));
        mat->setValue("ior", Value(double(data.ior)));
        if (data.workflow != 0) {
            mat->setValue("specularColor", colourToValue(data.specularFactor));
            mat->setValue("useFresnelColor", Value(data.useFresnelColor));
            if (data.useFresnelColor)
                mat->setValue("fresnelColor", colourToValue(data.fresnelFactor));
            // The specular / spec-gloss map binds to the SHARED metallic-specular
            // texture unit — the renderer reinterprets one unit by workflow, so
            // `metallicMap` is the right row and only one of the two sources can
            // fill it. Before the workflow switch this map had nowhere to go and
            // was dropped with a warning.
            bindIfFile("metallicMap", data.specularMapTexture);
        }
        bindIfFile("normalMap",    data.normalTexture);
        bindIfFile("emissiveMap",  data.emissiveTexture);

        // EMISSIVE INTENSITY IS PART OF "THERE IS EMISSION" (2026-09-08).
        // emissiveColor without emissiveIntensity is emission multiplied by
        // zero — the mirror pushes colour*intensity (scenemirror.cpp) — and
        // the import path that Studio actually uses set the colour and left
        // the intensity at the document default 0, so every model came back
        // with "#ffffff at 0". The two travel together or not at all.
        //
        // A BLACK emissive factor stays dark, map or no map: glTF's
        // emissiveFactor defaults to [0,0,0] and multiplies the emissive
        // texture, so "there is an emissive map" is NOT on its own a
        // statement that the surface emits. Inventing white emission for one
        // would light up every model whose exporter wrote a map the artist
        // muted. (Unlit is the one place a black factor is overridden, below,
        // and for a different reason: there the map is the COLOUR.)
        const bool emissiveMap = isFile(data.emissiveTexture);
        const bool emissiveTinted =
            data.emissionColor.isValid() && data.emissionColor != SrgbColor(0, 0, 0);
        if (emissiveTinted) {
            mat->setValue("emissiveColor", colourToValue(data.emissionColor));
            mat->setValue("emissiveIntensity", Value(1.0));
        }

        // KHR_materials_unlit. The engine's Unlit family consumes NO lighting
        // inputs at all — no emissive, no maps but the colour one
        // (PbrMaterial::rowsUnusedWhenUnlit) — so an unlit material's visible
        // colour has to end up on baseColor/baseColorMap or it is lost. The
        // common unlit export (Sketchfab and friends) leaves baseColorFactor
        // BLACK and puts the artwork in emissiveTexture/emissiveFactor: read
        // literally that is a black model, which is exactly how those files
        // imported. So on an unlit material the emissive slot BECOMES the
        // colour slot when the base-colour one is empty or black.
        if (data.unlit) {
            mat->setValue("shadingModel", Value(1));
            const bool baseIsBlack = !data.baseColorFactor.isValid() ||
                                     data.baseColorFactor == SrgbColor(0, 0, 0);
            if (!isFile(data.baseColorTexture) && emissiveMap)
                mat->setValue("baseColorMap", Value(data.emissiveTexture.toStdString()));
            if (baseIsBlack && (emissiveTinted || emissiveMap))
                mat->setValue("baseColor",
                              colourToValue(emissiveTinted ? data.emissionColor : SrgbColor(255, 255, 255)));
        }
        return mat;
    }

    mat->setValue("baseColor",
                  colourToValue(data.diffuseColor.isValid() ? data.diffuseColor : SrgbColor(200, 200, 200)));
    mat->setValue("metallic", Value(0.0));
    mat->setValue("roughness", Value(double(roughnessFromShininess(data.shininess))));
    bindIfFile("baseColorMap", data.diffuseTexture);
    bindIfFile("normalMap",    data.normalTexture);
    // specularTexture / hightTexture have no metallic-roughness home, and never
    // reached the renderer through the old path either.
    return mat;
}

}   // namespace iris
