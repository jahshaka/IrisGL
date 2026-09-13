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
#include <functional>
#include <QList>
#include <QStringList>
#include <QVector>
#include "irisglfwd.h"
#include "document/assets/texture2d.h"
#include "document/input/possession.h"
#include "document/scenegraph/nodegraph.h"
#include "document/scenegraph/nodedirtyset.h"
#include "document/scenegraph/shadowmap.h"
#include "document/scenegraph/simulationclock.h"
#include "core/geometry/frustum.h"

// temp
#include <QJsonArray>
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

/// THE ANALYTIC ("realistic") SKY's parameters — the ENGINE's own, since
/// SKY-GPU (2026-09-13).
///
/// The five Preetham dials this replaces (luminance, reileigh, mieCoefficient,
/// mieDirectionalG, turbidity) described a CPU bake that no longer exists: the
/// sky is Ogre's AtmosphereNpr, evaluated per pixel on the GPU, and these are
/// its parameters. There is no mapping from the old names and none is owed —
/// the arithmetic is different, not re-parameterised — so `world.sky realistic`
/// refuses them BY NAME and an old file's Realistic block reads as the
/// defaults below (no migrations, ever; the crud law).
struct SkyRealistic
{
	/// How much atmosphere the ray travels through — the blue's depth.
	float density;
	/// How fast the colour changes with altitude — the horizon's spread.
	float diffusion;
	/// The lowest point the sky is drawn at; raises the band in a sunset.
	float horizon;
	/// The sky's own colour before absorption, as a colour a user PICKS
	/// (decoded sRGB->linear at the boundary, like every other one).
	QColor skyColour;
	/// Multiplies the whole sky (HDR).
	float power;

	// THE SKY HAS NO SUN OF ITS OWN (SKY_LIGHT_SPEC.md §3, owner decision D15).
	// The analytic sky's sun DIRECTION comes from the scene's sun — the first
	// directional light, Scene::sunLight() — and nowhere else. The sunPosX/Y/Z
	// vector, setSunAngles/sunAzimuth/sunElevation and the panel's Azimuth /
	// Elevation sliders were the hack that let a sky carry a light; they are
	// deleted, along with the `skyDrivesSun` steering that pushed the light
	// around from them. A scene with a sky and no directional light bakes the
	// model's own night — correct, and the panel says so.

	/// The ONE set of starting values: iris::Scene's constructor and every
	/// per-key deserializer default read them from here.
	static SkyRealistic defaults();
};

class Scene: public QEnableSharedFromThis<Scene>
{
    QSharedPointer<Environment> environment;
    /// The one possession slot (§8.4). Owned like `environment` — created in
    /// the constructor, destroyed with the scene, never serialized.
    QSharedPointer<AvatarPossession> possession;

    /// WHAT CHANGED SINCE THE LAST FRAME (SPECS/DIRTY_SET_MIRROR_SPEC.md).
    /// Every node in this scene points at it (SceneNode::_setDirtySet, wired
    /// with scene membership) and SceneMirror is its one consumer. Runtime
    /// only, never serialized, and per SCENE rather than per process on
    /// purpose: a material preview and the editor mirror different documents
    /// on the same thread, and one list between them would hand each other's
    /// nodes to the wrong mirror.
    NodeDirtySet mDirtySet;

public:
    CameraNodePtr camera;
    SceneNodePtr rootNode;

    QSharedPointer<Environment> getPhysicsEnvironment() {
        return environment;
    }

    QHash<QString, LightNodePtr> lights;
    /// Every DecalNode in the scene, keyed by guid — the picker and the
    /// engine-side budget check walk this rather than the whole tree.
    QHash<QString, DecalNodePtr> decals;
	QHash<QString, MeshNodePtr> meshes;
	QHash<QString, ParticleSystemNodePtr> particleSystems;
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

    /// What PLAY does with this scene (AVATAR_LOCOMOTION_SPEC §8.5). SERIALIZED
    /// with the scene, beside `activeCameraGuid`, as a stable string. Read
    /// through getPlayMode(); the possession slot it arms is runtime only.
    ScenePlayMode playMode = ScenePlayMode::Explorer;

    QColor clearColor;
    bool renderSky;
    Texture2DPtr skyTexture;
    QColor skyColor;
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
    /// AERIAL PERSPECTIVE (SKY-GPU): the distance fog takes its colour from the
    /// ANALYTIC sky's own scattering for the direction each surface is seen
    /// from, instead of fogColor — so a far hill fades into the sky behind it
    /// and follows the sun. Needs the realistic sky (it IS that sky's model);
    /// with any other sky bound the engine keeps the authored colour. Off by
    /// default: fogColor is a colour a person picked.
    bool fogAtmosphere;

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
    //
    // THE LIT VOLUME IS THE RENDERER'S (owner decision D8, 2026-09-13): the
    // document carries no bounds at all any more — no min/max pin and no
    // ceiling on the automatic fit. The engine fits the volume to the scene's
    // content on every solve, and `world.giStatus()` reports what it decided,
    // which is the only reading anyone ever needed. The three fields that used
    // to live here (giBoundsMin, giBoundsMax, giAutoBoundsMax), their World
    // panel rows, the Fit button and world.fitGiBounds are deleted under the
    // CRUD law; an old scene that pinned a volume opens with the automatic one.
    GiMode giMode;
    GiQuality giQuality;
    QString giLightGuid;       // driving light for Instant Radiosity; empty = auto
    int giNumBounces;          // 1..4
    /// THE GI UPDATE BUDGET (FIX WAVE B1, 2026-09-07) — probe re-captures the
    /// renderer may spend per frame, and the single "is GI live?" switch.
    ///
    /// It replaces two fields that were asking the same question from opposite
    /// ends: `giAutoRefresh` (a bool: may the mirror re-solve when the scene
    /// changes?) and the old `giDynamicProbes` (an int: how many probes stay
    /// live?).
    /// Their combination had four states and only two of them meant anything.
    ///
    ///   0 = PAUSED. No probe re-captures and nothing auto-re-solves; GI shows
    ///       whatever it last built until world.refreshGi() asks for more. This
    ///       is exactly the old `giAutoRefresh = false`.
    ///   1 = the default, and a realtime editor: one probe's six faces per
    ///       frame (~2.1 ms Debug at Medium), so a grid of 18 probes is fully
    ///       refreshed in 18 frames and the probes covering whatever just moved
    ///       go first.
    ///   N = spend more per frame for less latency; the cost is linear.
    ///
    /// Documents written before the fix wave carry `giAutoRefresh` instead and
    /// map onto it (false -> 0, true -> 1); readers that still speak the old
    /// spelling (world.settings' `autoRefresh`) report `budget > 0`.
    int giUpdateBudget = 1;
    iris::Vec3 giPccGrid;       // hybrid: reflection-probe counts per world axis (1..8 each)
    // Hybrid probe-capture knobs (REFLECTIONS_ADOPTION_SPEC.md P3). Integrator
    // knobs, not quality-dial rows: they reach the engine through world.gi only
    // (SPEC §6 — "the panel stays the quality dial"). The two toggles are
    // TRI-STATE ints matching jahshaka::engine::GiToggle — -1 auto (derived
    // from giQuality), 0 off, 1 on — so a scene can pin either without pinning
    // the whole quality tier.
    int giProbeHdr = -1;          // HDR probe captures (RGBA16F); auto = High only
    int giProbeShadows = -1;      // shadowed probe captures; auto = High only
    float giProbeOverlap = 1.25f; // probe influence overlap (upstream's sample value)
    /// PROBE CAPTURE SIZE in pixels per cube face, 0 = follow the quality dial
    /// (128 Low, 256 Medium and High). The hybrid's single biggest VRAM lever:
    /// a probe costs 6 faces x size^2 x mips, so halving it quarters the grid
    /// (REFLECTION_PROBE_AUDIT §4.2). A Rayon-tiered row, like the technique
    /// and quality above — setting it in the World panel PINS it.
    int giProbeCaptureSize = 0;
    float giProbeSnapDeviation = 0.05f;  // shrink-fit snap-back tolerances: the pin's
    float giProbeSnapSidesMin = 0.25f;   // own ctor defaults, made explicit and ours
    float giProbeSnapSidesMax = 0.25f;
    /// VCT light-injection ray-march step scale AT REST (FIX WAVE B5).
    /// Integrator knob, verb-only (`world.gi({rayMarchStepScale})`), floor 1.0:
    /// bigger marches faster and starts losing contact shadows in the bounce,
    /// and upstream asserts below 1.0. The engine raises it on its own for the
    /// cheap in-motion re-injection only.
    float giRayMarchStepScale = 1.0f;
    /// DDGI — the irradiance-field diffuse layer (GI_UNIFIED_SPEC.md §4 P1).
    /// TRI-STATE, like giProbeHdr/giProbeShadows and for the same reason: -1
    /// auto, 0 off, 1 on. The Rayon tier (GI_UNIFIED_SPEC P2) RESOLVES it
    /// document-side and writes a concrete 0/1 through, exactly like giMode and
    /// giQuality (services/worldmodes.h — a backing field is always the
    /// resolved value), so -1 survives only in a scene no tier has ever been
    /// applied to. The engine reads a bare -1 as OFF; the reader never lets one
    /// reach it, because a document without a tier DERIVES one and -1 then
    /// means "the derived tier decides" (worldmodes::deriveRayonFromDocument,
    /// owner option (b) 2026-09-09: Medium and High are DDGI-fed, so the
    /// shipped vct+medium samples come up with the field on).
    /// Only meaningful in the VCT modes: the field is fed by the voxel volume.
    int giDdgi = -1;
    /// The DDGI diffuse INTENSITY. Ours, not upstream's: binding a field turns
    /// the voxel-cone diffuse OFF and replaces it with the probes' — which is
    /// smoother and leak-free — and upstream's IrradianceFieldSettings carries
    /// no brightness knob at all. 1.0 is the renderer's raw value and the
    /// calibrated default (measured at 86% of the VCT diffuse it replaces); the
    /// knob exists because the two terms are different integrals and a scene may
    /// want to trim one against the other.
    float giDdgiIntensity = 1.0f;
    /// THE DDGI AMBIENT SKY-VISIBILITY STRENGTH — the Rayon ambient fix
    /// (GI_UNIFIED_SPEC.md ADDENDUM CORRECTION). Inside a VCT volume the
    /// shader's own ambient term is gated off and the cone diffuse carried it
    /// instead; binding a field deletes that branch, so DDGI scenes lost their
    /// ambient (15-25% darker mid-ground on OPEN scenes; sealed rooms
    /// unaffected). The engine rebuilds it as the scene's SH ambient times a
    /// sky-visibility fraction read out of the field's own depth atlas, and
    /// this scales it: 1.0 = the honest reconstruction (default), 0 = the term
    /// removed entirely (the pre-fix behaviour, kept as a real setting because
    /// it is what makes the fix measurable). Only meaningful with a field
    /// bound. Clamped to [0, 8] on the way to the engine.
    float giDdgiAmbient = 1.0f;
    /// WHERE THE FIELD'S PROBES GET THEIR LIGHT (GI_UNIFIED_SPEC.md P3 "A2"):
    /// -1 auto (the tier's choice — voxel at every tier, Epic included; the
    /// raster feed costs 3.4-9 ms per probe in Debug, rayon2 S3, which is no
    /// default for anyone), 0 voxel cone tracing,
    /// 1 rasterised probe captures (six 32x32 scene renders per probe, under the
    /// same update budget; sees skinned/animated geometry the voxels cannot).
    /// An Advanced-only knob; never tier-written, so it has no registry row.
    int giDdgiSource = -1;
    /// RAYON — the user-facing quality tier for realtime global illumination
    /// (GI_UNIFIED_SPEC.md §2 / P2). 0 Low, 1 Medium, 2 High, 3 Epic.
    ///
    /// It is a REQUEST, never a second source of truth: the tier resolves
    /// WRITE-THROUGH into giMode / giQuality / giDdgi / giNumBounces
    /// (services/worldmodes.cpp, setRayon) exactly the way a World Mode resolves into its rows, so the
    /// mirror, the serializer, the engine and every existing verb keep reading
    /// the one field they always read. A field the user pinned deviates from
    /// the tier and survives tier switches, which is what makes the panel's
    /// Advanced section safe.
    ///
    /// WHETHER RAYON IS ON is `giMode != OFF` and nothing else — there is no
    /// second enable flag to disagree with the renderer. This field keeps the
    /// quality the scene would come back at, so turning Rayon off and on is not
    /// destructive. New scenes are born Epic (owner decision D2).
    int giTier = 3;
    /// MONOTONIC, never serialized: bumped by world.refreshGi() and by the
    /// Refresh button (REFLECTIONS_ADOPTION_SPEC.md P1d). The mirror compares it
    /// against the value it last acted on and re-solves the engine's GI once per
    /// bump. A serial rather than a bool because two refreshes in one frame must
    /// still be one re-solve, and because a bool would need a clearer — which is
    /// the mirror's job, not the caller's.
    quint64 giRefreshSerial = 0;
    /// The SAME shape for the cached point/spot shadow maps
    /// (ENGINE_CACHE_POLICY_SPEC P2): monotonic, never serialized, bumped by
    /// world.refreshShadows(). The mirror compares it against the value it last
    /// acted on and asks the renderer to re-render every cached map once per
    /// bump.
    quint64 shadowRefreshSerial = 0;

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

    // HOW MANY POINT/SPOT LIGHTS MAY HOLD A SHADOW MAP AT ONCE
    // (SPECS/SHADOW_TOOLING_SPEC.md §4.1). The renderer's atlas has room for a
    // fixed number of focused maps and Ogre fills them with the casters closest
    // to the camera, dropping the rest SILENTLY — which is why a scene with
    // three shadow-casting lamps used to show two shadows, and which two
    // changed as the camera moved.
    //
    // This is the CEILING the engine may grow to, not an allocation: the engine
    // counts the scene's casters and steps the count {2, 4, 8, 16} up to this
    // value. 0 = Auto, i.e. follow the World Mode tier's row, exactly as
    // shadowResolution's 0 does.
    int shadowMapBudget;

    // Particle time scale (PARTICLES_FX2_SPEC.md §10.3). 1 = the simulation
    // clock's own rate, 0 = frozen, 2 = double speed. The DOCUMENT owns the
    // clock (SimulationClock, below) and the ENGINE simulates — the same split
    // the animation migration settled on: every frame the host multiplies the
    // seconds the clock advanced by this and hands the product to the engine
    // as its frame delta (ENGINEERING_DEBT_SPEC A4.2).
    //
    // It lives on the SCENE and not on the emitter, because the renderer has
    // exactly ONE frame-time source for the whole process: there is no per-node
    // and, strictly, no per-scene particle clock to push. Scene-level is the
    // finest granularity that is not a lie, and the scene the active host
    // ticks owns it — the same accepted compromise as the process-wide GI
    // binding. Offscreen thumbnail and preview scenes push nothing, so they do
    // not fight the editor for it.
    float particleTimeScale;

    // ---- Post-processing chain (POST_CHAIN_SPEC.md phases 3-7) --------------
    // Per scene, pushed to the ENGINE VIEWPORT by SceneMirror. Offscreen views
    // (thumbnails, previews, every pixel suite) ignore all of it by
    // construction, which is what keeps their colours exact.
    bool  hdrEnabled;        ///< float scene target + filmic tonemap + auto exposure
    float exposure;          ///< auto-exposure midpoint; used as e^(exposure-2),
                             ///< so +0.69 is one doubling (NOT stops)
    /// The WINDOW auto-exposure may adapt within, around `exposure`. Both
    /// engine fields (PostFxDesc::exposureMin/Max) existed and were pushed at
    /// their hard-coded defaults; the document could not say otherwise, so the
    /// World > Post Process section could not offer them (fix wave 2026-09-07,
    /// item 8).
    ///
    /// WHAT min == max REALLY DOES (corrected by measurement, SS1 2026-09-13 —
    /// the old note here claimed it was "the deterministic setting the
    /// secondary-surface tonemap uses", and it is not): it clamps what the
    /// automatic exposure adapts TOWARDS to a single value, so the grade stops
    /// following the scene's content. It is still the AUTOMATIC chain — a
    /// temporal filter that takes about a second to arrive — and it lands
    /// nowhere near the constant the secondary surfaces grade with, which
    /// substitutes a 0.18 grey card for the measurement (measured on one floor
    /// region: 12.9 against 86.6). For a picture that is deterministic by
    /// construction, ask a screenshot for the "tonemap" or "scene" grade.
    float exposureMin;
    float exposureMax;
    bool  bloomEnabled;      ///< highlight bloom; rides the HDR node, needs hdrEnabled
    float bloomThreshold;    ///< where the bright pass starts, in tonemapper units
    /// How WIDE the ramp above that threshold is (ADDENDUM A-6). A width, not a
    /// second absolute threshold: the renderer clamps an inverted pair back up,
    /// so an absolute row could offer a state it silently refuses. 2.0 is what
    /// the engine hard-coded before this existed.
    float bloomKnee;
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
    /// DISTORTION (POST_LOOKS_SPEC.md §5.3): objects whose material's shading
    /// model is Distortion warp the image behind them. 0 off, 1 AUTO (the
    /// recommended default — the renderer grows the pass only while the scene
    /// actually holds such a material, so the cost when unused is exactly
    /// zero), 2 always on. Same three-state shape as refractionsMode, for the
    /// same reason.
    int   distortionMode;
    /// A global multiplier on every distortion material's own strength. 0 is
    /// inert and renders a byte-identical frame.
    float distortionStrength;

    // ---- The looks stack (POST_LOOKS_SPEC.md §4) ---------------------------
    // An ORDERED array of {id, enabled, params:{...}} — the LDR image filters
    // the renderer applies to the finished picture, entry 0 first. THE ARRAY
    // ORDER IS THE FRAME ORDER and is the whole authoring model: desaturating a
    // posterized image and posterizing a desaturated one are different
    // pictures, and nothing but this order says which one a scene means.
    //
    // Absent in every document written before this feature = an empty stack,
    // which is the renderer's byte-for-byte previous behaviour (no pass, no
    // texture, not merely a disabled stage).
    //
    // Kept as JSON rather than as a typed vector for the same reason
    // worldOverrides is: the catalogue grows, and a document written by a build
    // that knows a look this one does not must survive the trip. Every write
    // path goes through iris::normalizeLookStack (document/scenegraph/looks.h),
    // which is where the rules — known ids only, one instance per look, every
    // parameter present and clamped — are enforced exactly once.
    QJsonArray looks;
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
    // shadowFilterTier, giMode, giQuality,
    // ...) EXCEPT rows listed in worldOverrides, so every existing consumer —
    // the mirror, the serializer, the panels, the verbs — keeps reading the one
    // field it always read. The invariant: a backing field is always the
    // RESOLVED value.
    int worldMode;
    // { rowId: value } — the rows the user pinned. Overrides survive mode
    // switches by design (owner requirement).
    QJsonObject worldOverrides;

    // ---- OUTLINER FOLDERS (SCENEGRAPH_SPEC.md §6b) --------------------------
    // Folders are EDITOR ORGANISATION, never nodes: no transform, no place in
    // the hierarchy, invisible to the player, the exporters and the engine.
    // Membership is the per-node `folderPath` on the handle; this list is the
    // other half — the folders that exist even when nothing is in them, which
    // an implicit-from-membership model cannot express (Unreal's model exactly).
    //
    // Paths are "/"-separated and always NORMALISED (no leading/trailing slash,
    // no empty segment) — src/services/scenefolders.h owns that policy and is
    // the only thing that should write this list. Serialized in the project's
    // EDITOR section, beside editor.camera, never in the node format.
    QStringList folders;

    float gravity;
    bool shadowEnabled;

	SkyType skyType;
	SkyRealistic skyRealistic;

	// THE SUN PIN (SUN_AND_LIGHT_DEFAULTS Q1): the guid of the DIRECTIONAL
	// light the author has pinned as this scene's sun. Empty (the default) =
	// AUTOMATIC, i.e. the lowest forwardShadingPriority wins — which is what
	// every scene we ship stores.
	//
	// It used to mean "the light the sky's sun drives", and being one field for
	// two questions is why picking a sun and steering it could not be told
	// apart. The steering itself is GONE (D15): the sky follows the sun light,
	// never the other way round, so the pin is now only ever "which directional
	// light is the sun".
	//
	// An explicit guid on the SCENE, not a "driven by sky" flag on the light:
	// the coupling is a property of the world (there is exactly one sun), the
	// Database/no-Globals law wants explicit guids, and a guid survives the
	// scene-graph adoption unchanged — a per-light flag would have to be
	// re-homed with the light and would need a "which one wins" rule the
	// moment two lights carried it.
	//
	QString sunLightGuid;

	// ---- THE SUN DISC (SKY_LIGHT_SPEC.md §3, owner picks 2 and 4) -------
	// The bright disc drawn in the sky where the sun light points. ONE
	// mechanism, owned by the sun and not by the sky: it used to be a term
	// baked into the analytic sky's texels, which made it a property of one sky
	// type, at the bake's resolution, in a place no capture mask could exclude
	// it from.
	//
	// A SCENE setting, not a per-light row (owner, §193a: "we need a World
	// setting to hide the sun disc") — the disc is part of the world's picture,
	// like the sky it is drawn on. Its angular SIZE stays on the light
	// (LightNode::sunAngle), because that is a property of the sun itself.
	//
	// Drawn over EVERY sky type by default (owner pick 2). An image sky usually
	// has a sun PAINTED into it, so a disc that is not aimed at the painted one
	// shows two suns — the World-panel row's tooltip says so, and the answer is
	// to aim the light or switch the disc off.
	bool sunDiscVisible = true;
	/// Do reflection-probe captures contain the disc? FALSE by default (owner
	/// pick 4, both options): the sun's energy already reaches glossy surfaces
	/// through the directional light's own specular highlight, so capturing the
	/// disc as well paints a SECOND sun on everything the probes light.
	bool sunDiscInProbes = false;


	// ---- THE SUN -------------------------------------------------------
	// (SUN_AND_LIGHT_DEFAULTS_SPEC, owner decisions Q1/Q1e.) "Sun" is a UI
	// ROLE, not a type: there is one directional light type, and THE SUN is
	// the directional light with the lowest `forwardShadingPriority`. Nothing
	// stores "which one is the sun" — it is derived here, by the ONE resolver
	// every consumer asks, because three consumers each having their own rule
	// is exactly how they came to disagree with nobody being told.
	//
	// A SCENE WITH NO DIRECTIONAL LIGHT IS NORMAL (owner Q1c): Mirror Room and
	// Showroom 2 ship that way. sunLight() answers null, cleanly, and nothing
	// warns.

	/// The scene's sun, or null when it has no directional light.
	///   1. `sunLightGuid` when it names a live DIRECTIONAL light (an author's
	///      explicit pin, and what the sky's sun steers);
	///   2. otherwise the lowest `forwardShadingPriority`, ties broken by the
	///      lowest `nodeId` (creation order — deterministic across reloads, and
	///      the rule Ogre's own light-id sort agrees with by construction);
	///   3. otherwise none.
	LightNodePtr sunLight() const;

	/// Why sunLight() answered as it did: "pinned", "priority" or "none".
	/// The World panel prints it; `world.sun()` returns it.
	QString sunReason() const;

	/// Every DIRECTIONAL light in the scene, ordered by forwardShadingPriority
	/// then nodeId — so the first entry is the sun whenever nothing is pinned.
	QVector<LightNodePtr> directionalLights() const;

	/// Every directional light that is NOT the sun, in the same order. These
	/// light the scene and cast no shadow (one directional slot exists).
	QVector<LightNodePtr> secondaryDirectionals() const;

	/// The priority a directional light joining THIS scene should take: the
	/// lowest number no directional is using. The first directional gets 0.
	int nextForwardShadingPriority() const;

	// ---- THE SKY LIGHT -------------------------------------------------
	// (SKY_LIGHT_SPEC.md §2, owner decision D14.) Ambient is SKYLIGHT ONLY:
	// there is no flat World-panel ambient colour and no "Ambient From Sky"
	// switch any more. The scene's ambient is the sky's own cosine-convolved
	// integral, tinted and scaled by a LIGHT NODE of type LightType::Sky — and
	// a scene with no Sky Light has no ambient at all.
	//
	// The same shape as sunLight(): ONE resolver, asked by everything.

	/// The scene's skylight: the first VISIBLE LightType::Sky light by nodeId,
	/// or null. Hidden Sky Lights do not light (that is how a user turns the
	/// skylight off without deleting it, and how the duplicate issue clears).
	LightNodePtr skyLight() const;

	/// Why skyLight() answered as it did: "first", "none" or "allHidden".
	QString skyLightReason() const;

	/// Every LightType::Sky light in the scene, ordered by nodeId (creation
	/// order — deterministic across reloads, like directionalLights()).
	QVector<LightNodePtr> skyLights() const;

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
    /// The PRIMARY member's outline colour (EDITOR_MULTISELECT_SPEC D4 b, the
    /// Blender rule: the active object reads brighter than the rest of the
    /// selection). Only meaningful with MORE THAN ONE node selected — with a
    /// single selection there is nothing to distinguish, so the mirror draws
    /// `outlineColor` and a one-node selection is pixel-identical to what it
    /// was before this field existed. Invalid = "never set", and the mirror
    /// then lightens `outlineColor` itself.
    QColor outlinePrimaryColor;

	// The last absolute animation time, as handed to updateSceneAnimation.
	float animTime = 0.0f;

	// THE simulation clock (simulationclock.h): the one fixed grid physics,
	// animation, possession and the renderer's own simulation advance on.
	// Runtime only — never written to the file.
	SimulationClock clock;

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

    /// The scene's change collector. Handed to every node that joins the scene
    /// and consumed by SceneMirror::sync(); see NodeDirtySet.
    NodeDirtySet *dirtySet() { return &mDirtySet; }

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

    QString getSkyTextureSource();
    void clearSkyTexture();
    void setSkyColor(QColor color);

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

    /// ONE FRAME OF SIMULATED TIME (ENGINEERING_DEBT_SPEC A4.2). Hands `dt`
    /// seconds — the wall time the frame took, or a scripted step — to the
    /// simulation clock and runs the whole number of fixed steps that buys:
    /// per step, possession input, then Bullet and every avatar component
    /// (Environment::stepSimulation); once per frame, the animation pose at the
    /// clock's time (while playing), the rigid-body -> node copy, the follow
    /// camera and the camera matrices. Physics runs only while the environment
    /// is simulating (play, or the editor's Simulate); animation only while
    /// `playing`. A frame that buys no step changes nothing.
    ///
    /// Returns the simulated seconds this frame advanced (steps x the grid) —
    /// what the host hands the renderer as its frame delta, so particles and
    /// shader time stay on the same grid. Every host ticks through this, with
    /// no other clock: the editor viewport every driver frame, PlayBack in
    /// play-in-place and the player, editor.frame / player.frame with their dt.
    float advance(float dt);
    /// Derived state without time: the camera's matrices and the node walk —
    /// what a host does after placing a camera (previews, thumbnails, tests).
    /// The old `update(0)` idiom; advance() does the same after every frame
    /// that bought a step.
    void refresh();
    /// The clock itself: reset on play start/stop, read by scene.clock().
    SimulationClock &simulationClock() { return clock; }
    const SimulationClock &simulationClock() const { return clock; }

    // ---- the scene-graph binding (SPECS/SCENEGRAPH_SPEC.md D2) ------------
    /// The Ogre scene manager this document's ONE tree lives in. A scene starts
    /// in the process-wide STAGING manager (which renders nothing) and is moved
    /// into an engine scene's manager the moment a SceneMirror binds it — that
    /// move is what lets the engine read the document's transforms directly
    /// instead of having them pushed at it every frame (audit F1).
    graph::SceneHandle graphScene() const { return mGraphScene; }
    /// Registered by the mirror that OWNS this document's graph (the one whose
    /// engine scene the tree lives in). setGraphScene fires it exactly once,
    /// right BEFORE migrating the tree away, so the owner can release every
    /// engine-side object it hung off the document's nodes (particle systems,
    /// planar-reflection registrations, highlight shells, Items) while those
    /// nodes still exist. Without this, whichever mirror bound the document
    /// last silently took over the graph and the loser's engine objects kept
    /// pointers to destroyed nodes — the 2026-09-05 player→editor faults.
    void _setGraphEvacuationHook(std::function<void()> hook) { mGraphEvacuationHook = std::move(hook); }
    /// Rebuilds the whole tree inside `target`. Passing the staging handle (or
    /// nothing) UNBINDS: SceneMirror does that before it lets go of a document,
    /// because an engine scene may be destroyed at any time afterwards and the
    /// document's handles must not be inside it when that happens.
    void setGraphScene(graph::SceneHandle target);
    /// A subtree that has just left this scene's tree but is still ALIVE (the
    /// undo stack holds deleted nodes — audit §3.3). It stays in this scene's
    /// scene manager, so setGraphScene has to take it along; without that it
    /// would be left inside a manager the engine is free to destroy, and an
    /// undo after a world switch would walk stale handles.
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
    ///
    /// EDGE-DETECTING since AVATAR_LOCOMOTION Stage 3: the rising edge arms
    /// possession from `playMode` (auto-possess the first avatar in
    /// `third-person`) and the falling edge releases it and puts the editor
    /// camera back. Hanging that off the TRANSITION rather than off the caller
    /// is what makes `editor.stop(); editor.stop();` free — `editor.stop()`
    /// deliberately has no early-out (§8.3 rule 1, gate P6).
    void setPlaying(bool playing);
    bool isPlaying() const { return playing; }

    // ---- possession + the play mode (AVATAR_LOCOMOTION_SPEC §8.4/§8.5) ----

    /// The one possession slot and the spring-arm follow camera that rides with
    /// it. Never null: the scene owns one for its whole life, the way it owns
    /// its Environment. Nothing in it is serialized.
    AvatarPossession *getPossession() { return possession.data(); }
    const AvatarPossession *getPossession() const { return possession.data(); }

    ScenePlayMode getPlayMode() const { return playMode; }
    /// Changing the mode WHILE PLAYING re-arms possession immediately (the same
    /// transition the play edge runs), so a script can switch a running scene
    /// from explorer to third-person without a stop/start round trip.
    void setPlayMode(ScenePlayMode mode);

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

    /**
     * Sets the colour the PRIMARY member of a multi-selection is outlined in.
     * An invalid colour restores the derived default (a lightened outlineColor).
     * @param color
     */
    void setOutlinePrimaryColor(QColor color);

    void cleanup();

private:
    /// Where this document's tree currently lives. Never null once an
    /// Ogre::Root exists; see setGraphScene.
    graph::SceneHandle mGraphScene = nullptr;
    /// See _setGraphEvacuationHook. One-shot: setGraphScene clears it as it fires.
    std::function<void()> mGraphEvacuationHook;
    /// Subtrees detached from this scene and still alive — see
    /// rememberDetached. WEAK: this list must never be the reason a deleted
    /// node stays alive; expired entries are pruned as they are found.
    QList<SceneNodeWPtr> mDetached;
};

}


#endif // SCENE_H
