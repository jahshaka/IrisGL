/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MATERIAL_H
#define MATERIAL_H

#include "irisglfwd.h"
#include "document/materials/renderstates.h"

#include <QtGlobal>
#include <atomic>

namespace iris
{
struct RenderLayer
{
	enum Value {
		Background = 1000,
		Opaque = 2000,
		AlphaTested = 3000,
		Transparent = 4000,
		Overlay = 5000,
		Gizmo = 6000
	};
};

struct MaterialTexture {
    Texture2DPtr texture;
    QString name;
};

// Document-side material base: a property bag, texture map and render states.
// The GL half (shader program binding, uniform upload) died with the legacy
// renderer at step 14; the GLSL SOURCE half followed it at HLMS_ADOPTION P3
// (2026-09-07) - nothing ever read the stored text back, so the carrier
// (iris::Shader), the include-expanding loader and app/shaders/ are gone.
// The engine mirror translates these fields into engine materials.
class Material
{
public:
    int renderLayer;

    QMap<QString, Texture2DPtr> textures;

    // Editor-facing parameter list. Declared on the base so any material can be
    // rendered by the material property panel.
    QList<Property*> properties;

    /// Display name, and the asset/preset GUID this material came from.
    ///
    /// Both used to live on CustomMaterial ALONE, which is why "the base
    /// Material carries no name" turns up as an apology in several places — a
    /// PbrMaterial simply could not be named, so panels, exporters and the
    /// scripting surface had to special-case which class they were holding.
    /// With one material class (HLMS_ADOPTION P4b) they belong here.
    QString name;
    QString guid;
    void    setName(const QString &n) { name = n; }
    QString getName() const { return name; }
    void    setGuid(const QString &g) { guid = g; }
    QString getGuid() const { return guid; }

    // Applies a value by property name. Virtual so the property panel and the
    // scene reader can drive any material without knowing its concrete type.
    // Default is a no-op: materials with no editable parameters ignore it.
    virtual void setValue(const QString& name, const QVariant& value) { Q_UNUSED(name); Q_UNUSED(value); }

    // ---- THE CHANGE MARK (SPECS/DIRTY_SET_MIRROR_SPEC.md §3.6) ------------
    //
    // A MATERIAL edit moves no NODE, so the node-level dirty set cannot see
    // one: the panel writes a colour and the object it paints never changed.
    // This is the material's own half of the same signal.
    //
    // TWO NUMBERS, and they answer different questions. `revision()` is THIS
    // material's — the mirror's per-material memo keys its validity on it.
    // `globalRevision()` is "did ANY material in the process change", which is
    // what lets a still frame answer the whole question with ONE relaxed
    // atomic read instead of a per-material compare: the editor makes a
    // material per primitive, so an 8,404-node scene is 8,404 materials and a
    // per-material poll is the very cost this design exists to remove.
    //
    // THE FINGERPRINT SURVIVES AS THE ORACLE, not as the fast path
    // (SceneMirror::materialFingerprint): a counter is only as good as the
    // writer that remembers to bump it, so the mirror's verifier re-reads the
    // fields of a few materials a frame and COUNTS anything this missed.
    quint32 revision() const { return mRevision; }
    /// "Something in this material changed." Called by every setter below and
    /// by host code that writes a field by hand.
    void touch()
    {
        ++mRevision;
        sGlobalRevision.fetch_add(1, std::memory_order_relaxed);
    }
    /// How many material writes this process has made, ever. Relaxed: the only
    /// requirement is that a change moves it, never that it orders anything.
    static quint64 globalRevision()
    {
        return sGlobalRevision.load(std::memory_order_relaxed);
    }

    bool acceptsLighting;
    RenderStates renderStates;

    Material() {
        acceptsLighting = true;
        // Was left uninitialised. RenderList copies this straight onto the render
        // item (renderlist.cpp:39), so a material that never called
        // setRenderLayer() carried a garbage layer into the render list.
        // The retired CustomMaterial masked it by always setting one;
        // DefaultMaterial and any new subclass did not. Default to the layer it
        // used for "opaque", so an unconfigured material sorts with ordinary
        // geometry.
        renderLayer = RenderLayer::Background;
    }

    virtual ~Material() {}

    void setRenderLayer(int layer) {
        this->renderLayer = layer;
    }

	// setter for render states
	void setBlendState(const iris::BlendState& blendState);
	void setRasterizerState(const iris::RasterizerState& rasterState);
	void setDepthState(const iris::DepthState& depthState);
	iris::RenderStates getRenderState() { return renderStates; }

	// Preprocessor-define flags (only "SKINNING_ENABLED", set by MeshNode).
	// They used to be forwarded to the GLSL carrier; with that gone nothing
	// reads them back. Kept as a bare set - recorded debt, not a capability.
	bool isFlagEnabled(QString flag);
	void enableFlag(QString flag);
	void disableFlag(QString flag);

    /**
     * Adds texture to the material by name
     * If the material already contains the texture, it wil be replaced
     * @param name name of the texture uniform in the shader
     * @param textures texture pointer
     */
    void addTexture(QString name,Texture2DPtr textures);

    /**
     * Removes texture from material
     * @param name
     */
    void removeTexture(QString name);

    /// A FULL, INDEPENDENT COPY of this material — what a duplicated node
    /// carries. Pure because the base cannot copy what it does not know: this
    /// used to return a BLANK base Material, and MeshNode::createDuplicate
    /// handed that to every copy — Ctrl+D, Alt+drag, node.duplicate, the
    /// outliner menu — so every duplicate rendered the mirror's neutral grey
    /// fallback (RENDER_PIPELINE_AUDIT 3.2: original (86,2,2), copy
    /// (70,70,70); `material.get(copy)` returned {}). A subclass that forgets
    /// to implement it now fails to compile instead of losing the look.
    ///
    /// A COPY, NEVER A SHARED POINTER: a material belongs to ONE node. The
    /// panel and material.set edit a node's material in place, material.apply
    /// gives every mesh "its own material instance", and a saved scene writes
    /// one material per node — so a shared pointer would make an edit to the
    /// copy repaint the original, and would split back into two materials the
    /// first time the scene was saved and reopened.
    virtual MaterialPtr duplicate() const = 0;

protected:
	QSet<QString> flags;

private:
    quint32 mRevision = 1;
    static std::atomic<quint64> sGlobalRevision;
};

}

#endif // MATERIAL_H
