/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/materials/pbrmaterial.h"
#include "document/assets/livetextures.h"
#include "document/assets/texture2d.h"
#include "core/logger.h"
#include "core/properties/property.h"

namespace iris
{

PbrMaterial::PbrMaterial()
{
    baseColor           = QColor(255, 255, 255);
    baseColorFactor     = 1.0f;
    useBaseColorMap     = false;

    metallicFactor      = 0.0f;
    useMetallicMap      = false;
    roughnessFactor     = 0.5f;
    useRoughnessMap     = false;
    roughnessLowerBound = 0.0f;
    roughnessUpperBound = 1.0f;

    useNormalMap        = false;
    normalFactor        = 1.0f;

    // HLMS_ADOPTION P1. EVERY ONE OF THESE DEFAULTS IS THE RENDERER'S OWN
    // CONSTRUCTED DEFAULT, deliberately: at these values the backend removes
    // the clear-coat shader blocks entirely and its BRDF/shadow/lightmap
    // setters early-return, so an existing scene's generated shader text — and
    // therefore its pixels — is unchanged by this feature existing.
    // HLMS_ADOPTION P4a. Lit is the default for the same reason: it is the
    // family every existing material is already in, so the row's existence
    // cannot move a pixel of shipped content.
    shadingModel        = 0;      // Lit
    // MATERIAL_GAPS_SPEC GAP 1. Metallic is what every material in the library
    // already is (the engine applied it unconditionally at every creation
    // site), and every other value here is inert in that workflow — white kS
    // is a no-op multiplier, and ior/fresnelColor/separateFresnel are not read
    // at all. So the rows existing cannot move a pixel of shipped content.
    workflow            = 0;      // Metallic
    specularColor       = QColor(255, 255, 255);
    ior                 = 1.5f;   // window glass; F0 = 0.04
    fresnelColor        = QColor(10, 10, 10);   // ~0.04 linear, the dielectric F0
    useFresnelColor     = false;
    separateFresnel     = false;
    clearCoat           = 0.0f;
    clearCoatRoughness  = 0.0f;
    brdf                = 0;      // Default
    receiveShadows      = true;
    emissiveAsLightmap  = false;

    emissiveColor       = QColor(0, 0, 0);
    emissiveIntensity   = 0.0f;
    useEmissiveMap      = false;
    useReflectionMap    = false;

    alpha               = 1.0f;
    alphaCutoff         = 0.5f;
    alphaMode           = 0;
    refractionStrength  = 0.35f;

    textureScale        = 1.0f;
    textureScaleV       = 1.0f;
    textureOffsetU      = 0.0f;
    textureOffsetV      = 0.0f;
    textureRotation     = 0.0f;

    // ADDENDUM A-2. Both defaults are the values every map was hard-coded to
    // before the rows existed, so an existing material samples identically.
    anisotropy          = 1.0f;
    // mapAddress is left EMPTY, not filled with zeros: absent means Wrap
    // (addressFor's default), which is also what an old file with no address
    // keys reads as, so the two agree without a migration.

    // Opaque geometry. NOTE: CustomMaterial maps its "opaque" string to
    // RenderLayer::Background (custommaterial.cpp:296), which looks like a bug in
    // that mapping - Background is drawn before the sky. Using Opaque here.
    setRenderLayer(RenderLayer::Opaque);


    createProperties();
}

// ---------------------------------------------------------------- setters

void PbrMaterial::setBaseColor(QColor color)        { baseColor = color; }
void PbrMaterial::setBaseColorFactor(float factor)  { baseColorFactor = factor; }

void PbrMaterial::setBaseColorMap(Texture2DPtr tex)
{
    if (!!tex) { useBaseColorMap = true;  addTexture("u_baseColorMap", tex); }
    else       { useBaseColorMap = false; removeTexture("u_baseColorMap"); }
}

void PbrMaterial::setMetallicFactor(float factor)   { metallicFactor = factor; }

void PbrMaterial::setMetallicMap(Texture2DPtr tex)
{
    if (!!tex) { useMetallicMap = true;  addTexture("u_metallicMap", tex); }
    else       { useMetallicMap = false; removeTexture("u_metallicMap"); }
}

void PbrMaterial::setRoughnessFactor(float factor)  { roughnessFactor = factor; }

void PbrMaterial::setRoughnessMap(Texture2DPtr tex)
{
    if (!!tex) { useRoughnessMap = true;  addTexture("u_roughnessMap", tex); }
    else       { useRoughnessMap = false; removeTexture("u_roughnessMap"); }
}

void PbrMaterial::setNormalMap(Texture2DPtr tex)
{
    if (!!tex) { useNormalMap = true;  addTexture("u_normalMap", tex); }
    else       { useNormalMap = false; removeTexture("u_normalMap"); }
}

void PbrMaterial::setNormalFactor(float factor)     { normalFactor = factor; }

void PbrMaterial::setShadingModel(int model)            { shadingModel = model; }
void PbrMaterial::setWorkflow(int w)                    { workflow = w; }

// The renderer's detail texture units are bound from `textures` like every
// other map, under the sampler names the mirror's slot table reads.
void PbrMaterial::setDetailMap(int layer, Texture2DPtr tex)
{
    if (layer < 0 || layer >= kDetailLayers) return;
    const QString key = QStringLiteral("u_detail%1Map").arg(layer);
    if (!!tex) { detail[layer].map = tex->source; addTexture(key, tex); }
    else       { detail[layer].map.clear();       removeTexture(key); }
}

void PbrMaterial::setDetailNormalMap(int layer, Texture2DPtr tex)
{
    if (layer < 0 || layer >= kDetailLayers) return;
    const QString key = QStringLiteral("u_detail%1NormalMap").arg(layer);
    if (!!tex) { detail[layer].normalMap = tex->source; addTexture(key, tex); }
    else       { detail[layer].normalMap.clear();       removeTexture(key); }
}

void PbrMaterial::setDetailWeightMap(Texture2DPtr tex)
{
    if (!!tex) { detailWeightMap = tex->source; addTexture(QStringLiteral("u_detailWeightMap"), tex); }
    else       { detailWeightMap.clear();       removeTexture(QStringLiteral("u_detailWeightMap")); }
}

const QVector<const char *> &PbrMaterial::addressModeNames()
{
    // Index is the stored value and is permanent, like every other enum row.
    static const QVector<const char *> kNames = { "Wrap", "Clamp", "Mirror", "Border" };
    return kNames;
}

QString PbrMaterial::addressRow(const QString &mapRow)
{
    return mapRow + QStringLiteral("Address");
}

// EVERY MAP ROW, in the order the panel shows them. The one list the address
// rows, the panel and the mirror's slot mapping are all generated from — a
// second spelling of these names is how a picker and a renderer drift apart.
const QVector<QString> &PbrMaterial::mapRowNames()
{
    static const QVector<QString> kRows = [] {
        QVector<QString> rows = {
            QStringLiteral("baseColorMap"), QStringLiteral("normalMap"),
            QStringLiteral("metallicMap"),  QStringLiteral("roughnessMap"),
            QStringLiteral("emissiveMap"),
        };
        for (int i = 0; i < kDetailLayers; ++i) {
            rows << detailRow(i, "Map");
            rows << detailRow(i, "NormalMap");
        }
        rows << QStringLiteral("detailWeightMap");
        return rows;
    }();
    return kRows;
}

QString PbrMaterial::detailRow(int layer, const char *suffix)
{
    return QStringLiteral("detail%1%2").arg(layer).arg(QLatin1String(suffix));
}

// THE THIRTEEN BLEND MODES, in the renderer's own index order
// (OgreHlmsPbsPrerequisites.h PbsBlendModes). Index is the stored value and is
// permanent — the renderer's order, not ours to reshuffle.
const QVector<const char *> &PbrMaterial::detailBlendNames()
{
    static const QVector<const char *> kNames = {
        "NormalNonPremul", "NormalPremul", "Add", "Subtract", "Multiply",
        "Multiply2x", "Screen", "Overlay", "Lighten", "Darken",
        "GrainExtract", "GrainMerge", "Difference"
    };
    return kNames;
}
void PbrMaterial::setSpecularColor(QColor color)        { specularColor = color; }
void PbrMaterial::setIor(float v)                       { ior = v; }
void PbrMaterial::setFresnelColor(QColor color)         { fresnelColor = color; }
void PbrMaterial::setUseFresnelColor(bool use)          { useFresnelColor = use; }
void PbrMaterial::setSeparateFresnel(bool separate)     { separateFresnel = separate; }
void PbrMaterial::setClearCoat(float coat)              { clearCoat = coat; }
void PbrMaterial::setClearCoatRoughness(float r)        { clearCoatRoughness = r; }
void PbrMaterial::setBrdf(int index)                    { brdf = index; }
void PbrMaterial::setReceiveShadows(bool receive)       { receiveShadows = receive; }
void PbrMaterial::setEmissiveAsLightmap(bool asLightmap){ emissiveAsLightmap = asLightmap; }

// The BRDF vocabulary. Six of the renderer's twelve named values: the three
// families, plain and with SEPARATE diffuse fresnel (the variant that exists
// for glass, transparent plastics, fur and marbles — surfaces with complex
// re-scattering). The other six are uncorrelated/legacy-math combinations of
// these; twelve rows of jargon is worse product than six.
//
// INDEX IS THE STORED VALUE AND IT IS PERMANENT: appending is safe, reordering
// or removing a row silently re-points every saved material.
const QVector<PbrMaterial::BrdfName> &PbrMaterial::brdfNames()
{
    static const QVector<BrdfName> kNames = {
        { "Default",                            "Default" },
        { "CookTorrance",                       "Cook-Torrance" },
        { "BlinnPhong",                         "Blinn-Phong" },
        { "DefaultSeparateDiffuseFresnel",      "Default (Diffuse Fresnel)" },
        { "CookTorranceSeparateDiffuseFresnel", "Cook-Torrance (Diffuse Fresnel)" },
        { "BlinnPhongSeparateDiffuseFresnel",   "Blinn-Phong (Diffuse Fresnel)" },
    };
    return kNames;
}

QString PbrMaterial::brdfEngineName(int index)
{
    const auto &names = brdfNames();
    if (index < 0 || index >= names.size()) return QStringLiteral("Default");
    return QString::fromLatin1(names[index].engineName);
}

bool PbrMaterial::brdfSupportsClearCoat(int index)
{
    // The renderer's BRDF value is (family | modifier bits); the clear-coat
    // path is gated on the FAMILY being Default, so the diffuse-fresnel
    // variants of Default qualify and Cook-Torrance / Blinn-Phong do not.
    // Expressed on the names because the bit layout is the renderer's, not
    // the document's.
    return brdfEngineName(index).startsWith(QStringLiteral("Default"));
}

// The shading-model vocabulary. Two values today; the index is the stored value
// and is permanent, exactly like brdfNames().
const QVector<const char *> &PbrMaterial::shadingModelNames()
{
    static const QVector<const char *> kNames = { "Lit", "Unlit", "Distortion" };
    return kNames;
}

// The workflow vocabulary. Index is the stored value and is permanent, exactly
// like brdfNames(). "Specular (Fresnel)" is what most PBRs simply call
// "specular" — the renderer's own header says so — so it is worth the longer
// label rather than shipping two rows a user cannot tell apart.
const QVector<const char *> &PbrMaterial::workflowNames()
{
    static const QVector<const char *> kNames = { "Metallic", "Specular",
                                                  "Specular (Fresnel)" };
    return kNames;
}

const char *PbrMaterial::sharedMapDisplayName(int workflow)
{
    return workflow == 0 ? "Metallic Map" : "Specular Map";
}

bool PbrMaterial::sharedMapIsSrgb(int workflow)
{
    // Metalness is DATA (linear); a specular map is a COLOUR (sRGB). The
    // renderer's suggestUsingSRGB agrees, and because the two are one texture
    // unit the same file can legitimately be wanted in both colour spaces —
    // which is why both texture caches key on the sRGB flag now
    // (MATERIAL_GAPS_SPEC I-2).
    return workflow != 0;
}

// The fresnel rows are not read at all in the metallic workflow (metalness and
// F0 share one float in the renderer, and applyPbr writes exactly one of them),
// and the metallic factor is not read in the two specular workflows. Grey them
// out rather than let a user drag a slider that reaches nothing. The VALUES are
// untouched either way, so switching workflow restores them.
const QVector<QString> &PbrMaterial::rowsUnusedWhenMetallic()
{
    static const QVector<QString> kRows = {
        QStringLiteral("ior"), QStringLiteral("fresnelColor"),
        QStringLiteral("useFresnelColor"), QStringLiteral("separateFresnel"),
    };
    return kRows;
}

const QVector<QString> &PbrMaterial::rowsUnusedWhenSpecular()
{
    static const QVector<QString> kRows = { QStringLiteral("metallic") };
    return kRows;
}

// WHAT UNLIT CANNOT DO, as a list the UI can act on rather than prose a user has
// to discover. Each entry is a renderer fact, not a policy:
//   metallic/roughness/normal/emissive + their maps  no lighting term consumes
//                                                    them; the Unlit family has
//                                                    no such inputs at all
//   roughness bounds                                 remap of a value nothing reads
//   brdf, clearCoat, clearCoatRoughness              a BRDF is the lighting model
//   receiveShadows                                   nothing to receive INTO (an
//                                                    unlit item still occludes
//                                                    the shadow map — it casts)
//   textureScale                                     UV tiling rides a shader
//                                                    piece belonging to the PBS
//                                                    family; the Unlit family's
//                                                    animation-matrix equivalent
//                                                    is deliberately out of v1
//                                                    (decision D-P4a)
// The VALUES are untouched — the panel greys the rows, the document keeps them,
// and switching back to Lit restores every one.
const QVector<QString> &PbrMaterial::rowsUnusedWhenUnlit()
{
    static const QVector<QString> kRows = {
        QStringLiteral("metallic"), QStringLiteral("roughness"),
        QStringLiteral("roughnessLowerBound"), QStringLiteral("roughnessUpperBound"),
        QStringLiteral("normalFactor"),
        QStringLiteral("emissiveColor"), QStringLiteral("emissiveIntensity"),
        QStringLiteral("brdf"), QStringLiteral("clearCoat"), QStringLiteral("clearCoatRoughness"),
        QStringLiteral("receiveShadows"), QStringLiteral("emissiveAsLightmap"),
        QStringLiteral("textureScale"), QStringLiteral("textureScaleV"),
        QStringLiteral("textureOffsetU"), QStringLiteral("textureOffsetV"),
        QStringLiteral("textureRotation"),
        QStringLiteral("normalMap"), QStringLiteral("metallicMap"),
        QStringLiteral("roughnessMap"), QStringLiteral("emissiveMap"),
            "reflectionMap",   // the engine's unlit path returns before the reflection branch (code review 2026-09-10)
    };
    return kRows;
}

// WHAT DISTORTION CANNOT DO (SPECS/POST_LOOKS_SPEC.md §5.2). A far shorter list
// than Unlit's, because a distortion material has no SURFACE at all: it draws
// no colour anywhere, it writes a screen-space displacement field that the
// renderer then warps the image behind it by. THREE rows survive, and they are
// the whole authoring surface:
//   normalMap       the displacement field — a tangent-space normal map IS one
//                   once R and G are read as a signed offset, which is why the
//                   existing picker is reused rather than a sixth slot invented
//   opacity         the material's own STRENGTH, multiplied by the world's
//                   Distortion Strength
//   twoSided        as always: whether the back faces of the volume displace too
// Everything else — the base colour and its map, every PBR term, the BRDF, the
// coat, shadows, tiling — is untouched on the document and greyed in the panel.
const QVector<QString> &PbrMaterial::rowsUnusedWhenDistortion()
{
    static const QVector<QString> kRows = [] {
        QVector<QString> rows = rowsUnusedWhenUnlit();
        // ...minus the normal map, which is the one thing Distortion DOES read.
        rows.removeAll(QStringLiteral("normalMap"));
        rows.removeAll(QStringLiteral("normalFactor"));
        // ...plus everything an unlit surface still shows and this does not.
        rows << QStringLiteral("baseColor") << QStringLiteral("baseColorFactor")
             << QStringLiteral("baseColorMap") << QStringLiteral("alphaMode")
             << QStringLiteral("alphaCutoff") << QStringLiteral("castShadows");
        return rows;
    }();
    return kRows;
}

void PbrMaterial::setEmissiveColor(QColor color)        { emissiveColor = color; }
void PbrMaterial::setEmissiveIntensity(float intensity) { emissiveIntensity = intensity; }

void PbrMaterial::setEmissiveMap(Texture2DPtr tex)
{
    if (!!tex) { useEmissiveMap = true;  addTexture("u_emissiveMap", tex); }
    else       { useEmissiveMap = false; removeTexture("u_emissiveMap"); }
}

// ADDENDUM A-5. One more entry in `textures` under the sampler name the
// mirror's slot table reads — the whole point of routing it through the same
// map is that the override inherits the resolve, the cache, the reclaim and
// the family switch every other map already has.
void PbrMaterial::setReflectionMap(Texture2DPtr tex)
{
    if (!!tex) { useReflectionMap = true;  addTexture("u_reflectionMap", tex); }
    else       { useReflectionMap = false; removeTexture("u_reflectionMap"); }
}

void PbrMaterial::setAlpha(float a)          { alpha = a; }
void PbrMaterial::setAlphaCutoff(float c)    { alphaCutoff = c; }
void PbrMaterial::setAlphaMode(int mode)     { alphaMode = mode; }
void PbrMaterial::setRefractionStrength(float s) { refractionStrength = s; }
// The UNIFORM overload sets both axes: it is the one every caller written
// before the V axis existed uses, and it has to keep meaning "tile the whole
// thing this much" — including MaterialReader, which drives a loaded
// "textureScale" number through setValue.
void PbrMaterial::setTextureScale(float s)   { setTextureScale(s, s); }
void PbrMaterial::setTextureScale(float u, float v) {
    textureScale = u; textureScaleV = v;
    // BOTH rows, always (SMOKE_FIX S11 — see setValue's note): the rows are
    // what gets saved, and a V row left behind is a floor that squashes on
    // reopen.
    syncProperty(QStringLiteral("textureScale"), u);
    syncProperty(QStringLiteral("textureScaleV"), v);
}
void PbrMaterial::setTextureOffset(float u, float v) { textureOffsetU = u; textureOffsetV = v; }
void PbrMaterial::setTextureRotation(float degrees)  { textureRotation = degrees; }

// The generated-piece paths are stored, never opened here: the renderer's
// boundary registers the file's directory with its resource system and reads
// it, and nothing in the document model has any business parsing shader source.
void PbrMaterial::setCustomPiecePixel(const QString& path)  { customPiecePixel = path; }
void PbrMaterial::setCustomPieceVertex(const QString& path) { customPieceVertex = path; }


// Empty path clears the slot; a missing file yields a null texture, which the
// set*Map functions treat as "no map" rather than failing.
Texture2DPtr PbrMaterial::loadTexture(const QString& path)
{
    if (path.isEmpty()) return Texture2DPtr();
    // A LIVE REFERENCE ("live://<guid>") names pixels a producer owns for this
    // session, not a file (MATERIAL_GAPS_SPEC A-1). A MISS IS ORDINARY and is
    // not an error: it is what a scene saved by some other session, or a row
    // hand-edited into a file, looks like — the slot simply stays empty, which
    // is the same answer a missing file gets, and the material renders its
    // base colour instead of crashing on a guid nobody can resolve.
    if (LiveTextures::isLiveRef(path)) {
        Texture2DPtr live = LiveTextures::find(path);
        if (!live)
            irisLog("live texture not in this session, slot left empty: " + path);
        return live;
    }
    return Texture2D::load(path);
}

void PbrMaterial::setValue(const QString& name, const QVariant& value)
{
    if      (name == "baseColor")         baseColor         = value.value<QColor>();
    else if (name == "metallic")          metallicFactor    = value.toFloat();
    else if (name == "roughness")         roughnessFactor   = value.toFloat();
    else if (name == "normalFactor")      normalFactor      = value.toFloat();
    else if (name == "emissiveColor")     emissiveColor     = value.value<QColor>();
    else if (name == "emissiveIntensity") emissiveIntensity = value.toFloat();
    else if (name == "alpha")             alpha             = value.toFloat();
    // "textureScale" sets BOTH axes (see setTextureScale): a file or a script
    // that only knows the old key means uniform tiling. "textureScaleV" then
    // overrides V — and because both readers walk `properties` IN ORDER and V
    // is registered after U, a new file's explicit pair always lands correctly.
    else if (name == "textureScale")      setTextureScale(value.toFloat());
    else if (name == "textureScaleV")     textureScaleV     = value.toFloat();
    else if (name == "textureOffsetU")    textureOffsetU    = value.toFloat();
    else if (name == "textureOffsetV")    textureOffsetV    = value.toFloat();
    else if (name == "textureRotation")   textureRotation   = value.toFloat();
    else if (name == "roughnessLowerBound") roughnessLowerBound = value.toFloat();
    else if (name == "roughnessUpperBound") roughnessUpperBound = value.toFloat();
    else if (name == "alphaCutoff")       alphaCutoff       = value.toFloat();
    else if (name == "alphaMode")         alphaMode         = value.toInt();
    else if (name == "refractionStrength") refractionStrength = value.toFloat();
    else if (name == "clearCoat")          clearCoat          = value.toFloat();
    else if (name == "clearCoatRoughness") clearCoatRoughness = value.toFloat();
    else if (name == "shadingModel")       shadingModel       = value.toInt();
    else if (name == "workflow")           workflow           = value.toInt();
    else if (name == "specularColor")      specularColor      = value.value<QColor>();
    else if (name == "ior")                ior                = value.toFloat();
    else if (name == "fresnelColor")       fresnelColor       = value.value<QColor>();
    else if (name == "useFresnelColor")    useFresnelColor    = value.toBool();
    else if (name == "separateFresnel")    separateFresnel    = value.toBool();
    else if (name == "detailWeightMap")    setDetailWeightMap(loadTexture(value.toString()));
    else if (name == "anisotropy") {
        // THE ROW IS AN ENUM ("Off", "2x", "4x", "8x", "16x") and its stored
        // value is the INDEX; the field is the COUNT the renderer wants, which
        // is 1 << index. Keeping the row an enum rather than a free float is
        // deliberate: the renderer accepts only powers of two up to the
        // device's limit, and a slider that silently snapped would be exactly
        // the "does something else than it says" surface these rows exist to
        // avoid. Scripts write the NAME ("8x"); material.set refuses anything
        // that is not in the vocabulary rather than coercing it.
        anisotropy = float(1 << qBound(0, value.toInt(), 4));
    }
    else if (name.endsWith(QLatin1String("Address")) &&
             mapRowNames().contains(name.left(name.size() - 7)))
        mapAddress[name.left(name.size() - 7)] = value.toInt();
    else if (name == "brdf")               brdf               = value.toInt();
    else if (name == "receiveShadows")     receiveShadows     = value.toBool();
    else if (name == "emissiveAsLightmap") emissiveAsLightmap = value.toBool();
    else if (name == "customPiecePixel")   customPiecePixel   = value.toString();
    else if (name == "customPieceVertex")  customPieceVertex  = value.toString();

    // Texture properties arrive as a path, matching how CustomMaterial::setValue
    // is driven from the material presets.
    else if (name == "baseColorMap")  setBaseColorMap(loadTexture(value.toString()));
    else if (name == "metallicMap")   setMetallicMap(loadTexture(value.toString()));
    else if (name == "roughnessMap")  setRoughnessMap(loadTexture(value.toString()));
    else if (name == "normalMap")     setNormalMap(loadTexture(value.toString()));
    else if (name == "emissiveMap")   setEmissiveMap(loadTexture(value.toString()));
    else if (name == "reflectionMap") setReflectionMap(loadTexture(value.toString()));

    // ---- detail layers: flat rows, one branch (MATERIAL_GAPS_SPEC §3.4) ----
    // `detail<N><Suffix>` for N in [0, kDetailLayers). Parsed rather than
    // enumerated so raising kDetailLayers needs no edit here.
    else if (name.startsWith(QLatin1String("detail")) && name.size() > 7 &&
             name.at(6).isDigit()) {
        const int layer = name.at(6).digitValue();
        const QString suffix = name.mid(7);
        if (layer >= 0 && layer < kDetailLayers) {
            DetailLayer &d = detail[layer];
            if      (suffix == QLatin1String("Map"))          setDetailMap(layer, loadTexture(value.toString()));
            else if (suffix == QLatin1String("NormalMap"))    setDetailNormalMap(layer, loadTexture(value.toString()));
            else if (suffix == QLatin1String("Blend"))        d.blend        = value.toInt();
            else if (suffix == QLatin1String("OffsetU"))      d.offsetU      = value.toFloat();
            else if (suffix == QLatin1String("OffsetV"))      d.offsetV      = value.toFloat();
            else if (suffix == QLatin1String("ScaleU"))       d.scaleU       = value.toFloat();
            else if (suffix == QLatin1String("ScaleV"))       d.scaleV       = value.toFloat();
            else if (suffix == QLatin1String("Weight"))       d.weight       = value.toFloat();
            else if (suffix == QLatin1String("NormalWeight")) d.normalWeight = value.toFloat();
        }
    }

    // KEEP THE PROPERTY ROWS IN STEP WITH THE FIELDS — every field this call
    // touched, not just the row that was named (SMOKE_FIX S11).
    //
    // The rows are what SceneWriter serializes (scenewriter.cpp's values loop),
    // so a row that disagrees with its field is a scene that reopens wrong.
    // "textureScale" writes BOTH axes (setTextureScale above, which now syncs
    // both rows itself), and syncing only the named row left `textureScaleV` at
    // 1 in the file while the live material tiled 4x4: every scene built by
    // createDefaultScene saved a (4, 1) floor and reopened with the checkers
    // squashed along V. A caller that wants non-uniform tiling still gets it —
    // it sets "textureScaleV" after "textureScale", which is the order both
    // readers already apply.
    syncProperty(name, value);
}

MaterialPtr PbrMaterial::duplicate() const
{
    // The member-wise copy carries every FIELD — colours, factors, the five
    // maps and the detail layers (the `textures` map of shared handles), the
    // UV transform, the address modes, the render layer and states, the name
    // and the preset/asset guid the material came from — in one line that
    // cannot forget a field added next year.
    PbrMaterialPtr copy(new PbrMaterial(*this));
    // ...and SHARES the rows, which are the one thing it must not share: they
    // are what the panel edits and what SceneWriter saves. Fresh rows, then
    // this material's row values onto them BY NAME, so a row whose value is
    // not derivable from a field (a map row's path) survives too.
    copy->properties.clear();
    copy->createProperties();
    for (Property *row : copy->properties) {
        for (Property *mine : properties) {
            if (mine->name == row->name) { row->setValue(mine->getValue()); break; }
        }
    }
    return copy;
}

void PbrMaterial::syncProperty(const QString& name, const QVariant& value)
{
    for (auto prop : properties)
        if (prop->name == name) { prop->setValue(value); return; }
}

// ---------------------------------------------------------------- properties
//
// The panel renders these by PropertyType. Note that declaring them is NOT
// sufficient on its own - setValue() above is what carries an edited value onto
// the field the shader actually reads.

void PbrMaterial::createProperties()
{
    int id = 0;

    auto colProp         = new ColorProperty;
    colProp->id          = id++;
    colProp->displayName = "Base Color";
    colProp->name        = "baseColor";
    colProp->value       = baseColor;
    properties.append(colProp);

    auto metallicProp         = new FloatProperty;
    metallicProp->id          = id++;
    metallicProp->displayName = "Metallic";
    metallicProp->name        = "metallic";
    metallicProp->minValue    = 0.0f;
    metallicProp->maxValue    = 1.0f;
    metallicProp->value       = metallicFactor;
    properties.append(metallicProp);

    auto roughnessProp         = new FloatProperty;
    roughnessProp->id          = id++;
    roughnessProp->displayName = "Roughness";
    roughnessProp->name        = "roughness";
    roughnessProp->minValue    = 0.0f;
    roughnessProp->maxValue    = 1.0f;
    roughnessProp->value       = roughnessFactor;
    properties.append(roughnessProp);

    auto normalProp         = new FloatProperty;
    normalProp->id          = id++;
    normalProp->displayName = "Normal Intensity";
    normalProp->name        = "normalFactor";
    normalProp->minValue    = 0.0f;
    normalProp->maxValue    = 2.0f;
    normalProp->value       = normalFactor;
    properties.append(normalProp);

    auto emissiveColProp         = new ColorProperty;
    emissiveColProp->id          = id++;
    emissiveColProp->displayName = "Emissive Color";
    emissiveColProp->name        = "emissiveColor";
    emissiveColProp->value       = emissiveColor;
    properties.append(emissiveColProp);

    auto emissiveProp         = new FloatProperty;
    emissiveProp->id          = id++;
    emissiveProp->displayName = "Emissive Intensity";
    emissiveProp->name        = "emissiveIntensity";
    emissiveProp->minValue    = 0.0f;
    emissiveProp->maxValue    = 10.0f;
    emissiveProp->value       = emissiveIntensity;
    properties.append(emissiveProp);

    auto alphaProp         = new FloatProperty;
    alphaProp->id          = id++;
    alphaProp->displayName = "Alpha";
    alphaProp->name        = "alpha";
    alphaProp->minValue    = 0.0f;
    alphaProp->maxValue    = 1.0f;
    alphaProp->value       = alpha;
    properties.append(alphaProp);

    // The UV transform rows. ORDER MATTERS: "textureScale" sets both axes, so
    // it has to be read before "textureScaleV" overrides V — both material
    // readers iterate this list in order (SceneReader::readPbrMaterial,
    // MaterialReader::parsePbrMaterial).
    auto scaleProp         = new FloatProperty;
    scaleProp->id          = id++;
    scaleProp->displayName = "Texture Scale U";
    scaleProp->name        = "textureScale";
    scaleProp->minValue    = 0.0f;
    scaleProp->maxValue    = 10.0f;
    scaleProp->value       = textureScale;
    properties.append(scaleProp);

    auto scaleVProp         = new FloatProperty;
    scaleVProp->id          = id++;
    scaleVProp->displayName = "Texture Scale V";
    scaleVProp->name        = "textureScaleV";
    scaleVProp->minValue    = 0.0f;
    scaleVProp->maxValue    = 10.0f;
    scaleVProp->value       = textureScaleV;
    properties.append(scaleVProp);

    auto offsetUProp         = new FloatProperty;
    offsetUProp->id          = id++;
    offsetUProp->displayName = "Texture Offset U";
    offsetUProp->name        = "textureOffsetU";
    offsetUProp->minValue    = -10.0f;
    offsetUProp->maxValue    = 10.0f;
    offsetUProp->value       = textureOffsetU;
    properties.append(offsetUProp);

    auto offsetVProp         = new FloatProperty;
    offsetVProp->id          = id++;
    offsetVProp->displayName = "Texture Offset V";
    offsetVProp->name        = "textureOffsetV";
    offsetVProp->minValue    = -10.0f;
    offsetVProp->maxValue    = 10.0f;
    offsetVProp->value       = textureOffsetV;
    properties.append(offsetVProp);

    auto rotationProp         = new FloatProperty;
    rotationProp->id          = id++;
    rotationProp->displayName = "Texture Rotation";
    rotationProp->name        = "textureRotation";
    rotationProp->minValue    = -360.0f;
    rotationProp->maxValue    = 360.0f;
    rotationProp->value       = textureRotation;
    properties.append(rotationProp);

    // Remap bounds for a sampled roughness map (see the field comment in the
    // header: lower > upper deliberately inverts a legacy gloss map).
    auto roughLowerProp         = new FloatProperty;
    roughLowerProp->id          = id++;
    roughLowerProp->displayName = "Roughness Lower Bound";
    roughLowerProp->name        = "roughnessLowerBound";
    roughLowerProp->minValue    = 0.0f;
    roughLowerProp->maxValue    = 1.0f;
    roughLowerProp->value       = roughnessLowerBound;
    properties.append(roughLowerProp);

    auto roughUpperProp         = new FloatProperty;
    roughUpperProp->id          = id++;
    roughUpperProp->displayName = "Roughness Upper Bound";
    roughUpperProp->name        = "roughnessUpperBound";
    roughUpperProp->minValue    = 0.0f;
    roughUpperProp->maxValue    = 1.0f;
    roughUpperProp->value       = roughnessUpperBound;
    properties.append(roughUpperProp);

    auto cutoffProp         = new FloatProperty;
    cutoffProp->id          = id++;
    cutoffProp->displayName = "Alpha Cutoff";
    cutoffProp->name        = "alphaCutoff";
    cutoffProp->minValue    = 0.0f;
    cutoffProp->maxValue    = 1.0f;
    cutoffProp->value       = alphaCutoff;
    properties.append(cutoffProp);

    // 0 opaque, 1 cutout/masked, 2 blend/translucent (glTF's OPAQUE/MASK/BLEND),
    // 3 glass (engine realistic transparency), 4 additive (Src+Dest),
    // 5 modulate (Src×Dest) — the Unreal-parity blend modes — and
    // 6 refractive (glass that BENDS the background; needs the viewport's
    // refraction pass, POST_CHAIN_SPEC.md phase 7).
    //
    // A ListProperty (the generic ENUM row), not an IntProperty with a
    // hardcoded name branch in the panel: the labels ride the row, so the
    // picker cannot disagree with the vocabulary (PUBLISH_AUDIT #4 was exactly
    // that disagreement). ON DISK IT IS STILL AN INT — the enum row's value IS
    // the index — so existing scenes and .material files load unchanged.
    auto alphaModeProp         = new ListProperty;
    alphaModeProp->id          = id++;
    alphaModeProp->displayName = "Alpha Mode";
    alphaModeProp->name        = "alphaMode";
    alphaModeProp->labels      = { "Opaque", "Masked", "Translucent", "Glass",
                                   "Additive", "Modulate", "Refractive" };
    alphaModeProp->value       = alphaMode;
    properties.append(alphaModeProp);

    auto refractProp         = new FloatProperty;
    refractProp->id          = id++;
    refractProp->displayName = "Refraction Strength";
    refractProp->name        = "refractionStrength";
    refractProp->minValue    = 0.0f;
    refractProp->maxValue    = 1.0f;
    refractProp->value       = refractionStrength;
    properties.append(refractProp);

    // ---- MATERIAL_GAPS_SPEC GAP 1: the workflow ----
    // Before the shading model, because it renames a MAP row ("Metallic Map"
    // <-> "Specular Map", sharedMapDisplayName) and greys three others.
    auto workflowProp         = new ListProperty;
    workflowProp->id          = id++;
    workflowProp->displayName = "Workflow";
    workflowProp->name        = "workflow";
    for (const char *n : workflowNames()) workflowProp->labels << QString::fromLatin1(n);
    workflowProp->value       = workflow;
    properties.append(workflowProp);

    // kS. Meaningful in EVERY workflow (white = inert), so it is not on either
    // unused-rows list.
    auto specColProp         = new ColorProperty;
    specColProp->id          = id++;
    specColProp->displayName = "Specular Color";
    specColProp->name        = "specularColor";
    specColProp->value       = specularColor;
    properties.append(specColProp);

    // 1.0 (vacuum, F0 = 0) to 3.0 (diamond is 2.42): the authoring range that
    // covers every real dielectric. F0 = ((1-ior)/(1+ior))^2.
    auto iorProp         = new FloatProperty;
    iorProp->id          = id++;
    iorProp->displayName = "Index of Refraction";
    iorProp->name        = "ior";
    iorProp->minValue    = 1.0f;
    iorProp->maxValue    = 3.0f;
    iorProp->value       = ior;
    properties.append(iorProp);

    auto useFresnelProp         = new BoolProperty;
    useFresnelProp->id          = id++;
    useFresnelProp->displayName = "Use Fresnel Color";
    useFresnelProp->name        = "useFresnelColor";
    useFresnelProp->value       = useFresnelColor;
    properties.append(useFresnelProp);

    auto fresnelColProp         = new ColorProperty;
    fresnelColProp->id          = id++;
    fresnelColProp->displayName = "Fresnel Color (F0)";
    fresnelColProp->name        = "fresnelColor";
    fresnelColProp->value       = fresnelColor;
    properties.append(fresnelColProp);

    // Not a slider: flipping it changes the size of the renderer's fresnel term
    // and recompiles the material's shader.
    auto sepFresnelProp         = new BoolProperty;
    sepFresnelProp->id          = id++;
    sepFresnelProp->displayName = "Per-Channel Fresnel";
    sepFresnelProp->name        = "separateFresnel";
    sepFresnelProp->value       = separateFresnel;
    properties.append(sepFresnelProp);

    // ---- HLMS_ADOPTION P4a: the shading model ----
    // FIRST of the shading rows, because it constrains more of the panel than
    // anything else does: on Unlit, thirteen of the rows below have nothing to
    // reach (rowsUnusedWhenUnlit) and the panel greys them out.
    auto shadingProp         = new ListProperty;
    shadingProp->id          = id++;
    shadingProp->displayName = "Shading Model";
    shadingProp->name        = "shadingModel";
    for (const char *n : shadingModelNames()) shadingProp->labels << QString::fromLatin1(n);
    shadingProp->value       = shadingModel;
    properties.append(shadingProp);

    // ---- HLMS_ADOPTION P1: the cheap PBS knobs ----
    // The BRDF picker comes FIRST because it constrains the two coat rows: the
    // renderer can only carry a clear coat on the Default family, so the panel
    // disables them on any other pick (D-P1b) and the coat values are kept, not
    // destroyed.
    auto brdfProp         = new ListProperty;
    brdfProp->id          = id++;
    brdfProp->displayName = "Shading BRDF";
    brdfProp->name        = "brdf";
    for (const auto &n : brdfNames()) brdfProp->labels << QString::fromLatin1(n.displayName);
    brdfProp->value       = brdf;
    properties.append(brdfProp);

    auto coatProp         = new FloatProperty;
    coatProp->id          = id++;
    coatProp->displayName = "Clear Coat";
    coatProp->name        = "clearCoat";
    coatProp->minValue    = 0.0f;
    coatProp->maxValue    = 1.0f;
    coatProp->value       = clearCoat;
    properties.append(coatProp);

    auto coatRoughProp         = new FloatProperty;
    coatRoughProp->id          = id++;
    coatRoughProp->displayName = "Clear Coat Roughness";
    coatRoughProp->name        = "clearCoatRoughness";
    coatRoughProp->minValue    = 0.0f;
    coatRoughProp->maxValue    = 1.0f;
    coatRoughProp->value       = clearCoatRoughness;
    properties.append(coatRoughProp);

    // NAMED "receiveShadows" AND NOTHING ELSE. The materials graph used to ship
    // a `receiveShadow` (singular) checkbox in MaterialSettings that reached
    // nothing at all; that ghost is deleted. This row is the real one.
    auto receiveShadowsProp         = new BoolProperty;
    receiveShadowsProp->id          = id++;
    receiveShadowsProp->displayName = "Receive Shadows";
    receiveShadowsProp->name        = "receiveShadows";
    receiveShadowsProp->value       = receiveShadows;
    properties.append(receiveShadowsProp);

    // Only meaningful with an emissive map bound, and the emissive colour
    // should be white or it tints the lightmap.
    auto lightmapProp         = new BoolProperty;
    lightmapProp->id          = id++;
    lightmapProp->displayName = "Emissive as Lightmap";
    lightmapProp->name        = "emissiveAsLightmap";
    lightmapProp->value       = emissiveAsLightmap;
    properties.append(lightmapProp);

    // The six texture maps. Names match the setValue() cases above (paths, not
    // the "u_*Map" sampler names used as Material::textures keys). Declaring
    // them is what makes SceneWriter persist the maps and SceneReader restore
    // them — without these, PBR texture maps did not survive a scene save/load.
    struct MapDef { const char *display; const char *name; };
    static const MapDef kMaps[] = {
        { "Base Color Map", "baseColorMap" },
        { "Normal Map",     "normalMap"    },
        // The display name here is the METALLIC-workflow one; the panel
        // relabels this row from sharedMapDisplayName() when the workflow is
        // Specular (one renderer texture unit, two meanings — I-4).
        { "Metallic Map",   "metallicMap"  },
        { "Roughness Map",  "roughnessMap" },
        { "Emissive Map",   "emissiveMap"  },
    };
    for (const auto &m : kMaps) {
        auto texProp         = new TextureProperty;
        texProp->id          = id++;
        texProp->displayName = m.display;
        texProp->name        = m.name;
        properties.append(texProp);
    }

    // ---- ADDENDUM A-5: the per-material reflection cubemap ---------------
    // A row like any other map row, so the panel, the writer, the reader and
    // material.set all get it for free — but deliberately NOT in mapRowNames(),
    // which generates the addressing rows: a cube is sampled by direction and
    // has no U/V wrap to vary. Empty by default, at which the material samples
    // the scene's global reflection exactly as it always has.
    auto reflectionProp         = new TextureProperty;
    reflectionProp->id          = id++;
    reflectionProp->displayName = "Reflection Cubemap";
    reflectionProp->name        = "reflectionMap";
    properties.append(reflectionProp);

    // ---- MATERIAL_GAPS_SPEC GAP 2: the detail layers ---------------------
    // FLAT ROWS, one group per layer, generated from kDetailLayers so raising
    // the count is a constant change. Every default here is the renderer's own
    // no-op value: no map, blend index 0, offset (0,0), scale (1,1), weights 1
    // — at which the renderer sets NO shader property for the layer at all, so
    // this section existing cannot move an existing material's pixels.
    for (int layer = 0; layer < kDetailLayers; ++layer) {
        const QString label = QStringLiteral("Detail %1 ").arg(layer);

        auto mapProp         = new TextureProperty;
        mapProp->id          = id++;
        mapProp->displayName = label + QStringLiteral("Map");
        mapProp->name        = detailRow(layer, "Map");
        properties.append(mapProp);

        auto nmProp         = new TextureProperty;
        nmProp->id          = id++;
        nmProp->displayName = label + QStringLiteral("Normal Map");
        nmProp->name        = detailRow(layer, "NormalMap");
        properties.append(nmProp);

        auto blendProp         = new ListProperty;
        blendProp->id          = id++;
        blendProp->displayName = label + QStringLiteral("Blend");
        blendProp->name        = detailRow(layer, "Blend");
        for (const char *n : detailBlendNames()) blendProp->labels << QString::fromLatin1(n);
        blendProp->value       = detail[layer].blend;
        properties.append(blendProp);

        // Offset and scale are FOUR float rows rather than two Vec2s: the panel
        // renders Vec2 rows, but material.set, the reader and the graph baker
        // all address rows by a single name with a scalar value, and a Vec2 key
        // would be the one row a script could not write like the others.
        struct Scalar { const char *suffix; const char *label; float value; float lo; float hi; };
        const Scalar scalars[] = {
            { "OffsetU",      "Offset U",      detail[layer].offsetU,      -8.0f, 8.0f },
            { "OffsetV",      "Offset V",      detail[layer].offsetV,      -8.0f, 8.0f },
            { "ScaleU",       "Scale U",       detail[layer].scaleU,        0.0f, 32.0f },
            { "ScaleV",       "Scale V",       detail[layer].scaleV,        0.0f, 32.0f },
            { "Weight",       "Weight",        detail[layer].weight,        0.0f, 1.0f },
            { "NormalWeight", "Normal Weight", detail[layer].normalWeight,  0.0f, 2.0f },
        };
        for (const Scalar &sc : scalars) {
            auto p         = new FloatProperty;
            p->id          = id++;
            p->displayName = label + QString::fromLatin1(sc.label);
            p->name        = detailRow(layer, sc.suffix);
            p->minValue    = sc.lo;
            p->maxValue    = sc.hi;
            p->value       = sc.value;
            properties.append(p);
        }
    }

    // ONE mask for every layer: its R/G/B/A channels scale layers 0/1/2/3.
    // That is the renderer's shape (PBSM_DETAIL_WEIGHT), not a simplification.
    auto detailWeightProp         = new TextureProperty;
    detailWeightProp->id          = id++;
    detailWeightProp->displayName = "Detail Weight Map";
    detailWeightProp->name        = "detailWeightMap";
    properties.append(detailWeightProp);

    // ---- ADDENDUM A-2: sampler control -----------------------------------
    // One anisotropy dial for the material, and one addressing row per MAP —
    // generated from mapRowNames() so a new map slot gets its address row for
    // free. Every default is the value the maps were hard-coded to, so this
    // section existing cannot change how an existing material samples.
    auto anisoProp         = new ListProperty;
    anisoProp->id          = id++;
    anisoProp->displayName = "Anisotropic Filtering";
    anisoProp->name        = "anisotropy";
    anisoProp->labels      = { "Off", "2x", "4x", "8x", "16x" };
    // Stored as the COUNT (1/2/4/8/16) and shown as a label, so the row's
    // value is the number the renderer wants. The list index is log2.
    {
        int idx = 0;
        for (int i = 0, v = 1; i < 5; ++i, v *= 2) if (int(anisotropy) >= v) idx = i;
        anisoProp->value = idx;
    }
    properties.append(anisoProp);

    for (const QString &mapRow : mapRowNames()) {
        auto addrProp         = new ListProperty;
        addrProp->id          = id++;
        // "Base Color Map Address" would be a fifth column of noise; the row
        // sits directly under its map and says only what it varies.
        addrProp->displayName = QStringLiteral("  Address");
        addrProp->name        = addressRow(mapRow);
        for (const char *n : addressModeNames()) addrProp->labels << QString::fromLatin1(n);
        addrProp->value       = addressFor(mapRow);
        properties.append(addrProp);
    }
}

}
