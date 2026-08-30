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
// renderer at step 14 - the engine mirror translates these fields into engine
// materials.
class Material
{
public:
    int renderLayer;
	// Shader source text (shadergraph / .effect files); never compiled here.
	ShaderPtr shader;
	ShaderPtr shadowShader;

    QMap<QString, Texture2DPtr> textures;

    // Editor-facing parameter list. Declared on the base so any material can be
    // rendered by the material property panel, not just CustomMaterial.
    QList<Property*> properties;

    // Applies a value by property name. Virtual so the property panel and the
    // scene reader can drive any material without knowing its concrete type.
    // Default is a no-op: materials with no editable parameters ignore it.
    virtual void setValue(const QString& name, const QVariant& value) { Q_UNUSED(name); Q_UNUSED(value); }

    bool acceptsLighting;
    RenderStates renderStates;

    Material() {
        acceptsLighting = true;
        // Was left uninitialised. RenderList copies this straight onto the render
        // item (renderlist.cpp:39), so a material that never called
        // setRenderLayer() carried a garbage layer into the render list.
        // CustomMaterial masked it by always setting one; DefaultMaterial and any
        // new subclass did not. Default to the layer CustomMaterial uses for
        // "opaque", so an unconfigured material sorts with ordinary geometry.
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

	void setShader(ShaderPtr shader);
	void setShadowShader(ShaderPtr shader);

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

    // Loads the two files as shader source text and stores them on `shader`.
    void createProgramFromShaderSource(QString vsFile, QString fsFile);

	virtual MaterialPtr duplicate() {
		return MaterialPtr(new Material());
	}

	static MaterialPtr fromShader(ShaderPtr shader);

protected:
	QSet<QString> flags;
};

}

#endif // MATERIAL_H
