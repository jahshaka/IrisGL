// Materials (PBR, unlit, outline), textures and the mesh/material attachment
// verbs that bind them onto a node.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

Ogre::uint8 OgreScene::renderQueueFor(const MaterialRec &m) {
    if (m.onTop)      return kOverlayRenderQueue;      // gizmos, wires, always-on-top
    if (m.refractive) return kRefractiveRenderQueue;   // the chain's refraction pass
    return 10u;                                        // Ogre's default for normal items
}

void OgreScene::refileItems(MaterialId id, const MaterialRec &m) {
    const Ogre::uint8 rq = renderQueueFor(m);
    for (auto &kv : mNodes)
        if (kv.second.materialRef == id && kv.second.item)
            kv.second.item->setRenderQueueGroup(rq);
}

Ogre::Hlms *OgreScene::hlmsFor(const MaterialRec &m) const {
    // `unlit` is a POLICY flag (an overlay: no GI, its own render queue), not a
    // statement about which Hlms owns the datablock. The skinned selection
    // silhouette is unlit in effect but built on HlmsPbs, because HlmsUnlit
    // cannot skin — see createOutlineMaterial.
    const bool pbs = !m.unlit || m.pbsBacked;
    return mRoot->getHlmsManager()->getHlms(pbs ? Ogre::HLMS_PBS : Ogre::HLMS_UNLIT);
}

void OgreScene::setRefractionsActive(bool active) {
    if (active == mRefractionsActive) return;
    mRefractionsActive = active;
    JAH_TRY {
        auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        for (auto &kv : mMaterials) {
            if (!kv.second.refractive || kv.second.unlit) continue;
            auto *db = static_cast<Ogre::HlmsPbsDatablock *>(
                hlmsPbs->getDatablock(Ogre::IdString(kv.second.datablockName)));
            if (!db) continue;
            db->setTransparency(db->getTransparency(),
                                active ? Ogre::HlmsPbsDatablock::Refractive
                                       : Ogre::HlmsPbsDatablock::Transparent);
        }
    } JAH_CATCH(mError, );
}

// The six BRDFs we expose, by NAME (PbrParams::brdf). Ogre's PbsBrdf values are
// a bitfield — BRDF_MASK selects the family, the high bits are modifiers — and
// nothing outside this file should ever see one. Six of the pin's twelve named
// values: the three families, plain and with SEPARATE diffuse fresnel (the
// variant Ogre documents for glass, transparent plastics, fur and marbles).
// The remaining six are uncorrelated/legacy-math combinations; offering twelve
// rows of jargon is worse product than six (HLMS_ADOPTION_SPEC D-P1a).
//
// An unknown name is Default, NOT an error: a document written by a newer build
// must still open, and a material silently falling back to the physically
// accurate BRDF is the safe direction.
static Ogre::PbsBrdf::PbsBrdf brdfFromName(const std::string &name, bool *known = nullptr) {
    struct Row { const char *name; Ogre::PbsBrdf::PbsBrdf value; };
    static const Row kRows[] = {
        { "Default",                            Ogre::PbsBrdf::Default },
        { "CookTorrance",                       Ogre::PbsBrdf::CookTorrance },
        { "BlinnPhong",                         Ogre::PbsBrdf::BlinnPhong },
        { "DefaultSeparateDiffuseFresnel",      Ogre::PbsBrdf::DefaultSeparateDiffuseFresnel },
        { "CookTorranceSeparateDiffuseFresnel", Ogre::PbsBrdf::CookTorranceSeparateDiffuseFresnel },
        { "BlinnPhongSeparateDiffuseFresnel",   Ogre::PbsBrdf::BlinnPhongSeparateDiffuseFresnel },
    };
    for (const Row &r : kRows)
        if (name == r.name) { if (known) *known = true; return r.value; }
    if (known) *known = false;
    return Ogre::PbsBrdf::Default;
}

static void warnUnknownBrdfOnce(const std::string &name) {
    static std::set<std::string> warned;
    if (!warned.insert(name).second) return;
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka: unknown material brdf '" + name + "' — falling back to Default",
        Ogre::LML_CRITICAL);
}

void OgreScene::applyPbr(Ogre::HlmsPbsDatablock *db, const PbrParams &p,
                         bool refractionsActive) {
    db->setDiffuse(Ogre::Vector3(p.albedo.r, p.albedo.g, p.albedo.b));
    db->setMetalness(p.metalness);
    // Roughness 0 is reachable from the material slider, and HlmsPbs then logs
    // "Very low roughness values can cause NaNs in the pixel shader!" — once per
    // setRoughness, i.e. once per material PER FRAME while the mirror pushes.
    // The shader really can produce NaNs there too, so this is a clamp, not a
    // log suppression; 1e-4 sits an order of magnitude above Ogre's own 1e-6
    // warning threshold and is visually indistinguishable from a mirror.
    // The contract is stated on PbrParams::roughness in Types.h.
    db->setRoughness(std::max(p.roughness, 1e-4f));
    db->setEmissive(Ogre::Vector3(p.emissive.r, p.emissive.g, p.emissive.b));
    db->setNormalMapWeight(p.normalMapWeight);
    // UV tiling: HlmsPbs has no UV transform for its base maps (only detail maps
    // have offset/scale), so the scale rides in the datablock's user values and a
    // custom_ps_uv_modifier_macros piece (JahFog_piece_vs_piece_ps.any, a library
    // folder of HlmsPbs — no longer attached per datablock) multiplies every
    // base-map lookup by material.userValue[0].xy. setUserValue only schedules a
    // const-buffer update — scale edits never recompile shaders.
    db->setUserValue(0, Ogre::Vector4(p.uvScale, p.uvScale, 1.0f, 1.0f));
    // Manage the macroblock ourselves: setTwoSidedLighting(changeMacroblock=true)
    // swaps culling to CULL_NONE when enabling but never restores it when
    // disabling, and applyPbr must be idempotent in both directions.
    //
    // GUARDED, unlike every other setter here: HlmsPbsDatablock::setTwoSidedLighting
    // ends in an UNCONDITIONAL flushRenderables() (OgreHlmsPbsDatablock.cpp:787) —
    // it does not compare against mTwoSided first, the way setAlphaTest and
    // setTransparency compare against their own state. flushRenderables walks
    // every renderable using the datablock and recomputes its Hlms hash, so an
    // unconditional call from applyPbr made the mirror's per-frame
    // setPbrMaterial push a full hash recompute for every renderable, 60x a
    // second (deep audit 2026-09, area 5). The mirror now skips unchanged
    // pushes as well; this guard is the engine-side half, and it also protects
    // every other caller of applyPbr.
    if (db->getTwoSidedLighting() != p.twoSided) db->setTwoSidedLighting(p.twoSided, false);
    {
        Ogre::HlmsMacroblock macro = *db->getMacroblock();
        const Ogre::CullingMode want = p.twoSided ? Ogre::CULL_NONE : Ogre::CULL_CLOCKWISE;
        if (macro.mCullMode != want) { macro.mCullMode = want; db->setMacroblock(macro); }
    }
    // NOTE HlmsPbs has NO ambient-occlusion slot and no roughness remap:
    // roughness bounds are clamped by the caller before they reach here.
    //
    // ---- BRDF + clear coat (HLMS_ADOPTION P1) ----
    // ORDER IS LOAD-BEARING, in both directions:
    //  * setClearCoat asserts the datablock's BRDF family is Default
    //    (OgreHlmsPbsDatablock.cpp:887). Our Ogre is built RelWithDebInfo, so
    //    NDEBUG compiles that assert out and the failure would be SILENT —
    //    exactly the class of defect this program exists to remove. So the coat
    //    is cleared BEFORE the family changes and set only AFTER it is Default.
    //  * BRDF WINS over the coat. A non-Default BRDF does not merely ignore the
    //    coat, it cannot carry one at all: PbsProperty::ClearCoat is set only
    //    inside the Default branch (OgreHlmsPbs.cpp:796-804). The panel disables
    //    the coat rows on a non-Default BRDF (D-P1b) so the two agree; the
    //    document keeps the authored values, so switching back restores them.
    //    (HLMS_ADOPTION_SPEC §3.3 sketched the opposite rule — force Default
    //    when coat > 0 — which contradicts D-P1b: the picker would say
    //    Cook-Torrance while the surface rendered Default. Reported to the lead.)
    {
        bool knownBrdf = false;
        const Ogre::PbsBrdf::PbsBrdf want = brdfFromName(p.brdf, &knownBrdf);
        // applyPbr is static (no mError) and runs per changed material per
        // frame, so an unknown name is logged ONCE per distinct spelling.
        if (!knownBrdf) warnUnknownBrdfOnce(p.brdf);
        const bool defaultFamily = (want & Ogre::PbsBrdf::BRDF_MASK) == Ogre::PbsBrdf::Default;
        const bool coatWanted    = defaultFamily && p.clearCoat > 0.0f;

        if (!coatWanted && db->getClearCoat() != 0.0f &&
            (db->getBrdf() & Ogre::PbsBrdf::BRDF_MASK) == Ogre::PbsBrdf::Default)
            db->setClearCoat(0.0f);   // flushes only on the non-zero -> zero edge
        if (db->getBrdf() != static_cast<Ogre::uint32>(want)) db->setBrdf(want);
        if (coatWanted) {
            db->setClearCoat(p.clearCoat);
            // Clamped for the same reason as roughness above, and for a second
            // one: setClearCoatRoughness LOGS A WARNING at <= 1e-6 on EVERY
            // call, with no compare-first (OgreHlmsPbsDatablock.cpp:899-909),
            // and the mirror pushes changed materials per frame.
            db->setClearCoatRoughness(std::max(p.clearCoatRoughness, 1e-4f));
        }
    }
    // Both compare-before-flush internally (OgreHlmsPbsDatablock.cpp:914-929),
    // so the per-frame push costs nothing at an unchanged value.
    db->setReceiveShadows(p.receiveShadows);
    db->setUseEmissiveAsLightmap(p.emissiveAsLightmap);
    switch (p.alphaMode) {
    case PbrAlphaMode::Opaque:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        db->setTransparency(1.0f, kTransparencyNone);
        break;
    case PbrAlphaMode::Cutout:
        // The Hlms template discards when `threshold CMP alpha` is true
        // (threshold on the LEFT — 800.PixelShader_piece_ps.any:300), so
        // CMPF_GREATER discards alpha < cutoff: glTF MASK semantics. The
        // compared alpha comes from the diffuse texture; with no diffuse
        // texture it is 1.0.
        db->setTransparency(1.0f, kTransparencyNone);
        db->setAlphaTest(Ogre::CMPF_GREATER);
        db->setAlphaTestThreshold(p.alphaCutoff);
        break;
    case PbrAlphaMode::Blend:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Fade = plain alpha blending (glTF BLEND); imports keep spec semantics.
        db->setTransparency(p.alpha, Ogre::HlmsPbsDatablock::Fade);
        break;
    case PbrAlphaMode::Glass:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Ogre's own words: "realistic transparency that preserves lighting
        // reflections (particularly specular on the edges). Great for glass."
        // Fade here was why authored glass looked merely faded.
        db->setTransparency(p.alpha, Ogre::HlmsPbsDatablock::Transparent);
        break;
    case PbrAlphaMode::Additive:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Fade carries alpha into the fragment's output .a (it does NOT scale
        // the colour in-shader — the blend factor does that), and
        // changeBlendblock=false keeps SBT_TRANSPARENT_ALPHA out: the
        // blendblock below is SRC_ALPHA/ONE, so Final = Src·alpha + Dest —
        // alpha is the glow intensity, exactly three.js AdditiveBlending.
        db->setTransparency(p.alpha, Ogre::HlmsPbsDatablock::Fade, true, false);
        break;
    case PbrAlphaMode::Refractive:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Ogre: "similar to transparent, but also performs refractions. The
        // compositor scene pass must be set to render refractive objects in its
        // own pass" — that pass is the chain's RQ-200 pass (OgreChain.cpp).
        // Without it the generated shader references an undeclared refractionMap
        // and fails to compile, taking the whole frame with it — so outside a
        // refraction pass the material renders as ordinary glass instead
        // (setRefractionsActive owns that decision).
        db->setTransparency(p.alpha, refractionsActive
                                         ? Ogre::HlmsPbsDatablock::Refractive
                                         : Ogre::HlmsPbsDatablock::Transparent);
        db->setRefractionStrength(p.refractionStrength);
        break;
    case PbrAlphaMode::Modulate:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // No shader-side transparency: Final = Src × Dest via the blendblock
        // below (SBT_MODULATE). alpha deliberately ignored — scaling the colour
        // toward black would darken harder, not fade the effect out.
        db->setTransparency(1.0f, kTransparencyNone, true, false);
        break;
    }
    // Additive/Modulate ride an explicit blendblock preset (setTransparency
    // only knows alpha blending) and never write depth — like other transparents
    // they must not occlude what they blend over. Managed idempotently both
    // ways, the same discipline as the culling macroblock above.
    {
        const bool srcDest = p.alphaMode == PbrAlphaMode::Additive ||
                             p.alphaMode == PbrAlphaMode::Modulate;
        Ogre::HlmsBlendblock want = *db->getBlendblock();
        if (p.alphaMode == PbrAlphaMode::Additive) {
            // SRC_ALPHA/ONE, not SBT_ADD's ONE/ONE: Fade puts alpha in .a and
            // the source factor scales the contribution by it.
            want.mSeparateBlend = false;
            want.mSourceBlendFactor      = Ogre::SBF_SOURCE_ALPHA;
            want.mDestBlendFactor        = Ogre::SBF_ONE;
            want.mSourceBlendFactorAlpha = Ogre::SBF_SOURCE_ALPHA;
            want.mDestBlendFactorAlpha   = Ogre::SBF_ONE;
        }
        else if (p.alphaMode == PbrAlphaMode::Modulate)
            want.setBlendType(Ogre::SBT_MODULATE);
        else if (p.alphaMode == PbrAlphaMode::Opaque || p.alphaMode == PbrAlphaMode::Cutout)
            want.setBlendType(Ogre::SBT_REPLACE);   // undo a previous Additive/Modulate
        const Ogre::HlmsBlendblock &cur = *db->getBlendblock();
        if (want.mSourceBlendFactor != cur.mSourceBlendFactor ||
            want.mDestBlendFactor != cur.mDestBlendFactor ||
            want.mSourceBlendFactorAlpha != cur.mSourceBlendFactorAlpha ||
            want.mDestBlendFactorAlpha != cur.mDestBlendFactorAlpha ||
            want.mSeparateBlend != cur.mSeparateBlend)
            db->setBlendblock(want);
        Ogre::HlmsMacroblock macro = *db->getMacroblock();
        const bool wantDepthWrite = !srcDest;
        if (macro.mDepthWrite != wantDepthWrite) {
            macro.mDepthWrite = wantDepthWrite;
            db->setMacroblock(macro);
        }
    }
    // Alpha-to-coverage on Cutout ONLY, and only when the target is actually
    // multisampled (A2cEnabledMsaaOnly): MSAA then dithers the hard alpha-test
    // edge into coverage samples instead of a 1px staircase. Free at 1x; a PSO
    // rebuild for cutout datablocks only. AFTER the switch: setTransparency
    // rewrites the blendblock, and applyPbr must stay idempotent both ways.
    {
        const Ogre::uint8 want = static_cast<Ogre::uint8>(
            p.alphaMode == PbrAlphaMode::Cutout ? Ogre::HlmsBlendblock::A2cEnabledMsaaOnly
                                                : Ogre::HlmsBlendblock::A2cDisabled);
        if (db->getBlendblock()->mAlphaToCoverage != want) {
            Ogre::HlmsBlendblock bb = *db->getBlendblock();
            bb.mAlphaToCoverage = want;
            db->setBlendblock(bb);
        }
    }
}

// The UNLIT shading model (HLMS_ADOPTION P4a). This is the whole of what the
// Unlit family can honour from PbrParams, and the shortness IS the feature:
// there is no lighting term to feed metalness, roughness, normals, emissive,
// clear coat, a BRDF or shadow reception into. Those values are not consumed
// and not destroyed — the document keeps them, the panel greys them out with a
// reason, and switching back to Lit brings them all back.
void OgreScene::applyUnlit(Ogre::HlmsUnlitDatablock *db, const PbrParams &p) {
    // The alpha modes that mean something without lighting. Glass and
    // Refractive are defined ENTIRELY by what light does at the surface (glass
    // keeps its specular while its diffuse fades; refraction bends what is
    // behind it) so on Unlit they degrade to a plain alpha blend rather than
    // pretending. Additive and Modulate are already unlit-leaning by design.
    const bool blended = p.alphaMode == PbrAlphaMode::Blend ||
                         p.alphaMode == PbrAlphaMode::Glass ||
                         p.alphaMode == PbrAlphaMode::Refractive ||
                         p.alphaMode == PbrAlphaMode::Additive;
    const float alpha = blended ? p.alpha : 1.0f;
    db->setUseColour(true);
    db->setColour(Ogre::ColourValue(p.albedo.r, p.albedo.g, p.albedo.b, alpha));

    if (p.alphaMode == PbrAlphaMode::Cutout) {
        // Same sense as the PBS path: the template discards when
        // `threshold CMP alpha`, so GREATER discards alpha below the cutoff.
        db->setAlphaTest(Ogre::CMPF_GREATER);
        db->setAlphaTestThreshold(p.alphaCutoff);
    } else {
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
    }

    // Blendblock and macroblock are managed idempotently in BOTH directions,
    // the same discipline applyPbr uses: this runs on every changed push.
    {
        Ogre::HlmsBlendblock want = *db->getBlendblock();
        if (p.alphaMode == PbrAlphaMode::Additive) {
            want.mSeparateBlend = false;
            want.mSourceBlendFactor      = Ogre::SBF_SOURCE_ALPHA;
            want.mDestBlendFactor        = Ogre::SBF_ONE;
            want.mSourceBlendFactorAlpha = Ogre::SBF_SOURCE_ALPHA;
            want.mDestBlendFactorAlpha   = Ogre::SBF_ONE;
        } else if (p.alphaMode == PbrAlphaMode::Modulate) {
            want.setBlendType(Ogre::SBT_MODULATE);
        } else if (blended) {
            want.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
        } else {
            want.setBlendType(Ogre::SBT_REPLACE);
        }
        const Ogre::HlmsBlendblock &cur = *db->getBlendblock();
        if (want.mSourceBlendFactor != cur.mSourceBlendFactor ||
            want.mDestBlendFactor != cur.mDestBlendFactor ||
            want.mSourceBlendFactorAlpha != cur.mSourceBlendFactorAlpha ||
            want.mDestBlendFactorAlpha != cur.mDestBlendFactorAlpha ||
            want.mSeparateBlend != cur.mSeparateBlend)
            db->setBlendblock(want);
    }
    {
        Ogre::HlmsMacroblock macro = *db->getMacroblock();
        const Ogre::CullingMode wantCull = p.twoSided ? Ogre::CULL_NONE : Ogre::CULL_CLOCKWISE;
        // Anything that blends must not write depth — it cannot occlude what it
        // is blending over. Modulate blends too even though it ignores alpha.
        const bool wantDepthWrite = !blended && p.alphaMode != PbrAlphaMode::Modulate;
        if (macro.mCullMode != wantCull || macro.mDepthWrite != wantDepthWrite) {
            macro.mCullMode = wantCull;
            macro.mDepthWrite = wantDepthWrite;
            db->setMacroblock(macro);
        }
    }
}

// ---- Materials ----
MaterialId OgreScene::createPbrMaterial(const PbrParams &p) {
    JAH_TRY {
        MaterialRec rec;
        rec.params = p;
        // The shading model is honoured AT CREATION, so a scene full of saved
        // Unlit materials builds them in the right family directly instead of
        // creating a Pbs datablock and immediately destroying it.
        if (p.shadingModel == ShadingModel::Unlit) {
            rec.datablockName = processUniqueName("unlitpbr");
            rec.unlit = true;          // no GI, and hlmsFor picks HlmsUnlit
            rec.shadingUnlit = true;   // ...but it is scene geometry, not an overlay
            auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(
                mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
            auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
                Ogre::IdString(rec.datablockName), rec.datablockName,
                Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
            applyUnlit(db, p);
            mMaterials[++mNextMaterialId] = rec;
            return mNextMaterialId;
        }
        rec.datablockName = processUniqueName("pbr");
        rec.refractive = p.alphaMode == PbrAlphaMode::Refractive;
        auto *hlmsPbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->createDatablock(
            Ogre::IdString(rec.datablockName), rec.datablockName,
            Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        db->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        applyPbr(db, p, mRefractionsActive);
        if (Ogre::TextureGpu *rt = reflectionTexForDatablocks()) db->setTexture(Ogre::PBSM_REFLECTION, rt);
        mMaterials[++mNextMaterialId] = rec;
        return mNextMaterialId;
    } JAH_CATCH(mError, 0);
}

bool OgreScene::setPbrMaterial(MaterialId id, const PbrParams &p) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) return false;
    // An OVERLAY unlit material (grid, gizmo, outline) has no PBR parameters at
    // all. A material whose SHADING MODEL is Unlit does — it just renders them
    // through the other family.
    if (it->second.unlit && !it->second.shadingUnlit) {
        mError = "setPbrMaterial: material is unlit"; return false;
    }
    JAH_TRY {
        // p.shadingModel is IGNORED here, by design: a family switch destroys
        // the datablock and re-attaches every renderable, which cannot happen
        // inside a per-frame parameter push. setShadingModel owns it, and the
        // host calls that first (the model term is in PbrParams::operator== so
        // the host's change-guard notices).
        auto *hlms = hlmsFor(it->second);
        auto *raw = hlms->getDatablock(Ogre::IdString(it->second.datablockName));
        if (!raw) return false;
        it->second.params = p;
        if (it->second.shadingUnlit) {
            applyUnlit(static_cast<Ogre::HlmsUnlitDatablock *>(raw), p);
            return true;   // an unlit material is never refractive
        }
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(raw);
        applyPbr(db, p, mRefractionsActive);
        // An alpha-mode change moves the item between render queues, and the
        // items already exist: re-file them or a material turned refractive
        // keeps rendering in the opaque pass (as plain glass) until something
        // else happens to re-attach it.
        const bool wasRefractive = it->second.refractive;
        it->second.refractive = p.alphaMode == PbrAlphaMode::Refractive;
        if (wasRefractive != it->second.refractive) refileItems(id, it->second);
        return true;
    } JAH_CATCH(mError, false);
}

// THE FAMILY SWITCH (HLMS_ADOPTION P4a §6.2). Destroy, recreate in the other
// family, re-attach — and the MaterialId survives, because the host's scene
// refers to it from every node.
bool OgreScene::setShadingModel(MaterialId id, ShadingModel model) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) { mError = "setShadingModel: unknown material"; return false; }
    MaterialRec &rec = it->second;
    if (rec.unlit && !rec.shadingUnlit) {
        // Grids, gizmos, wires and the selection outline are unlit as a POLICY;
        // they are not PBR materials and have no shading model to set.
        mError = "setShadingModel: this is an overlay material, not a PBR material";
        return false;
    }
    const bool wantUnlit = model == ShadingModel::Unlit;
    if (wantUnlit == rec.shadingUnlit) return true;   // idempotent, touches nothing

    // RULE 5, and it is a REFUSAL rather than a fallback: HlmsUnlit's
    // calculateHashForPreCreate hard-zeroes Skeleton and BonesPerVertex, so an
    // unlit rigged mesh renders welded to its bind pose — a second, solid body
    // standing where the character used to be, with no error anywhere. The
    // outline path solved the same problem by staying on HlmsPbs
    // (createOutlineMaterial); a USER material cannot, because unlit is the
    // thing being asked for.
    //
    // The test is the NODE's live skeleton, not "the mesh has a rig bound":
    // several nodes may share one mesh and only some of them attach it through
    // attachSkinnedMesh, and an unrigged node using rigged GEOMETRY has nothing
    // to lose here.
    if (wantUnlit) {
        for (const auto &kv : mNodes) {
            if (kv.second.materialRef != id) continue;
            if (hasSkeleton(kv.first)) {
                mError = "setShadingModel: the Unlit shading model cannot skin, and this "
                         "material is used by a rigged mesh (it would render at its bind "
                         "pose) — remove the rig or use a different material";
                return false;
            }
        }
    }

    JAH_TRY {
        // BEFORE the datablock pointer dies: VctMaterial caches its conversions
        // by raw datablock pointer, and a recycled address would alias.
        invalidateGiCaches();

        // Remember who was rendering with this material, then take the
        // renderables down. attachMesh below rebuilds each one from scratch,
        // which is also what re-applies the render-queue and GI-visibility
        // rules for the NEW family.
        std::vector<std::pair<NodeId, MeshId>> attached;
        for (auto &kv : mNodes) {
            if (kv.second.materialRef != id || !kv.second.meshRef) continue;
            attached.emplace_back(kv.first, kv.second.meshRef);
            detachItem(kv.first, kv.second);
        }

        Ogre::Hlms *oldHlms = hlmsFor(rec);
        if (oldHlms->getDatablock(Ogre::IdString(rec.datablockName)))
            oldHlms->destroyDatablock(Ogre::IdString(rec.datablockName));

        // A FRESH NAME, not the old one reused. Datablock names are IdStrings
        // in a per-Hlms registry and also key the shader cache's per-datablock
        // state; minting a new one makes "this name, in this family, means this
        // content" true by construction instead of relying on destroy/create
        // ordering inside one frame.
        rec.shadingUnlit = wantUnlit;
        rec.unlit = wantUnlit;
        rec.pbsBacked = false;
        rec.refractive = !wantUnlit && rec.params.alphaMode == PbrAlphaMode::Refractive;
        rec.datablockName = processUniqueName(wantUnlit ? "unlitpbr" : "pbr");
        rec.params.shadingModel = model;

        if (wantUnlit) {
            auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(
                mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
            auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
                Ogre::IdString(rec.datablockName), rec.datablockName,
                Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
            applyUnlit(db, rec.params);
        } else {
            auto *hlmsPbs = static_cast<Ogre::HlmsPbs *>(
                mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
            auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->createDatablock(
                Ogre::IdString(rec.datablockName), rec.datablockName,
                Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
            db->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
            applyPbr(db, rec.params, mRefractionsActive);
            if (Ogre::TextureGpu *rt = reflectionTexForDatablocks()) db->setTexture(Ogre::PBSM_REFLECTION, rt);
        }
        // The maps the host already pushed are the host's state, not the
        // datablock's: re-bind them or a switch would silently strip every
        // texture until something happened to push them again.
        bindTrackedTextures(rec);
        // ...and the same for a generated piece: the new datablock is a blank
        // one, so a graph material that took a detour through Unlit would come
        // back rendering its plain PBR surface with no error anywhere.
        bindTrackedPieces(rec);

        for (const auto &na : attached) attachMesh(na.first, na.second, id);
        return true;
    } JAH_CATCH(mError, false);
}

std::string OgreScene::dumpMaterial(MaterialId id) const {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) { mError = "dumpMaterial: unknown material"; return {}; }
    JAH_TRY {
        auto *hlms = hlmsFor(it->second);
        auto *db = hlms->getDatablock(Ogre::IdString(it->second.datablockName));
        if (!db) { mError = "dumpMaterial: the material has no datablock"; return {}; }
        // A null listener is fine — HlmsJson substitutes its own default
        // (OgreHlmsJson.cpp:147-153). This reads the LIVE datablock, so what
        // comes back is the state after every clamp, guard and idempotency
        // rule in applyPbr, which is the entire point of the verb.
        Ogre::HlmsJson json(mRoot->getHlmsManager(), nullptr);
        Ogre::String out;
        json.saveMaterial(db, out, Ogre::BLANKSTRING);
        return std::string(out.c_str());
    } JAH_CATCH(mError, {});
}

// ---- Generated shader pieces (HLMS_ADOPTION P5) ----
//
// A "custom piece" is a fragment of the backend's own shader language spliced
// into ONE datablock's generated shader at a named hook. It is the mechanism
// the shader graph needs and the CPU baker cannot be: a bake freezes time, has
// a resolution, and cannot move a vertex at all.
//
// Two rules make this safe to hand a generator:
//   * A material with NO piece is unaffected, bit for bit. The backend only
//     sets its `_DatablockCustomPieceShaderName*` property when the datablock
//     carries a piece id, and only parses when that property is set — so every
//     material in the tree that never calls this generates exactly the source
//     it generated before the verb existed.
//   * The piece REGISTRY IS KEYED BY FILE NAME (an IdString hash of it), not by
//     path, and re-registering one name with different content is a hard throw
//     from the backend. The same trap the IES profile loader documents, with a
//     stricter failure mode. We do not guard it with a clash test the way
//     OgreLights does; we ask callers to name pieces by a hash of their
//     CONTENT, which makes "same name, different content" impossible instead of
//     merely detected. The generator does exactly that.
static Ogre::CustomPieceStage::CustomPieceStage ogrePieceStage(CustomPieceStage stage) {
    return stage == CustomPieceStage::VertexPreTransform ? Ogre::CustomPieceStage::VertexShader
                                                         : Ogre::CustomPieceStage::PixelShader;
}

bool OgreScene::setMaterialCustomPiece(MaterialId id, const std::string &path,
                                       CustomPieceStage stage) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) { mError = "setMaterialCustomPiece: unknown material"; return false; }
    MaterialRec &rec = it->second;
    // The Unlit family runs a different template with a different pixel-data
    // structure; a piece written against the PBR surface would not compile
    // there, and a shader that does not compile takes the whole frame with it.
    // The BINDING IS KEPT in the record either way, so a material that goes
    // Unlit and comes back gets its piece again (setShadingModel re-applies).
    const size_t slot = stage == CustomPieceStage::VertexPreTransform ? 1u : 0u;
    if (rec.unlit) {
        rec.customPiece[slot] = path;
        mError = "setMaterialCustomPiece: the Unlit shading model has no PBR surface for a "
                 "generated piece to write to; the binding is remembered and applies again "
                 "if the material returns to Lit";
        return false;
    }
    JAH_TRY {
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(
            hlmsFor(rec)->getDatablock(Ogre::IdString(rec.datablockName)));
        if (!db) { mError = "setMaterialCustomPiece: the material has no datablock"; return false; }
        if (path.empty()) {
            db->setCustomPieceFile(Ogre::BLANKSTRING, Ogre::BLANKSTRING, ogrePieceStage(stage));
            rec.customPiece[slot].clear();
            applyClockProperty(db, rec);
            return true;
        }
        const size_t sep = path.find_last_of("/\\");
        const std::string dir  = sep == std::string::npos ? "." : path.substr(0, sep);
        const std::string file = sep == std::string::npos ? path : path.substr(sep + 1);
        Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
        static const char *kGroup = "Jahshaka";
        if (!rgm.resourceGroupExists(kGroup)) rgm.createResourceGroup(kGroup, false);
        if (!mPieceDirs.count(dir)) {
            rgm.addResourceLocation(dir, "FileSystem", kGroup, false);
            mPieceDirs.insert(dir);
        }
        if (!rgm.resourceExists(kGroup, file)) {
            mError = "setMaterialCustomPiece: piece file not found: " + path;
            return false;
        }
        // Throws (and is caught below) when this NAME was already registered
        // with different content — see the header comment. Also flushes the
        // material's renderables, which is what makes the new shader take.
        db->setCustomPieceFile(file, kGroup, ogrePieceStage(stage));
        rec.customPiece[slot] = path;
        applyClockProperty(db, rec);
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::setShaderTime(float seconds) {
    mShaderTime = seconds;
    // Read by FogHlmsListener::preparePassBuffer, on the render thread, once
    // per pass. Nothing is flushed and nothing recompiles — the value lands in
    // the pass constant buffer the next time one is built.
    FogHlmsListener::setSceneTime(mSceneMgr, seconds);
}

float OgreScene::shaderTime() const { return mShaderTime; }

// OUR OWN datablock property, and the isolation contract in one function:
// `jah_shader_clock` is what our Hlms library gates the pass-buffer clock
// declaration on, so a material with no generated piece declares nothing, reads
// nothing, and produces byte-identical shader source to a build in which none
// of this existed. Set when a piece is bound, cleared when the last one goes.
void OgreScene::applyClockProperty(Ogre::HlmsPbsDatablock *db, const MaterialRec &rec) {
    const bool wanted = !rec.customPiece[0].empty() || !rec.customPiece[1].empty();
    const Ogre::HlmsDatablock::CustomPropertyVec &current = db->getCustomProperties();
    const bool have = !current.empty();
    if (wanted == have) return;   // setCustomProperties flushes renderables: idempotent or nothing
    Ogre::HlmsDatablock::CustomPropertyVec props;
    if (wanted) props.emplace_back("jah_shader_clock", 1);
    db->setCustomProperties(props, true);
}

void OgreScene::bindTrackedPieces(const MaterialRec &rec) {
    if (rec.unlit) return;
    auto *db = static_cast<Ogre::HlmsPbsDatablock *>(
        hlmsFor(rec)->getDatablock(Ogre::IdString(rec.datablockName)));
    if (!db) return;
    for (size_t slot = 0; slot < 2; ++slot) {
        if (rec.customPiece[slot].empty()) continue;
        const std::string &path = rec.customPiece[slot];
        const size_t sep = path.find_last_of("/\\");
        const std::string file = sep == std::string::npos ? path : path.substr(sep + 1);
        db->setCustomPieceFile(file, "Jahshaka",
                               ogrePieceStage(slot == 1 ? CustomPieceStage::VertexPreTransform
                                                        : CustomPieceStage::PixelPreLights));
    }
    applyClockProperty(db, rec);
}

bool OgreScene::destroyMaterial(MaterialId id) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) return false;
    JAH_TRY {
        invalidateGiCaches();   // VctMaterial caches conversions by raw datablock pointer
        for (auto &kv : mNodes) if (kv.second.materialRef == id) detachItem(kv.first, kv.second);
        Ogre::Hlms *hlms = hlmsFor(it->second);
        if (hlms->getDatablock(Ogre::IdString(it->second.datablockName)))
            hlms->destroyDatablock(Ogre::IdString(it->second.datablockName));
        mMaterials.erase(it);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::attachMesh(NodeId id, MeshId meshId, MaterialId matId) {
    auto nit = mNodes.find(id); auto mit = mMeshes.find(meshId); auto tit = mMaterials.find(matId);
    if (nit == mNodes.end()) { mError = "attachMesh: unknown node"; return false; }
    if (mit == mMeshes.end()) { mError = "attachMesh: unknown mesh"; return false; }
    if (tit == mMaterials.end()) { mError = "attachMesh: unknown material"; return false; }
    JAH_TRY {
        Node &n = nit->second;
        detachItem(id, n);
        // THE ITEM IS BORN IN ITS NODE'S CLASS (SCENEGRAPH_SPEC §6 rule 3):
        // SceneNode::attachObject THROWS when the object's static flag and the
        // node's disagree, and the document marks never-moving geometry
        // SCENE_STATIC before the mirror ever gets here. Reading the node
        // rather than hard-coding SCENE_DYNAMIC is the whole of the engine's
        // part in static subtrees.
        const Ogre::SceneMemoryMgrTypes cls =
            n.node && n.node->isStatic() ? Ogre::SCENE_STATIC : Ogre::SCENE_DYNAMIC;
        n.item = mSceneMgr->createItem(mit->second.mesh, cls);
        n.item->setDatablock(hlmsFor(tit->second)->getDatablock(Ogre::IdString(tit->second.datablockName)));
        // Only lit (PBR) surfaces participate in GI; unlit overlays, wires and
        // line meshes must neither bounce nor occlude the radiosity rays.
        n.item->setVisibilityFlags(itemVisibilityFlags(n, tit->second.unlit));
        // LIGHTING CHANNELS: the Item is BORN with Ogre's all-ones default, so
        // this only matters for a node whose mask was set before its geometry
        // arrived — or whose Item this very call is REBUILDING after a material
        // swap. Without it a masked object silently goes back to being lit by
        // everything the moment its material changes.
        n.item->setLightMask(n.lightMask);
        // Render-queue policy (POST_CHAIN_SPEC.md §6): on-top overlays go in the
        // chain's overlay pass, refractive items in its refraction pass, and
        // everything else stays on Ogre's default queue.
        n.item->setRenderQueueGroup(renderQueueFor(tit->second));
        n.node->attachObject(n.item);
        // A static Item is not in the per-frame bounds list: without this its
        // world AABB stays at its birth value and it is culled (and picked)
        // wrong. The manager batches the dirty per frame.
        if (cls == Ogre::SCENE_STATIC) mSceneMgr->notifyStaticAabbDirty(n.item);
        n.meshRef = meshId; n.materialRef = matId;
        // New lit geometry must join the voxel volume / next trace; unlit
        // overlays (outlines, wires) never participate in GI.
        if (!tit->second.unlit) invalidateGiCaches();
        // A reflector node whose mesh was swapped keeps its flag (detachItem
        // above only disarmed the dead Item) — re-derive the plane from the new
        // geometry. A failure here is not fatal to attachMesh: the node simply
        // stops reflecting and lastError() says why.
        if (mReflectors.count(id)) armReflector(id, n);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::detachMesh(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    JAH_TRY { detachItem(id, it->second); return true; } JAH_CATCH(mError, false);
}

// ---- Textures ----
std::string OgreScene::textureKey(const std::string &path, bool decal, DecalMap kind) {
    if (!decal) return path;
    return "d" + std::to_string(int(kind)) + "|" + path;
}

TextureId OgreScene::trackTexture(const TextureRec &rec) {
    const TextureId id = ++mNextTextureId;
    mTextures[id] = rec;
    // Pixel-uploaded textures (createTexture) have no path and are never
    // deduplicated — the caller owns their identity.
    if (!rec.path.empty())
        mTextureIndex.emplace(textureKey(rec.path, rec.decal, rec.decalKind), id);
    return id;
}

bool syncTextureLoads() {
    // Read ONCE per process: the answer cannot change while a process runs, and
    // a getenv per texture in a mirror walk would be a silly cost to add to the
    // phase that exists to remove one.
    static const bool sync = [] {
        const char *v = std::getenv("JAH_TEXTURE_SYNC_LOAD");
        return v && *v && v[0] != '0';
    }();
    return sync;
}

void waitForTextureResident(Ogre::TextureGpu *tex) {
    // Read the contract in EnginePrivate.h. Two guards, both meaningful: a
    // texture already Resident has nothing to wait for, and a ManualTexture is
    // transitioned by hand and would deadlock in waitForData (there is no
    // streaming request behind it to complete).
    if (!tex) return;
    if (tex->getResidencyStatus() == Ogre::GpuResidency::Resident && tex->isDataReady()) return;
    if (tex->isManualTexture()) return;
    tex->waitForData();
}

TextureId OgreScene::loadTexture(const std::string &path, bool srgb) {
    // Path dedup, but NEVER across the decal atlases: the same image file can be
    // both an ordinary PBR map and a decal image, and they live in different
    // pools with different formats. Handing a decal slice back from here would
    // bind a pooled slice as a base map AND let the caller destroyTexture() a
    // texture other scenes' decals are still sampling. mTextureIndex keeps the
    // two namespaces apart (textureKey) — and turns what was a linear scan of
    // every texture in the scene into one hash lookup.
    {
        auto hit = mTextureIndex.find(textureKey(path, false, DecalMap::Diffuse));
        if (hit != mTextureIndex.end()) return hit->second;
    }
    const size_t slash = path.find_last_of("/\\");
    const std::string dir  = slash == std::string::npos ? "." : path.substr(0, slash);
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    JAH_TRY {
        Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
        static const char *kGroup = "Jahshaka";
        if (!rgm.resourceGroupExists(kGroup)) rgm.createResourceGroup(kGroup, false);
        if (!mTextureDirs.count(dir)) {
            rgm.addResourceLocation(dir, "FileSystem", kGroup, false);
            mTextureDirs.insert(dir);
        }
        if (!rgm.resourceExists(kGroup, file)) { mError = "loadTexture: file not found: " + path; return 0; }
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        // THE CHANNEL PROBE, and the sidecar that skips it (THREADING_ADOPTION_
        // SPEC.md P2 item 7, decision D-D(b)).
        //
        // Grayscale files (single-channel jpg/png) decode to an R8 texture and
        // sample red-only — a black/white checker renders black/red — so they
        // are expanded to RGBA on the CPU below. Asking "is this file
        // single-channel?" used to mean FULLY DECODING every texture on the UI
        // thread and throwing the result away: every image in the project was
        // decoded twice, once here and once by the streaming worker.
        //
        // The sidecar (TextureCache, EnginePrivate.h) remembers the answer per
        // path. A hit that says "not grayscale" skips the decode entirely, which
        // is the common case and the whole win. A hit that says "grayscale" still
        // has to decode, because the expansion needs the PIXELS — that is not a
        // regression, it is the same work as today, on the rare path. A miss
        // decodes once and records, so a first-ever launch pays exactly what
        // every launch used to.
        unsigned cachedComponents = 0;
        bool     cachedCompressed = false;
        const bool cacheHit = textureCache().channels(path, cachedComponents, cachedCompressed);
        const bool mayBeGrayscale = !cacheHit || (cachedComponents == 1 && !cachedCompressed);
        if (mayBeGrayscale) {
            // load2, not load(file, group): load() picks the codec by file
            // extension alone and THROWS on mislabeled files (the old importer
            // wrote PNG bytes under .jpg names — GLB embedded textures), which
            // made every such texture silently render white. load2 validates
            // the extension's codec against the magic bytes and falls back to
            // content sniffing — the same tolerant route TextureGpuManager
            // itself uses for the actual GPU load below.
            Ogre::Image2 probe;
            {
                Ogre::DataStreamPtr stream = rgm.openResource(file, kGroup);
                probe.load2(stream, file);
            }
            const Ogre::PixelFormatGpu pf = probe.getPixelFormat();
            const unsigned components = Ogre::PixelFormatGpuUtils::getNumberOfComponents(pf);
            const bool compressed = Ogre::PixelFormatGpuUtils::isCompressed(pf);
            textureCache().note(path, components, compressed);
            if (components == 1 && !compressed) {
                const Ogre::uint32 w = probe.getWidth(), h = probe.getHeight();
                Ogre::Image2 *rgba = new Ogre::Image2();
                rgba->createEmptyImage(w, h, 1u, Ogre::TextureTypes::Type2D,
                                       srgb ? Ogre::PFG_RGBA8_UNORM_SRGB : Ogre::PFG_RGBA8_UNORM,
                                       Ogre::PixelFormatGpuUtils::getMaxMipmapCount(w, h));
                for (Ogre::uint32 y = 0; y < h; ++y)
                    for (Ogre::uint32 x = 0; x < w; ++x) {
                        Ogre::ColourValue c = probe.getColourAt(x, y, 0);
                        c.g = c.b = c.r; c.a = 1.0f;
                        rgba->setColourAt(c, x, y, 0);
                    }
                rgba->generateMipmaps(srgb, Ogre::Image2::FILTER_BILINEAR);
                Ogre::TextureGpu *tex = tm->createTexture(processUniqueName("gray"),
                                                          Ogre::GpuPageOutStrategy::Discard, 0,
                                                          Ogre::TextureTypes::Type2D);
                tex->setResolution(w, h);
                tex->setNumMipmaps(rgba->getNumMipmaps());
                tex->setPixelFormat(rgba->getPixelFormat());
                tex->scheduleTransitionTo(Ogre::GpuResidency::Resident, rgba, true);   // deletes rgba
                // THIS WAIT STAYS (THREADING_ADOPTION_SPEC.md P2 item 1). It is
                // not a file-streaming request: the pixels are a CPU-built
                // Image2 we own, passing one implies bSkipMultiload
                // (OgreTextureGpu.h:405-418), and single-channel images are
                // rare. Keeping it synchronous costs nothing measurable and
                // keeps this branch's ownership of `rgba` obvious.
                tex->waitForData();
                TextureRec rec; rec.texture = tex; rec.path = path;
                return trackTexture(rec);
            }
        }
        Ogre::uint32 flags = Ogre::TextureFlags::AutomaticBatching;
        if (srgb) flags |= Ogre::TextureFlags::PrefersLoadingFromFileAsSRGB;
        // Alias by full path so the same file name in two folders stays distinct.
        Ogre::TextureGpu *tex = tm->createOrRetrieveTexture(file, path, Ogre::GpuPageOutStrategy::Discard,
                                                            flags, Ogre::TextureTypes::Type2D, kGroup,
                                                            Ogre::TextureFilter::TypeGenerateDefaultMipmaps);
        if (!tex) { mError = "loadTexture: could not create texture for " + path; return 0; }
        // SCHEDULE, DO NOT WAIT (THREADING_ADOPTION_SPEC.md P2 item 1, decision
        // D-C(1)). This used to be followed by `tex->waitForData()`, which
        // blocked the calling thread — the UI thread, inside SceneMirror's
        // per-frame walk — until this ONE texture had been read, decoded and
        // uploaded. N textures in a scene meant N serial round trips to the
        // streaming worker, and a texture-heavy open was a sequence of stalls.
        //
        // Now every texture in a mirror walk is SCHEDULED before any of them is
        // WAITED ON, so their decodes overlap (and, with the multiload pool,
        // run on several threads); the wait happens once, at the frame edge, in
        // OgreEngine::renderOneFrame. Nothing between here and that wait draws
        // anything, so no frame ever samples a texture that is not resident —
        // which is what keeps every pixel suite byte-exact.
        //
        // BINDING A NOT-YET-RESIDENT TEXTURE IS SAFE ANYWAY, and that is
        // upstream's design rather than our luck: any residency or pool-slot
        // change destroys the datablock's descriptor set and reschedules its
        // const-buffer update (OgreHlmsTextureBaseClass.inl:459-486), and the
        // rebake re-reads getInternalSliceStart() ("May have changed if the
        // TextureGpuManager updated the Texture", :165-180).
        //
        // JAH_TEXTURE_SYNC_LOAD=1 PUTS THE OLD WAIT BACK, at run time, with no
        // rebuild. Two reasons it exists, both named in the spec: it is the A
        // arm of the batched-loading A/B measurement (G2-c — "today's behaviour,
        // kept reachable by env for exactly this purpose"), and it is the second
        // step of the phase's order of retreat (pool -> wait -> caches) if this
        // ever has to be backed out on a user's machine. A measurement and
        // recovery hatch, deliberately not a preference and not persisted — the
        // same shape as JAHSHAKA_SCENE_THREADS.
        tex->scheduleTransitionTo(Ogre::GpuResidency::Resident);
        if (detail::syncTextureLoads()) tex->waitForData();
        TextureRec rec; rec.texture = tex; rec.path = path;
        return trackTexture(rec);
    } JAH_CATCH(mError, 0);
}

TextureId OgreScene::createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb) {
    if (!w || !h || !rgba) { mError = "createTexture: empty image"; return 0; }
    JAH_TRY {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        const std::string name = processUniqueName("pixels");
        // ManualTexture (non-batched) on purpose: setSkyCubemap copyTo's these
        // into cubemap faces, which batched pool slices cannot do. KNOWN macOS
        // DEFECT: non-batched base maps sample the wrong data under MoltenVK
        // (pbr_texture_scale_tiles_uvs; file-loaded/batched textures are fine) —
        // fixing it means teaching the cubemap path to copy from pool slices,
        // then batching these like loadTexture does.
        Ogre::TextureGpu *tex = tm->createTexture(name, Ogre::GpuPageOutStrategy::Discard,
                                                  Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        tex->setResolution(w, h);
        tex->setNumMipmaps(1u);
        tex->setPixelFormat(srgb ? Ogre::PFG_RGBA8_UNORM_SRGB : Ogre::PFG_RGBA8_UNORM);
        // IMMEDIATE residency (_transitionTo), and NO explicit notifyDataIsReady:
        // for a ManualTexture _transitionTo(Resident) calls notifyDataIsReady
        // ITSELF (OgreTextureGpu.cpp:600). A second call underflows the uint8
        // mDataPreparationsPending to 255, isDataReady() then never turns true,
        // and anything that waits on the texture — InstantRadiosity's
        // downloadTexture under GI, Image2::convertFromTexture — spins in
        // waitForData forever (found by the GI churn test hanging).
        tex->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        Ogre::StagingTexture *staging = tm->getStagingTexture(w, h, 1u, 1u, tex->getPixelFormat());
        staging->startMapRegion();
        Ogre::TextureBox box = staging->mapRegion(w, h, 1u, 1u, tex->getPixelFormat());
        for (unsigned y = 0; y < h; ++y) std::memcpy(box.at(0, y, 0), rgba + size_t(y) * w * 4u, size_t(w) * 4u);
        staging->stopMapRegion();
        staging->upload(box, tex, 0, nullptr, nullptr, true);
        tm->removeStagingTexture(staging);
        TextureRec rec; rec.texture = tex; rec.path = "";
        return trackTexture(rec);
    } JAH_CATCH(mError, 0);
}

void OgreScene::releaseTextureRec(const TextureRec &rec) {
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    // A decal-atlas slice is shared across every scene in the process: only the
    // last user's release frees it (OgreDecals.cpp). Destroying it outright here
    // would leave the other scenes' Decals pointing at a dead TextureGpu.
    if (rec.decal) { detail::releaseDecalTexture(tm, rec.decalKind, rec.texture); return; }
    tm->destroyTexture(rec.texture);
}

bool OgreScene::destroyTexture(TextureId id) {
    auto it = mTextures.find(id);
    if (it == mTextures.end()) return false;
    JAH_TRY {
        invalidateGiCaches();   // BEFORE the texture dies: IR caches images by TextureGpu*
        // UNBIND FIRST, from every material still holding it. An
        // HlmsPbsDatablock keeps the raw TextureGpu* (and a descriptor set
        // built from it); destroying a bound texture leaves that pointer
        // stale with no diagnostic until the GPU faults. Nothing reclaimed
        // textures before the deep-audit fix wave, so this was latent — it
        // stops being latent the moment reclaimUnused frees one.
        for (auto &kv : mMaterials) {
            MaterialRec &m = kv.second;
            // Overlay materials (grid, gizmo, outline) hold no tracked maps. A
            // material whose SHADING MODEL is Unlit does — it is a PBR material.
            if (m.unlit && !m.shadingUnlit) continue;
            bool hit = false;
            for (size_t s = 0; s < kPbrTextureSlotCount; ++s) {
                if (m.boundTextures[s] != id) continue;
                m.boundTextures[s] = 0;
                hit = true;
            }
            // Re-bind from the (now cleared) record rather than clearing one
            // unit: on Unlit the mapping from slot to unit is not one-to-one,
            // and one code path owning it is what keeps the two families honest.
            if (hit) bindTrackedTextures(m);
        }
        if (!it->second.path.empty()) {
            const std::string key = textureKey(it->second.path, it->second.decal,
                                               it->second.decalKind);
            auto ix = mTextureIndex.find(key);
            if (ix != mTextureIndex.end() && ix->second == id) mTextureIndex.erase(ix);
        }
        releaseTextureRec(it->second);
        mTextures.erase(it);
        return true;
    } JAH_CATCH(mError, false);
}

Ogre::PbsTextureTypes OgreScene::pbsSlotOf(PbrTextureSlot slot) {
    switch (slot) {
    case PbrTextureSlot::Albedo:    return Ogre::PBSM_DIFFUSE;
    case PbrTextureSlot::Normal:    return Ogre::PBSM_NORMAL;
    case PbrTextureSlot::Metalness: return Ogre::PBSM_METALLIC;
    case PbrTextureSlot::Roughness: return Ogre::PBSM_ROUGHNESS;
    case PbrTextureSlot::Emissive:  return Ogre::PBSM_EMISSIVE;
    }
    return Ogre::PBSM_DIFFUSE;
}

// The sampler every material map is bound with.
//
// Anisotropy must stay 1 while min/mag/mip are FO_LINEAR: Ogre warns, and
// NVIDIA ignores the mismatch, but Metal (MoltenVK) applies maxAnisotropy
// regardless of filter mode and averages the whole texture into every texel
// (caught by pbr_texture_scale_tiles_uvs on the first macOS run). Real
// anisotropic filtering = FO_ANISOTROPIC on all three filters plus a
// pixel-suite recalibration — a deliberate visual change, not a default.
static Ogre::HlmsSamplerblock materialSamplerblock() {
    Ogre::HlmsSamplerblock sampler;
    sampler.mU = Ogre::TAM_WRAP; sampler.mV = Ogre::TAM_WRAP;
    sampler.mMaxAnisotropy = 1; sampler.mMipFilter = Ogre::FO_LINEAR;
    return sampler;
}

void OgreScene::bindTrackedTextures(const MaterialRec &rec) {
    auto *raw = hlmsFor(rec)->getDatablock(Ogre::IdString(rec.datablockName));
    if (!raw) return;
    const Ogre::HlmsSamplerblock sampler = materialSamplerblock();
    auto textureOf = [this](TextureId id) -> Ogre::TextureGpu * {
        if (!id) return nullptr;
        auto tit = mTextures.find(id);
        return tit == mTextures.end() ? nullptr : tit->second.texture;
    };
    if (rec.shadingUnlit) {
        // The Unlit family has 16 texture units, all of them plain colour
        // layers — there is no normal/roughness/metalness/emissive input to
        // map the other four slots onto. Unit 0 multiplies the datablock
        // colour, which is exactly base colour x base-colour map. The other
        // four TextureIds stay in the record, unbound, so switching back to
        // Lit restores every map the user authored.
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(raw);
        Ogre::TextureGpu *tex = textureOf(rec.boundTextures[size_t(PbrTextureSlot::Albedo)]);
        db->setTexture(Ogre::uint8(0), tex, tex ? &sampler : nullptr);
        return;
    }
    auto *db = static_cast<Ogre::HlmsPbsDatablock *>(raw);
    for (size_t s = 0; s < kPbrTextureSlotCount; ++s) {
        Ogre::TextureGpu *tex = textureOf(rec.boundTextures[s]);
        db->setTexture(static_cast<Ogre::uint8>(pbsSlotOf(PbrTextureSlot(s))), tex,
                       tex ? &sampler : nullptr);
    }
}

bool OgreScene::setPbrTexture(MaterialId mat, PbrTextureSlot slot, TextureId texId) {
    auto mit = mMaterials.find(mat);
    if (mit == mMaterials.end() || (mit->second.unlit && !mit->second.shadingUnlit)) {
        mError = "setPbrTexture: not a PBR material"; return false;
    }
    if (texId && mTextures.find(texId) == mTextures.end()) {
        mError = "setPbrTexture: unknown texture"; return false;
    }
    JAH_TRY {
        // Remember the binding first: it is what destroyTexture undoes, what a
        // shading-model switch rebuilds from, and — on Unlit — the record of a
        // map the family cannot show but the document still owns.
        mit->second.boundTextures[size_t(slot)] = texId;
        if (!hlmsFor(mit->second)->getDatablock(Ogre::IdString(mit->second.datablockName)))
            return false;
        // An Unlit material only has somewhere to put Albedo; binding the whole
        // tracked set keeps that decision in ONE place.
        if (mit->second.shadingUnlit) { bindTrackedTextures(mit->second); return true; }
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(
            hlmsFor(mit->second)->getDatablock(Ogre::IdString(mit->second.datablockName)));
        Ogre::TextureGpu *tex = nullptr;
        if (texId) tex = mTextures.find(texId)->second.texture;
        const Ogre::HlmsSamplerblock sampler = materialSamplerblock();
        db->setTexture(static_cast<Ogre::uint8>(pbsSlotOf(slot)), tex, &sampler);
        return true;
    } JAH_CATCH(mError, false);
}

// ---- Overlay primitives ----
MaterialId OgreScene::createUnlitMaterial(const Colour &c, bool depthTest, bool wireframe) {
    JAH_TRY {
        MaterialRec rec; rec.datablockName = processUniqueName("unlit"); rec.unlit = true; rec.onTop = !depthTest;
        auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
        Ogre::HlmsMacroblock macro;
        macro.mDepthCheck = depthTest;
        macro.mDepthWrite = depthTest;
        macro.mCullMode = Ogre::CULL_NONE;
        if (wireframe) macro.mPolygonMode = Ogre::PM_WIREFRAME;
        Ogre::HlmsBlendblock blend;
        if (c.a < 0.999f) blend.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
            Ogre::IdString(rec.datablockName), rec.datablockName, macro, blend, Ogre::HlmsParamVec()));
        db->setUseColour(true);
        db->setColour(toOgre(c));
        mMaterials[++mNextMaterialId] = rec;
        return mNextMaterialId;
    } JAH_CATCH(mError, 0);
}

MaterialId OgreScene::createOutlineMaterial(const Colour &c, bool skinnable) {
    JAH_TRY {
        MaterialRec rec; rec.datablockName = processUniqueName("outline"); rec.unlit = true;
        Ogre::HlmsMacroblock macro;
        // Inverted hull: cull FRONT faces so only the shell's far side shows,
        // forming a silhouette band around the (slightly smaller) original.
        macro.mCullMode = Ogre::CULL_ANTICLOCKWISE;
        macro.mDepthCheck = true;
        macro.mDepthWrite = false;
        if (!skinnable) {
            auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
            auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
                Ogre::IdString(rec.datablockName), rec.datablockName, macro, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
            db->setUseColour(true);
            db->setColour(toOgre(c));
            mMaterials[++mNextMaterialId] = rec;
            return mNextMaterialId;
        }
        // THE SKINNED SILHOUETTE. HlmsUnlit has no skeletal path in this engine
        // (`hlms_skeleton` appears only in the Pbs templates), so an unlit hull
        // over a rigged mesh is welded to the bind pose while the character
        // animates away from it — which is not a subtle artefact: it is a
        // second, solid, selection-coloured body standing in the scene.
        //
        // HlmsPbs skins, so the same silhouette is built there and made unlit IN
        // EFFECT: no diffuse, no specular, no metal, the colour as EMISSIVE.
        // Emissive is added straight to the shaded result, so with everything
        // else at zero the shell renders the flat selection colour — and it
        // deforms, which is the whole point. `rec.unlit` stays TRUE because it
        // means "an overlay, not scene geometry" everywhere else in this file
        // (GI participation, render-queue choice); rec.pbsBacked is what tells
        // hlmsFor which Hlms actually owns the datablock.
        rec.pbsBacked = true;
        auto *hlmsPbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->createDatablock(
            Ogre::IdString(rec.datablockName), rec.datablockName, macro, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        db->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        db->setDiffuse(Ogre::Vector3::ZERO);
        db->setSpecular(Ogre::Vector3::ZERO);
        db->setMetalness(0.0f);
        db->setRoughness(1.0f);
        db->setEmissive(Ogre::Vector3(c.r, c.g, c.b));
        // The shell is a UI affordance, not geometry: it must not throw a
        // shadow of the character it outlines.
        db->setReceiveShadows(false);
        mMaterials[++mNextMaterialId] = rec;
        return mNextMaterialId;
    } JAH_CATCH(mError, 0);
}

bool OgreScene::setUnlitMaterial(MaterialId id, const Colour &c) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end() || !it->second.unlit) return false;
    JAH_TRY {
        // The skinned silhouette's colour lives in a Pbs datablock's EMISSIVE
        // (createOutlineMaterial), so the one setter the host calls has to know
        // both shapes — a live outline-colour change must reach either.
        if (it->second.pbsBacked) {
            auto *pbs = static_cast<Ogre::HlmsPbsDatablock *>(
                hlmsFor(it->second)->getDatablock(Ogre::IdString(it->second.datablockName)));
            if (!pbs) return false;
            pbs->setEmissive(Ogre::Vector3(c.r, c.g, c.b));
            return true;
        }
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsFor(it->second)->getDatablock(Ogre::IdString(it->second.datablockName)));
        if (!db) return false;
        db->setColour(toOgre(c));
        return true;
    } JAH_CATCH(mError, false);
}

}}}  // namespace jahshaka::engine::detail
