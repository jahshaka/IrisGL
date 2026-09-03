/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef SCENE_H
#define SCENE_H

#include <QList>
#include "irisglfwd.h"
#include "document/assets/texture2d.h"
#include "document/scenegraph/shadowmap.h"
#include "core/geometry/frustum.h"

// temp
#include <QJsonObject>

class QMediaPlayer;
class QMediaPlaylist;

namespace iris
{

class Environment;

enum class SceneRenderFlags : int
{
    Vr = 0x1
};

struct PickingResult
{
    iris::SceneNodePtr hitNode;
    QVector3D hitPoint;

    float distanceFromStartSqrd;
};

enum class SkyType : int
{
	SINGLE_COLOR = 0,
	CUBEMAP,
	EQUIRECTANGULAR,
	GRADIENT,
	MATERIAL,
	REALISTIC
};

// Global illumination (world panel). Values are serialized by ordinal-stable
// string names in SceneWriter/SceneReader, not by these ints.
enum class GiMode : int
{
	OFF = 0,
	INSTANT_RADIOSITY,
	VCT,
	VCT_PCC_HYBRID
};

enum class GiQuality : int
{
	LOW = 0,
	MEDIUM,
	HIGH
};

struct SkyRealistic
{
	float luminance;
	float reileigh;
	float mieCoefficient;
	float mieDirectionalG;
	float turbidity;
	float sunPosX;
	float sunPosY;
	float sunPosZ;

	// --- sun position, in the terms a user can reason about --------------
	// sunPos* stays the stored truth (old scenes keep working), but the model
	// only ever uses two things from it: the NORMALIZED direction, and sunPosY
	// divided by 450000 (the `sunfade` day/night term). So the vector is kept
	// at that radius and azimuth/elevation are exact round-trips of it.
	// Azimuth: degrees clockwise from +Z toward +X. Elevation: degrees above
	// the horizon (negative = below it, where sunfade finally does something).
	static constexpr float kSunRadius = 450000.0f;
	void  setSunAngles(float azimuthDegrees, float elevationDegrees);
	float sunAzimuth() const;      ///< [0, 360)
	float sunElevation() const;    ///< [-90, 90]

	/// The ONE set of starting values: iris::Scene's constructor and every
	/// per-key deserializer default read them from here.
	static SkyRealistic defaults();
};

class Scene: public QEnableSharedFromThis<Scene>
{
    QSharedPointer<Environment> environment;

public:
    CameraNodePtr camera;
    SceneNodePtr rootNode;

    QSharedPointer<Environment> getPhysicsEnvironment() {
        return environment;
    }

    /*
     * This is the default viewer that the scene
     * will use when playing in vr mode
     */
    ViewerNodePtr vrViewer;

    QHash<QString, LightNodePtr> lights;
    /// Every DecalNode in the scene, keyed by guid — the picker and the
    /// engine-side budget check walk this rather than the whole tree.
    QHash<QString, DecalNodePtr> decals;
	QHash<QString, MeshNodePtr> meshes;
	QHash<QString, ParticleSystemNodePtr> particleSystems;
	QHash<QString, ViewerNodePtr> viewers;
	QHash<QString, SceneNodePtr> nodes;

    QColor clearColor;
    bool renderSky;
    Texture2DPtr skyTexture;
    QColor skyColor;
    QColor ambientColor;
	QColor gradientTop;
	QColor gradientMid;
	QColor gradientBot;
	float gradientOffset;

    // Fog properties. The model is EXPONENTIAL (jahshaka::engine::FogDesc):
    // transmittance = 2^(-distance * fogDensity), times a second, height-varying
    // layer of the same colour. fogStart/fogEnd are the retired LINEAR pair, kept
    // so old scenes keep loading and round-tripping: together they still derive
    // the density when a scene predates fogDensity, and fogStart has no meaning
    // of its own any more (the World panel greys it out).
    QColor fogColor;
    float fogStart;
    float fogEnd;
    bool fogEnabled;
    float fogDensity;          // per world unit, exp2
    float fogHeightDensity;    // 0 = no height layer
    float fogHeightFalloff;    // per world unit; larger = thins out faster with altitude
    float fogHeightLevel;      // world Y at which fogHeightDensity applies
    float fogBreakMinBrightness;   // luminance where bright pixels start resisting the fog
    float fogBreakFalloff;         // how fast they do; 0 = pure exponential fog

    /// The exponential density an old LINEAR start/end pair maps to: the two
    /// curves are matched where the eye reads fog, at the HALF-fogged distance.
    /// Linear fog is 50% at (start + end) / 2; exponential fog is 50% at 1/density.
    ///
    /// Matching the far end instead (density = 4.32/end, i.e. 95% fogged exactly
    /// where the linear fog became total) was tried first and rejected on the
    /// shipped samples: it washes their SUBJECTS — 55% of the Physics red pipe,
    /// 24% of the teapot's brightness — because exponential fog, unlike linear,
    /// starts at the camera. This mapping leaves the subjects where they were and
    /// still fades the far ground away.
    static float fogDensityFromLinear(float start, float end) {
        return 2.0f / qMax(start + end, 0.001f);
    }

    // global illumination (world panel; rendered by the engine viewport only).
    // giBounds min == max means "automatic" (scene bounds + margin).
    GiMode giMode;
    GiQuality giQuality;
    QVector3D giBoundsMin;
    QVector3D giBoundsMax;
    QString giLightGuid;       // driving light for Instant Radiosity; empty = auto
    int giNumBounces;          // 1..4
    bool giAutoRefresh;        // editor: re-solve automatically on edits
    QVector3D giPccGrid;       // hybrid: reflection-probe counts per world axis (1..8 each)

    // anti-aliasing: MSAA sample count for the scene's viewport — 1 (off), 2, 4
    // or 8 (rendered by the engine viewport only; the driver may clamp).
    int antiAliasing;

    // shadow-map resolution for the WHOLE scene (VISUAL_PARITY_SPEC item 2,
    // option A). The renderer has ONE shadow atlas whose sizes derive from a
    // single base value, so the per-light `shadowMap->resolution` is only a
    // request: SceneMirror pushes the largest one. This field OVERRIDES that
    // derivation. 0 = "Auto" (derive from the lights, the historical
    // behaviour); otherwise 256..8192 pixels.
    int shadowResolution;

    // Shadow FILTER quality for the whole scene (POST_CHAIN_SPEC.md §9.3).
    // The renderer has ONE global PCF filter, so the per-light ShadowMapType is
    // only a request and SceneMirror pushes the softest one; this field
    // OVERRIDES that derivation exactly the way shadowResolution overrides its
    // own. -1 = Auto (derive from the lights, the historical behaviour);
    // 0 = Hard (PCF 2x2), 1 = Soft (4x4), 2 = VerySoft (6x6).
    int shadowFilterTier;

    // ---- World Modes (POST_CHAIN_SPEC.md §9) --------------------------------
    // A scalability tier for the whole scene. -1 = Custom (no tier: the fields
    // below are whatever the user/document set them to), 0 = Low, 1 = Medium,
    // 2 = High, 3 = Epic. Resolution is WRITE-THROUGH: setting a mode writes the
    // tier value into each backing field (antiAliasing, shadowResolution,
    // shadowFilterTier, giMode, giQuality, skyBakeResolution, ambientFromSky,
    // ...) EXCEPT rows listed in worldOverrides, so every existing consumer —
    // the mirror, the serializer, the panels, the verbs — keeps reading the one
    // field it always read. The invariant: a backing field is always the
    // RESOLVED value.
    int worldMode;
    // { rowId: value } — the rows the user pinned. Overrides survive mode
    // switches by design (owner requirement).
    QJsonObject worldOverrides;

    float gravity;
    bool shadowEnabled;

	SkyType skyType;
	SkyRealistic skyRealistic;

	// Equirect width the analytic (realistic) sky is CPU-baked at; the height is
	// half of it. 256 is the historical value; 512/1024 trade bake time for a
	// sharper sun disc on big displays (VISUAL_PARITY_SPEC item 1).
	int skyBakeResolution;

	// Sky-driven ambient/diffuse IBL (VISUAL_PARITY_SPEC item 3b). ON by owner
	// decision: with a textured/analytic sky the ambient hemisphere colours come
	// from the sky itself (cosine-weighted upper/lower averages) instead of the
	// flat `ambientColor` — a red sky reddens what it lights. Single-colour skies
	// have nothing to integrate and always use `ambientColor`.
	bool ambientFromSky;

    QString skyGuid;
    QString ambientMusicGuid;

	QJsonObject skyDataSingleColor;
	QJsonObject skyDataRealistic;
	QJsonObject skyDataGradient;
	QJsonObject skyDataEqui;
	QJsonObject skyDataCubemap;
	QJsonObject skyDataMaterial;

	QMap<QString, QJsonObject> skyData;

	void setWorldGravity(float gravity);

    QString skyBoxTextures[6];

    /*
     * customizations that can be passed in and applied to a scene. ideally these
     * should or can be GLOBAL but a scene is the highest prioritized obj atm...
     * @future maybe have a __GlobalWorldSettings__ object?
     * @future todo could include camera speed, motion blur px, clipping (near/far plane) pos
     */
    int outlineWidth;
    QColor outlineColor;

	// time counter to pass to shaders that do time-based animation
	float time;

	// The last absolute animation time, as handed to updateSceneAnimation.
	float animTime = 0.0f;

	// needed for playing music
	QMediaPlayer* mediaPlayer;
	// a playlist is needed to play looping sounds
	QMediaPlaylist* playList;
	QString ambientMusicPath;
	float ambientMusicVolume;

    Scene();
public:
    static ScenePtr create();

    /**
     * Returns the scene's root node. A scene should always have a root node so it should be assumed
     * that the returned value is never null.
     * @return
     */
    SceneNodePtr getRootNode() {
        return rootNode;
    }

	QStringList skyTypeToStr = {
		"SingleColor",
		"Cubemap",
		"Equirectangular",
		"Gradient",
		"Material",
		"Realistic"
	};

    void setSkyTexture(Texture2DPtr tex);
    void setSkyTextureSource(QString src) {
        skyTexture->source = src;
    }

	float getRunningTime()
	{
		return time;
	}

    QString getSkyTextureSource();
    void clearSkyTexture();
    void setSkyColor(QColor color);
    void setAmbientColor(QColor color);

	void setAmbientMusic(QString path);
	void stopPlayingAmbientMusic();
	void startPlayingAmbientMusic();
	void setAmbientMusicVolume(float volume);

    void updateSceneAnimation(float time);
    /// The last time updateSceneAnimation was given.
    ///
    /// The document owns the CLOCK — that is the half of animation it keeps
    /// after the clip evaluator moved to the engine. The mirror pushes this
    /// value as each active clip's ABSOLUTE time; it never advances a clip
    /// relatively, because a relative clock makes every pose assertion
    /// order-dependent (and a scrub backwards impossible to reason about).
    float animationTime() const { return animTime; }
    void update(float dt);

    void rayCast(const QVector3D& segStart,
                 const QVector3D& segEnd,
                 QList<PickingResult>& hitList,
			     uint64_t pickingMask = 0,
				 bool allowUnpickable = false);

    void rayCast(const QSharedPointer<iris::SceneNode>& sceneNode,
                 const QVector3D& segStart,
                 const QVector3D& segEnd,
                 QList<iris::PickingResult>& hitList,
				 uint64_t pickingMask = 0,
				 bool allowUnpickable = false);

	ViewerNodePtr getActiveVrViewer() { return vrViewer; }
	void setActiveVrViewer(ViewerNodePtr viewer) { this->vrViewer = viewer; }

    /**
     * Adds node to scene. If node is a LightNode then it is added to a list of lights.
     * @param node
     */
    void addNode(SceneNodePtr node);

    /**
     *  Removes node from scene. If node is a LightNode then it is removed to a list of lights.
     * @param node
     */
    void removeNode(SceneNodePtr node);

    /**
     * Sets the active camera of the scene
     * @param cameraNode
     */
    void setCamera(CameraNodePtr cameraNode);

	/*
	Return scene's active camera
	*/
	iris::CameraNodePtr getCamera() { return camera; }

    /**
     * Sets the viewport stencil width
     * @param width
     */
    void setOutlineWidth(int width);

    /**
     * Sets the viewport stencil color
     * @param color
     */
    void setOutlineColor(QColor color);

    void cleanup();
};

}


#endif // SCENE_H
