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

#include "core/math/vec.h"
#include <QList>
#include "irisglfwd.h"
#include "document/assets/texture2d.h"
#include "document/scenegraph/nodegraph.h"
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
    iris::Vec3 hitPoint;

    float distanceFromStartSqrd;
    /// The TriMesh triangle that was hit. Reported since both ray walks became
    /// one implementation (audit F13): this half of the pair used to drop it
    /// while the other half depended on it.
    int triangleIndex = -1;
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
    /// Every scene-graph CameraNode, keyed by guid (CAMERAS_SPEC §3). NOT the
    /// editor camera: `camera` above is the viewport's virtual explorer and is
    /// never a child of the root, so it is never in here.
    QHash<QString, CameraNodePtr> cameras;
	QHash<QString, SceneNodePtr> nodes;

    /// Socket attachments (CAMERAS_SPEC §5), keyed by the SOCKET OWNER's guid:
    /// owner guid -> every node currently riding one of that owner's sockets.
    ///
    /// Keyed by owner and not by rider on purpose — SocketResolver reads a
    /// rig's posed bones ONCE per owner however many things hang off it, and
    /// this is the grouping that makes that free. Maintained by addNode /
    /// removeNode / attachToSocket / detachFromSocket; never serialized (the
    /// attachment lives on the RIDER, which is what the file carries).
    QHash<QString, QList<SceneNodePtr>> socketAttachments;

    /// The camera PLAY renders through (CAMERAS_SPEC D6). Empty = the free
    /// viewer, which is what every scene written before cameras existed means.
    /// Serialized with the scene; resolved through `cameras` above.
    QString activeCameraGuid;

    /// Play state (see setPlaying). Runtime only — never written to the file.
    bool playing = false;

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
    iris::Vec3 giBoundsMin;
    iris::Vec3 giBoundsMax;
    QString giLightGuid;       // driving light for Instant Radiosity; empty = auto
    int giNumBounces;          // 1..4
    bool giAutoRefresh;        // editor: re-solve automatically on edits
    iris::Vec3 giPccGrid;       // hybrid: reflection-probe counts per world axis (1..8 each)

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

    // Particle simulation clock (PARTICLES_FX2_SPEC.md §10.3). 1 = real time,
    // 0 = frozen, 2 = double speed. The DOCUMENT owns the clock and the ENGINE
    // simulates — the same split the animation migration settled on.
    //
    // It lives on the SCENE and not on the emitter, because the renderer has
    // exactly ONE frame-time source for the whole process: there is no per-node
    // and, strictly, no per-scene particle clock to push. Scene-level is the
    // finest granularity that is not a lie, and the last scene whose
    // applyEnvironment runs owns it — the same accepted compromise as the
    // process-wide GI binding. Offscreen thumbnail and preview scenes never call
    // applyEnvironment, so they do not fight the editor for it.
    float particleTimeScale;

    // ---- Post-processing chain (POST_CHAIN_SPEC.md phases 3-7) --------------
    // Per scene, pushed to the ENGINE VIEWPORT by SceneMirror. Offscreen views
    // (thumbnails, previews, every pixel suite) ignore all of it by
    // construction, which is what keeps their colours exact.
    bool  hdrEnabled;        ///< float scene target + filmic tonemap + auto exposure
    float exposure;          ///< auto-exposure midpoint; used as e^(exposure-2),
                             ///< so +0.69 is one doubling (NOT stops)
    bool  bloomEnabled;      ///< highlight bloom; rides the HDR node, needs hdrEnabled
    float bloomThreshold;    ///< where the bright pass starts, in tonemapper units
    bool  ssaoEnabled;
    float ssaoScale;         ///< AO buffer resolution factor (0.5 or 1.0)
    float ssaoPower;         ///< contrast of the occlusion term
    float ssaoRadius;        ///< world-space reach, in metres
    int   smaaPreset;        ///< -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra
    int   ssrMode;           ///< 0 off, 1 half-res rays, 2 HQ
    /// 0 off, 1 AUTO (the chain gains its refraction nodes only while the scene
    /// actually contains a refractive material — cost when unused is zero),
    /// 2 always on.
    int   refractionsMode;
    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md §6) -----------------
    // How many mirror planes may re-render the scene. THE most expensive dial
    // in the world: each active plane is a whole extra scene render every
    // frame. 0 = off.
    //
    // -1 = "follow the world mode" and is the only negative value: it is the
    // state of a scene that has never had a mode applied and never had the row
    // pinned. Everything that applies a mode writes a concrete 0..8 here (the
    // write-through invariant in services/worldmodes.h), so -1 never survives a
    // mode switch. SceneMirror — which is IrisGL and cannot see the tier table —
    // reads any negative value as OFF, which is what "never set" means anyway.
    int planarReflectionBudget;
    // Edge of each plane's reflection target in pixels; 256..2048. 0 = derive
    // it from the resolved BUDGET, which is how the world-mode tiers reach it
    // without needing rows of their own: budget >= 2 (Epic) means 1024, budget
    // 1 (High) means 512. SceneMirror owns that derivation.
    int planarReflectionResolution;
    // Shadows INSIDE the reflections, which cost a private half-resolution
    // shadow atlas per plane. 0 = off, 1 = on, -1 = derive from the budget the
    // same way (Epic's 2 planes get shadows, High's 1 does not).
    int planarReflectionShadows;

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

	// needed for playing music — nullptr until the first startPlayingAmbientMusic();
	// building one costs an audio-device probe, so it is NOT built in the ctor
	// (STABILITY_PROGRAM_SPEC Lane 6a; see scene.cpp ensureMediaPlayer()).
	QMediaPlayer* mediaPlayer;
	void ensureMediaPlayer();
	// a playlist is needed to play looping sounds
	QMediaPlaylist* playList;
	QString ambientMusicPath;
	float ambientMusicVolume;

    Scene();
public:
    /// Scene had no destructor at all until the deep audit of 2026-09 (area 3):
    /// it did not need one while `SceneNode::scene` was a QSharedPointer,
    /// because that cycle meant no Scene was ever destroyed in the first place.
    /// Now that the back-references are weak, this runs — and it runs cleanup()
    /// so that a scene dropped WITHOUT an explicit close (every offscreen /
    /// preview / test scene) releases its node registries exactly like one that
    /// was closed properly.
    ~Scene();

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

    // ---- the scene-graph binding (SPECS/SCENEGRAPH_SPEC.md D2) ------------
    /// The Ogre scene manager this document's ONE tree lives in. A scene starts
    /// in the process-wide STAGING manager (which renders nothing) and is moved
    /// into an engine scene's manager the moment a SceneMirror binds it — that
    /// move is what lets the engine read the document's transforms directly
    /// instead of having them pushed at it every frame (audit F1).
    graph::SceneHandle graphScene() const { return mGraphScene; }
    /// Rebuilds the whole tree inside `target`. Passing the staging handle (or
    /// nothing) UNBINDS: SceneMirror does that before it lets go of a document,
    /// because an engine scene may be destroyed at any time afterwards and the
    /// document's handles must not be inside it when that happens.
    void setGraphScene(graph::SceneHandle target);
    /// A subtree that has just left this scene's tree but is still ALIVE (the
    /// undo stack holds deleted nodes — audit §3.3). It stays in this scene's
    /// scene manager, so setGraphScene has to take it along; without that it
    /// would be left inside a manager the engine is free to destroy, and an
    /// undo after a world switch would walk dangling handles.
    void rememberDetached(const SceneNodePtr &node);

    /// One of the TWO ENTRY POINTS onto iris::picking::raycastMeshes (the other
    /// is Studio's ScenePicker) — audit F13's duplicate walk is gone, and with
    /// it the recursive `->children` descent this used to do. The broad phase
    /// is Ogre's RaySceneQuery; the triangle test is ours.
    void rayCast(const iris::Vec3& segStart,
                 const iris::Vec3& segEnd,
                 QList<PickingResult>& hitList,
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
     * Sets the scene's VIEWPORT camera — the editor's virtual explorer, or the
     * camera a preview scene renders through. NOT the "active camera" of
     * CAMERAS_SPEC D6: that one is a scene-graph node, named by guid below.
     * @param cameraNode
     */
    void setCamera(CameraNodePtr cameraNode);

	/*
	Return scene's viewport camera (see setCamera).
	*/
	iris::CameraNodePtr getCamera() { return camera; }

    // ---- the ACTIVE camera (CAMERAS_SPEC D6) -----------------------------
    //
    // A scene-graph camera that PLAY renders through. Null/empty means the
    // free viewer, which is the behaviour every scene had before cameras
    // existed. Switching it at runtime from a script is camera cuts v0.

    /// Points play at a scene camera. An empty guid clears it (free viewer).
    /// A guid that names no camera in this scene is REFUSED — returns false and
    /// leaves the previous choice alone, because silently rendering through the
    /// wrong camera is exactly the failure a caller cannot see.
    bool setActiveCamera(const QString &guid);
    /// The active camera node, or null when there is none / the guid no longer
    /// resolves (the camera was deleted with the guid still recorded).
    CameraNodePtr getActiveCamera() const;
    QString getActiveCameraGuid() const { return activeCameraGuid; }

    // ---- sockets (CAMERAS_SPEC §5, D9; see scenegraph/socket.h) ----------
    //
    // The scene owns the ATTACHMENT half — which node rides whose socket — the
    // way it owns the active-camera choice: it is the only place that can
    // resolve a guid to a node and therefore the only place that can validate
    // one. The sockets themselves live on the owning MeshNode.

    /// Points `node` at `ownerGuid`'s socket `socketName`, and registers it so
    /// SocketResolver drives it. Refused (false, `error` set when given) when
    /// the owner does not exist, is not a mesh, has no such socket, or sits
    /// inside `node`'s own subtree (which would be a feedback loop). The
    /// node's transform is not touched here — the next resolve() does that.
    bool attachToSocket(const SceneNodePtr &node, const QString &ownerGuid,
                        const QString &socketName, QString *error = nullptr);
    /// Stops driving `node`. It keeps the pose it was last resolved to.
    /// False when it was not attached.
    bool detachFromSocket(const SceneNodePtr &node);
    /// Registry maintenance for the paths that set the attachment directly
    /// (the reader, node duplication) — addNode already calls the first one.
    void registerSocketAttachment(const SceneNodePtr &node);
    void unregisterSocketAttachment(const SceneNodePtr &node);

    /// Whether the scene is being PLAYED (editor play-in-place or the player
    /// view). Runtime state, never serialized: PlayBack owns it, and
    /// SceneMirror::applyCamera reads it to decide whether the active camera
    /// takes the view. Editing must NOT route through the active camera — the
    /// main viewport stays the explorer until phase 3's pilot mode.
    void setPlaying(bool playing) { this->playing = playing; }
    bool isPlaying() const { return playing; }

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

private:
    /// Where this document's tree currently lives. Never null once an
    /// Ogre::Root exists; see setGraphScene.
    graph::SceneHandle mGraphScene = nullptr;
    /// Subtrees detached from this scene and still alive — see
    /// rememberDetached. WEAK: this list must never be the reason a deleted
    /// node stays alive; expired entries are pruned as they are found.
    QList<SceneNodeWPtr> mDetached;
};

}


#endif // SCENE_H
