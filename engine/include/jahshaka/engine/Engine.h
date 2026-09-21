#pragma once
// Jahshaka's engine abstraction.
//
// This is THE boundary between the application and the 3D engine. Studio talks
// only to these types. No Ogre type, header or symbol appears here — swapping the
// backend must not touch a single file under src/.
//
// Interface derived from what Studio DOES, not from what any engine offers.
//
// THREAD AFFINITY — no exceptions: every call on Engine, Scene and View, including
// destruction, must happen on the thread that called Engine::create(). The backend
// owns a single device and is not internally synchronised. Background work
// (thumbnails, imports) posts to that thread; it never calls in directly.
//
// ERRORS: no backend exception ever escapes this boundary. A failing call returns
// null/false and the reason is available from Engine::lastError() until the next
// failing call overwrites it.
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Types.h"

namespace jahshaka { namespace engine {

class Scene;
class View;

/// A renderable scene. Views draw it; several Views may share one, or each may own one.
/// Owned by the Engine: destroy with Engine::destroyScene().
class Scene {
public:
    virtual ~Scene() = default;
    virtual const std::string &name() const = 0;
    /// Ambient light as a hemisphere pair: `upper` is what a surface facing +Y
    /// receives, `lower` what a surface facing -Y receives. Kept as the simple
    /// entry point (previews, thumbnails, tests); it is expressed EXACTLY in the
    /// spherical harmonics setAmbientSh() takes — see the note there about the
    /// scale of a flat (upper == lower) ambient.
    virtual void        setAmbient(const Colour &upper, const Colour &lower) = 0;
    /// Ambient light as 9 spherical-harmonic coefficients per channel, the form
    /// a real sky integrates to (a hemisphere pair can only ever say "up" and
    /// "down"). Layout: 9 groups of 3 floats (r, g, b), in the basis order
    ///     1, y, z, x, x*y, y*z, 3z^2 - 1, z*x, x^2 - y^2
    /// over WORLD axes, evaluated for the shading normal. The value the sum
    /// produces is a MEAN INCIDENT RADIANCE (irradiance / pi), the same unit the
    /// two hemisphere colours above are in, so a uniform white environment of
    /// radiance L is sh[0..2] = L and the rest zero.
    /// This is the only ambient path the backend has: setAmbient() converts.
    virtual void        setAmbientSh(const float sh[27]) = 0;
    /// THE ENVIRONMENT'S GAIN AS A SPECULAR LIGHT — the other half of
    /// setAmbientSh, and the half that used to be missing.
    ///
    /// setAmbientSh carries the environment's DIFFUSE contribution, already
    /// scaled by whatever light the host decided the environment is: 27 zeros
    /// mean "no environment light" and a matte surface goes black. Its
    /// SPECULAR contribution does not travel in those coefficients — it is
    /// sampled from the prefiltered environment cube — so a host that scaled
    /// the coefficients and stopped there left a mirror reflecting a sky that
    /// was lighting nothing (measured: a metal sphere reflected the sky
    /// byte-identically with every light in the scene hidden, Sky Light
    /// included).
    ///
    /// This is that scale, and the two are meant to be pushed together: the
    /// gain of the environment light in the same units, so 1.0 is "the cube's
    /// own radiance" and 0.0 is "there is no environment light", which makes
    /// the sky a BACKDROP — still drawn, still visible behind the scene,
    /// reflecting nothing into it.
    ///
    /// It is a SCALAR because the pin's is: the value rides
    /// `ambientUpperHemi.w` and HlmsPbs broadcasts it over the three channels
    /// (`envS.xyz *= midf3_c( passBuf.ambientUpperHemi.w )`). A host with a
    /// TINTED environment light should therefore pass its luminance and accept
    /// that the tint shows in the diffuse half only.
    ///
    /// NOT APPLIED WHILE PARALLAX-CORRECTED PROBES ARE BOUND. A probe is a
    /// photograph of the scene's real radiance — mostly of GEOMETRY lit by the
    /// scene's own lamps — and dimming it with the sky's gain would put a room
    /// out because its skylight was turned down. The gate is about the SKY, and
    /// under PCC the sky reaches a surface only through what a probe captured.
    /// 1.0 is the default, and the value is clamped at zero.
    virtual void        setEnvironmentLightScale(float gain) = 0;
    /// Exponential distance fog (+ optional height layer) on lit (PBR) surfaces —
    /// see FogDesc for the model. Unlit overlays (gizmos, wires, billboards) and
    /// the sky are never fogged. Off by default, and OFF IS EXACT: a disabled
    /// FogDesc leaves the scene rendering the very same pixels it did before fog
    /// was ever mentioned. Cheap to call every frame while enabled; the enabled
    /// EDGE costs a shader rebuild (fog is a shader variant, not a uniform).
    virtual void        setFog(const FogDesc &) = 0;
    /// THE SCENE'S SKY AND ITS ENVIRONMENT REFLECTIONS, as one description —
    /// SkyDesc carries the whole model (modes, face order, what
    /// `reflections == false` means, and why the flat colour and the CPU-baked
    /// skies are not in it).
    ///
    /// IDEMPOTENT: a description equal to the live one does nothing — no
    /// upload, no cube rebuild, no IBL reconvolution — and the two halves are
    /// compared separately, so changing only the reflection faces never tears
    /// the sky down. Hosts may therefore push every frame and let SkyDesc's
    /// `operator==` be the change guard, instead of keeping "what did I push
    /// last" bookkeeping of their own.
    ///
    /// False (see lastError()) leaves the scene's sky and reflections exactly
    /// as they were: an unknown texture id, six faces that are not all the same
    /// size, or a compressed face format.
    virtual bool        setSky(const SkyDesc &) = 0;
    /// THE SHADER-DRAWN EDITOR GRID (GridDesc). One quad per scene, drawn
    /// analytically from the camera ray, a helper (never in probes, shadows,
    /// reflectors, the Player or the Scene grade). IDEMPOTENT: an equal
    /// description does nothing; a disabled one hides the quad. False (see
    /// lastError()) when the grid material is not staged.
    virtual bool        setGrid(const GridDesc &) = 0;
    /// THE SKY'S OWN AMBIENT, as the same 9x3 spherical harmonics setAmbientSh
    /// takes, integrated from the sky the backend just drew (SKY-GPU).
    ///
    /// The backend captures whatever sky is bound into a small cubemap on the
    /// GPU and integrates THAT — so a photograph, a gradient, a picked colour
    /// and the analytic sky all answer the same question the same way, and no
    /// host needs an image decoder, a scattering model or a sphere-integral of
    /// its own. Returns false while no sky has been captured yet (no sky at
    /// all, or the capture has not run: it happens inside the next rendered
    /// frame, like the IBL convolution).
    ///
    /// WHEN THE ANSWER CHANGES, exactly (lane ENGINE-SMALL-A / audit ON-14,
    /// 2026-09-18) — because it is one frame for a lone edit and two for a
    /// gesture, and a host that renders a fixed number of frames and then
    /// asserts a picture has to know which:
    ///
    ///   * A LONE sky change is read SYNCHRONOUSLY, inside the frame that
    ///     captured it. This call answers with the new sky from the next frame
    ///     on, and a host pushing the ambient per frame has it in the picture
    ///     one frame after the capture — the behaviour this contract has always
    ///     described.
    ///   * A GESTURE — a second capture within a couple of drawn frames of the
    ///     previous one, i.e. a sun being dragged — DEFERS its readback: the
    ///     download is issued without a flush and read at the top of the NEXT
    ///     frame, so this call answers with the new sky from that frame on and
    ///     a host's per-frame push puts it in the picture the frame after, TWO
    ///     frames behind the capture. In exchange the capture frame does not
    ///     block on the GPU (measured 0.94 ms of flush and wait per change,
    ///     i.e. per frame of a drag). Nothing ever flickers: the previous
    ///     coefficients stay valid until the new ones land.
    ///
    /// JAHSHAKA_SKY_SH_SYNC forces the synchronous form for every capture — the
    /// run-wide diagnostic latch this engine's measurable rules carry, and the
    /// way the two arms are A/B'd on one binary.
    ///
    /// UNSCALED: this is the sky's mean incident radiance. A host that models a
    /// sky LIGHT multiplies by its intensity and tint and pushes the result
    /// through setAmbientSh — the backend never applies a light of its own.
    virtual bool        skyAmbientSh(float out[27]) const = 0;
    /// The description currently in force (default-constructed = no sky).
    virtual SkyDesc     sky() const = 0;
    /// THE ATMOSPHERE'S TINT ON A LIGHT COMING FROM `toSun` (SUN_FOLLOWS_
    /// ATMOSPHERE, lane ENGINE-7 item 6). White (1,1,1) unless the scene's sky
    /// IS the analytic atmosphere — every other sky is a picture, and a picture
    /// knows nothing about what the air does to sunlight.
    ///
    /// WHAT IT IS. The DIRECT BEAM's transmittance through the atmosphere at
    /// the sun's elevation, divided by its value with the sun at the zenith,
    /// per channel — Beer-Lambert, exp(-tau * airmass), with Rayleigh, aerosol
    /// and ozone optical depths and Kasten-Young airmass (OgreSky.cpp carries
    /// the constants and the reference). So it is 1,1,1 at noon — the user's
    /// picked colour IS the noon value — and falls, blue first, as the sun goes
    /// down: the reddening AND the dimming a low sun really does to direct
    /// light.
    ///
    /// ITS ONE INPUT IS `AtmosphereSky::sunHaze`, the atmosphere's turbidity,
    /// and NOT the sky's `density` (lane SKY-DENSITY-1). The sky's radiance and
    /// the sun's extinction are two physical quantities: the first is an
    /// integral of scattering over every view ray, drawn by a non-physical
    /// model whose density dial is artistic, the second is the absorption along
    /// one ray and is physics. They shared a dial until 2026-09-15, so tuning
    /// the sky's look moved the sunlight's colour and back. Moving `density`
    /// now leaves this value untouched, and moving `sunHaze` leaves every sky
    /// pixel untouched. (The component's own light link is still never armed —
    /// it would take the light's colour and power over entirely.)
    ///
    /// ...TIMES THE EARTH. The scattering model is frozen below the horizon
    /// (its own inputs clamp there), so on its own it would light the scene
    /// from a sun that has set — at 0.2 of noon with a thin sky. The answer is
    /// multiplied by a smooth occlusion that runs 1 to 0 between geometric
    /// elevations -0.305 and -0.835 degrees: the sun's own 0.53-degree disc
    /// setting through the horizon, lifted by 0.57 degrees of refraction. So
    /// this value REACHES ZERO, continuously, and a host does not need a
    /// threshold to decide when night starts — the light, the sun disc and the
    /// sun's shadow all ride this one number and fade together. (With the
    /// physical extinction the beam is already a thousandth of noon by an
    /// elevation of 0.7 degrees, so a host's night threshold trips just before
    /// the occlusion band rather than inside it — on a value that is three
    /// hundred times below one 8-bit step either way.)
    ///
    /// `toSun` points AT the sun (the opposite of the direction the light
    /// travels), in world space; it does not have to be normalised. Cheap to
    /// call per frame: the answer is memoised against the direction and the
    /// preset, and an unchanged sun costs a compare.
    virtual Colour      atmosphereSunTint(const Vec3 &toSun) const = 0;
    /// THIS SCENE'S SHADOW REQUEST — ShadowDesc says what the shape means and
    /// why it exists (the backend's filter and atlas are global; this hides
    /// that rather than pretending otherwise). Idempotent, and cheap when
    /// unchanged, so hosts push it per frame rather than guarding two global
    /// Engine setters by hand.
    virtual void        setShadowSettings(const ShadowDesc &) = 0;
    /// What this scene last requested (not what is globally in force — read
    /// Engine::shadowFilter()/shadowResolution() for that).
    virtual ShadowDesc  shadowSettings() const = 0;
    /// Removes a node and everything it uniquely owns (mesh, material). Unknown or
    /// already-removed ids are ignored and return false. Children are NOT removed;
    /// they are re-parented to the scene root.
    virtual bool        removeNode(NodeId) = 0;

    // ---- Hierarchy and transforms (VIEWPORT_MIGRATION_PLAN.md step 2) ----
    /// An empty transform node under `parent` (0 = the scene root).
    virtual NodeId      createNode(NodeId parent = 0) = 0;
    /// ADOPTS a scene node the HOST already owns and gives it a NodeId, so that
    /// everything else on this interface (attachMesh, setLight, decals, clips,
    /// particles) works on it exactly as if the engine had made it.
    ///
    /// SPECS/SCENEGRAPH_SPEC.md D2: the document's scene graph IS this backend's
    /// scene graph, and this is the one call that says so. An adopted node's
    /// transform, parent and children belong to the host — setNodeTransform and
    /// setNodeParent on it are refused — which is what makes the old per-frame
    /// transform push unnecessary.
    ///
    /// The pointer is opaque here on purpose (`Ogre::SceneNode*` in the Ogre
    /// backend); the host obtains it from iris::graph, which is the only other
    /// place in the program that names an engine node type. The node must belong
    /// to THIS scene's native scene manager — nativeSceneManager() is how the
    /// host arranges that. Returns 0 on a null pointer or a foreign manager.
    ///
    /// removeNode() releases the engine's attachments and forgets the id; it
    /// never destroys an adopted node.
    virtual NodeId      adoptNode(void *nativeSceneNode) = 0;
    /// This scene's native scene manager, opaque (`Ogre::SceneManager*`). The
    /// document migrates its tree into it when a SceneMirror binds; nothing else
    /// may use it.
    virtual void       *nativeSceneManager() const = 0;
    virtual bool        setNodeParent(NodeId, NodeId parent) = 0;
    /// Absolute LOCAL transform (relative to the parent). The document owns the
    /// numbers; the engine composes the hierarchy.
    virtual void        setNodeTransform(NodeId, const Vec3 &position, const Quat &rotation,
                                         const Vec3 &scale) = 0;
    /// The node's OWN visibility. A node is drawn iff it and every ancestor
    /// were told visible: hiding a node hides its subtree (geometry, lights,
    /// decals, particles, and GI — a hidden subtree neither bounces light nor
    /// shapes the automatic volume), and showing it again restores each
    /// descendant to what that descendant was told, never to visible
    /// wholesale. A host whose hierarchy differs from the engine's (a socket
    /// rider hangs off a bone) pushes its own effective state per node.
    virtual void        setNodeVisible(NodeId, bool) = 0;
    /// The same push from a host that walks its tree PARENT-FIRST and therefore
    /// already knows the parent's effective state (SceneMirror::visit). It is
    /// the only difference: `setNodeVisible` derives that state by walking up
    /// to the nearest registered ancestor — one registry lookup per push, which
    /// on a first sync is one per adopted node on top of the one `adoptNode`
    /// already paid, for an answer the caller computed a line earlier.
    /// `parentShown` is what the host would have found (true at the root of the
    /// pushed walk, and for a node whose engine parent is not in the document's
    /// tree at all — a socket rider on a bone — which is why a host pushes the
    /// EFFECTIVE state of those itself).
    virtual void        setNodeVisibleUnder(NodeId, bool visible, bool parentShown) = 0;

    // ---- Meshes and materials (step 3/4) ----
    /// Uploads geometry. Returns 0 on invalid data (lastError()).
    virtual MeshId      createMesh(const MeshData &) = 0;
    virtual bool        destroyMesh(MeshId) = 0;
    /// Rewrites the vertex positions (and, when non-empty, normals) of a mesh
    /// created with MeshData::dynamic — the CPU-skinning path: the host computes
    /// skinned vertices per frame and pushes them here. positions is xyz per
    /// vertex and must match the mesh's vertex count; normals likewise or empty
    /// to keep the current ones. Tangents and uvs keep their created values.
    /// Bounds are recomputed so culling stays correct. False (lastError()) for
    /// unknown or non-dynamic meshes or size mismatches.
    virtual bool        updateMeshVertices(MeshId, const std::vector<float> &positions,
                                           const std::vector<float> &normals) = 0;
    virtual MaterialId  createPbrMaterial(const PbrParams &) = 0;
    /// Re-applies every parameter EXCEPT the shading model — see
    /// setShadingModel for why that one is a separate, atomic call.
    virtual bool        setPbrMaterial(MaterialId, const PbrParams &) = 0;
    /// Moves a material between the LIT and UNLIT shading families
    /// (HLMS_ADOPTION P4a). See ShadingModel in Types.h for what Unlit costs.
    ///
    /// ATOMIC, and it has to be: the two families are different backend
    /// material types, so this destroys the backend material, builds a new one
    /// in the other family from the parameters and textures already pushed, and
    /// RE-ATTACHES every renderable that referenced it. The MaterialId is
    /// preserved on purpose — the host's own material identity is referenced by
    /// every node, and a switch that minted a new id would be a rewrite of the
    /// host's scene rather than a property change.
    ///
    /// REFUSES (lastError(), nothing changed) when the material is used by any
    /// node carrying a rig: the Unlit family cannot skin, and an unlit rigged
    /// mesh renders welded to its bind pose while the character animates away
    /// from it — silently. Idempotent: setting the model it already has
    /// succeeds and touches nothing.
    virtual bool        setShadingModel(MaterialId, ShadingModel) = 0;
    virtual bool        destroyMaterial(MaterialId) = 0;
    /// Binds a GENERATED shader piece to one material (HLMS_ADOPTION P5).
    /// `path` is an absolute path to a piece file; its directory is registered
    /// with the backend's resource system on first use. An EMPTY path clears
    /// the binding for that stage. Returns false (lastError()) for an unknown
    /// material, an unreadable file, or a material in the Unlit family.
    ///
    /// FILE FORM ONLY, and that is a decision rather than an omission: the
    /// backend's from-memory form cannot be disk-cached (it cannot prove the
    /// cached shader still matches the source), so a from-memory piece would
    /// recompile every material on every launch. Callers must therefore write
    /// the piece to disk first — and should name it by a hash OF ITS CONTENT,
    /// because the backend treats "same filename, different content" as a hard
    /// error, and content-addressed names make that impossible by construction.
    ///
    /// The material keeps the binding across a shading-model switch or a
    /// parameter push; only an empty path (or destroying the material) removes
    /// it. A material with no piece bound generates BYTE-IDENTICAL shader
    /// source to one from a build where this verb did not exist — the backend
    /// only sets the piece property when a piece id is non-zero.
    virtual bool        setMaterialCustomPiece(MaterialId, const std::string &path,
                                               CustomPieceStage) = 0;
    /// THE SHADER CLOCK this scene's generated pieces read (HLMS_ADOPTION P5).
    /// Seconds, and the HOST owns it: a paused editor, a scrubbed timeline and
    /// a deterministic test each render exactly the frame they ask for, because
    /// nothing in the engine advances this by itself.
    ///
    /// Scene-wide rather than per-material for two reasons, one of them
    /// structural: the vertex shader has NO material buffer in this backend's
    /// PBR template, so a per-material clock could never reach a vertex piece;
    /// and "time" is the same number for every material in a frame anyway. It
    /// costs one float in the pass constant buffer and one write per pass — no
    /// shader is ever rebuilt by it.
    ///
    /// Materials with no generated piece do not read it and are unaffected.
    virtual void        setShaderTime(float seconds) = 0;
    virtual float       shaderTime() const = 0;
    /// ATOM stage 1 (SPECS/NANITE_SPEC.md §7): the scene-wide LOD dial. 1 is the
    /// reference — every mesh switches level where its baked geometric error
    /// covers ONE PIXEL of the pass that is drawing it, at that pass's live lens
    /// and its render target's height (Types.h, `kLodBudgetPixels`), so the same
    /// object switches at the same SIZE ON SCREEN in the viewport, in a
    /// thumbnail and in a headset eye rather than at one distance for all three.
    /// The dial is a multiplier on that pixel budget: larger swaps earlier
    /// (coarser); 0 PINS every object at level 0, which is how a pixel test
    /// asserts one level at a time and how a user turns the whole thing off.
    /// Applies immediately to meshes that already exist: the thresholds are
    /// re-derived in place and every Item reads them through a pointer. Meshes
    /// with no baked chain are unaffected by any value.
    virtual void        setLodBias(float bias) = 0;
    virtual float       lodBias() const = 0;
    /// DIAGNOSTIC: what the backend datablock actually ends up holding, as
    /// text. Empty (lastError()) for an unknown material.
    ///
    /// "What did the datablock actually end up holding?" has been the hardest
    /// question in every material bug so far — the document says one thing, the
    /// mirror pushes another, the backend clamps or ignores a third, and
    /// nothing between them was inspectable without a debugger. This answers it
    /// from a script, in a running app. It pairs with the shader dump
    /// (JAHSHAKA_HLMS_DEBUG_DIR), which answers "and what shader did that
    /// produce".
    ///
    /// The format is the BACKEND'S, and it is a DIAGNOSTIC ONLY. It is not a
    /// material format and must never become one: the document is the truth,
    /// and it holds asset guids, a node graph, baked maps, our alpha-mode
    /// vocabulary and roughness remap bounds — none of which a datablock has a
    /// home for. Do not parse this.
    virtual std::string dumpMaterial(MaterialId) const = 0;
    /// Makes the node render `mesh` with `material`. A node renders at most one mesh;
    /// attaching again replaces it. Mesh and material may be shared across nodes and
    /// survive the node.
    virtual bool        attachMesh(NodeId, MeshId, MaterialId) = 0;
    /// A MATERIAL SWAP ON A LIVE ITEM (MATERIAL-SWAP-GI-1): the node keeps its
    /// mesh, its Item and its rig; only the material changes, in place. What a
    /// material decides for the Item (its render queue, its visibility family,
    /// its shadow shape) is re-derived, and the GI caches are invalidated for
    /// the ITEM'S BOX with nothing died — under a cascade chain only the
    /// cascades that box reaches re-voxelise, where attachMesh (detach + create)
    /// re-voxelises every cascade. False, with lastError(), when the swap
    /// crosses a family the Item cannot carry in place (Lit <-> Unlit /
    /// Distortion), when the node carries no mesh, or when the new material
    /// binds a normal map the mesh has no tangents for — attachMesh is the
    /// answer to every refusal. Idempotent for the material already worn.
    virtual bool        setNodeMaterial(NodeId, MaterialId) = 0;
    virtual bool        detachMesh(NodeId) = 0;
    /// How many renderables this node actually carries right now.
    ///
    /// The invariant is ONE (a node renders at most one mesh) and this is how a
    /// suite can say so: an attach path that created an Item and then lost its
    /// bookkeeping would leave the old one attached to the same scene node,
    /// drawing a second copy of the object with nothing in the host's model
    /// saying so. Counts the engine node's own attachments only — an outline
    /// shell lives on a node of its own and never shows up here.
    virtual size_t      itemCount(NodeId) const = 0;

    // ---- Rigs: GPU skinning (GPU_SKINNING_SPEC) ----
    /// Like attachMesh, but the mesh deforms on the GPU: the host pushes bone
    /// poses (setBonePoses) instead of vertices, and the vertex shader skins
    /// position, normal AND tangent.
    ///
    /// SEPARATE entry point on purpose — the backend must know the mesh is
    /// skinned BEFORE the renderable exists, so attaching first and skinning
    /// later would silently produce an unskinned object.
    ///
    /// The mesh must have been created with MeshData::hasSkinData(); its blend
    /// indices name bones of `rig`. Several nodes may attach the same mesh and
    /// the same rig and still pose independently — one rig instance per node.
    /// Refuses (lastError()) a mesh with no skin data, a rig whose bones do not
    /// cover the mesh's blend indices, an empty rig, a rig whose parent indices
    /// are out of range or cyclic, or a rig with more than 256 bones (which is
    /// attached UNSKINNED at bind pose, with a warning, rather than crashing).
    /// Bone ORDER is free — the index is what the vertex data names.
    ///
    /// `blendToRig` is the PER-PIECE REMAP (AVATAR_RIG_PERF_SPEC §3.1): entry
    /// `i` is the rig bone the mesh's blend index `i` names. Null (the default)
    /// means the identity — the mesh's blend indices ARE rig indices, which is
    /// what a single-piece character has and what every caller did before the
    /// union rig existed, byte for byte. A character whose pieces are SUBSETS
    /// of one rig passes a map per piece: the piece keeps its own compact blend
    /// indices, every piece binds the SAME rig (the precondition for sharing a
    /// SkeletonInstance at all), and the backend streams only the mapped bones
    /// per draw instead of the whole rig per piece.
    ///
    /// The map lives on the MESH, so two nodes sharing a mesh asset must pass
    /// the same map; a different one is refused (lastError()). Entries must
    /// name bones of `rig`, and the mesh's blend indices must fall inside the
    /// map.
    virtual bool        attachSkinnedMesh(NodeId, MeshId, MaterialId, const SkeletonDesc &,
                                          const unsigned short *blendToRig = nullptr,
                                          size_t blendToRigCount = 0) = 0;
    /// True when the node carries a GPU-skinned mesh with a live rig.
    virtual bool        hasSkeleton(NodeId) const = 0;
    /// The node's bone names, in rig index order. Empty when it has no rig.
    virtual std::vector<std::string> boneNames(NodeId) const = 0;
    /// The node's pose: `count` entries, index-parallel to the rig's bones, each
    /// LOCAL to its parent bone. This is the per-frame call — everything else on
    /// the rig surface is event-driven. False (lastError()) when the node has no
    /// rig or `count` does not match the rig's bone count.
    virtual bool        setBonePoses(NodeId, const BonePose *poses, size_t count) = 0;
    /// Reads back the bone matrices the vertex shader is actually handed for this
    /// node: `count` bones in rig order, each a ROW-MAJOR 3x4 (12 floats, so
    /// `out` holds count*12), WORLD-relative — the node's own transform is folded
    /// in, because a skinned vertex is never multiplied by a world matrix
    /// separately. Resolved as of the last rendered frame. False (lastError())
    /// when the node has no rig or `count` misses the rig's bone count.
    /// The read-back surface for the pose: what proves GPU and CPU skinning agree.
    virtual bool        boneMatrices(NodeId, float *out, size_t count) const = 0;
    /// How many bone matrices HlmsPbs streams for this node PER PASS: the
    /// length of the renderable's blend-index map, which is the whole rig while
    /// the map is the identity and only the piece's own bones once it is
    /// compacted (AVATAR_RIG_PERF_SPEC §1 row 4). 0 when the node has no rig.
    ///
    /// A read of the real Ogre state, not of our intent: the map IS what the
    /// shader is handed, so a remap that silently failed to land reads as the
    /// old number here.
    virtual size_t      streamedBoneCount(NodeId) const = 0;
    /// What this scene's rigs cost right now (RigStats). The measurement
    /// surface the rig-perf bench and its gates read; cheap enough to call per
    /// frame, but nothing on the frame path calls it.
    virtual RigStats    rigStats() const = 0;

    // ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC) ----
    /// Attaches clips to a node that already carries a rig. IDEMPOTENT per clip
    /// id: attaching a clip whose id is already on the node is a no-op.
    ///
    /// EVERY clip a node will use must be attached BEFORE any of them is
    /// enabled. This is not a style preference: the engine's own
    /// addAnimationsFromSkeleton push_backs into the vector its list of ACTIVE
    /// animations holds raw pointers into, and it does not fix that list up —
    /// so attaching while something plays dangles every active clip. The call
    /// therefore REFUSES (lastError()) while any clip on the node is enabled.
    ///
    /// Attaching the first clip also takes the node OUT of manual-bone mode:
    /// a manual bone is not reset to the bind pose before a clip accumulates,
    /// so an enabled clip would ADD to whatever setBonePoses last wrote. Bones
    /// explicitly marked by setBoneManual keep their override.
    ///
    /// Refuses: a node with no rig; a track naming a bone the rig does not
    /// have; unsorted, duplicated or empty key times; a clip with no tracks.
    /// A clip whose length is <= 0 is PADDED to a minimum length and reported
    /// in the log, never refused.
    virtual bool attachClips(NodeId, const ClipDesc *clips, size_t count) = 0;
    /// The node's clip names, in attach order — including any uniquifying
    /// suffix the backend added for a collision. Empty when it has no rig.
    virtual std::vector<std::string> clipNames(NodeId) const = 0;

    /// THE per-frame clip call. Absolute times only. Clips the array does not
    /// name are disabled. Weights are raw intent; the backend normalizes them
    /// PER BONE and honours manual-bone overrides (a manual bone gets zero
    /// weight from every clip, so the override really overrides).
    ///
    /// NOTE, and it must be designed for rather than discovered: with NO clip
    /// enabled the engine does not reset to the bind pose at all — the pose
    /// FREEZES wherever it was. "Stop" means one clip enabled at t = 0, or a
    /// setBonePoses write, never an empty state array.
    virtual bool setClipStates(NodeId, const ClipState *states, size_t count) = 0;

    /// Per-bone override channel. A manual bone keeps whatever setBonePoses
    /// wrote and is excluded from every clip's weighting.
    virtual bool setBoneManual(NodeId, const std::string &bone, bool manual) = 0;

    /// Reads back the EVALUATED pose: `count` bones in rig order, each LOCAL to
    /// its parent bone (a root bone: local to the mesh node) — the same frame
    /// setBonePoses writes in. Resolved as of the last rendered frame.
    virtual bool bonePoses(NodeId, BonePose *out, size_t count) const = 0;

    /// The effective per-bone weight the backend applied for `clip`, in rig
    /// bone order (0 for a bone the clip does not animate). The test and
    /// diagnostic surface for the normalization rule; empty on any error.
    virtual std::vector<float> clipBoneWeights(NodeId, const std::string &clip) const = 0;

    // ---- Textures (step 4b): image files on disk, shared across materials ----
    /// Loads an image file (png/jpg/tga/dds...). `srgb` for colour maps (albedo,
    /// emissive); false for data maps (normal, roughness, metalness). The same path
    /// loaded twice returns the same id. 0 on failure (lastError()).
    virtual TextureId   loadTexture(const std::string &path, bool srgb) = 0;
    /// A texture from RGBA8 pixels in memory (top-left origin, width*height*4 bytes).
    ///
    /// `mipmaps` builds the whole chain (box-filtered on the CPU, at upload
    /// time) instead of the single level these textures used to get. Ask for it
    /// whenever the image will be seen MINIFIED: a light icon's source glyph is
    /// 640x640 and lands on ~30 screen pixels, and with one level the sampler
    /// point-samples 1 texel in 400 — which is the sparkling, broken-edged
    /// "blurred" glyph of the 2026-09-08 owner report. Costs a third more
    /// memory and a few hundred microseconds, once, per icon.
    virtual TextureId   createTexture(unsigned width, unsigned height, const unsigned char *rgba,
                                      bool srgb, bool mipmaps = false) = 0;
    /// A CUBEMAP from six square, same-size, same-format face textures, in
    /// WORLD-AXIS order (+X, -X, +Y, -Y, +Z, -Z) with image row 0 at the top —
    /// exactly what SkyDesc::reflectionFaces takes, and built by the SAME code, so the
    /// left-handed remap the backend's cubemap lookups need is applied once and
    /// in one place (the 2026-09-03 fact: getting this wrong silently mirrors
    /// every reflection). Returns a TextureId the caller owns and destroys.
    ///
    /// Its use today is a per-material reflection override (ADDENDUM A-5):
    /// `setPbrTexture(mat, PbrTextureSlot::Reflection, id)`.
    virtual TextureId   createCubemap(const TextureId faces[6]) = 0;
    /// REPLACE the pixels of an existing texture, same size, same format
    /// (ADDENDUM A-1). The upload path createTexture already is, re-run — the
    /// staging texture comes from and returns to a pool, so a per-frame call is
    /// not an allocation.
    ///
    /// createTexture-BORN IDS ONLY. A file-loaded texture takes its format, mip
    /// count and colour space from the file and is POOLED, so writing into one
    /// would change every material that loaded that path; a decal-atlas slice is
    /// shared process-wide and refcounted, which is worse. Both are refused by
    /// name. A different size is refused too — a Vulkan texture cannot resize;
    /// destroy and create.
    ///
    /// ORDERING: the upload records into the OPEN command buffer, ahead of this
    /// frame's draws, so the new pixels RENDER with no flush. `flushCommands()`
    /// is required only before READING this texture back in the same frame.
    virtual bool        updateTexture(TextureId, unsigned width, unsigned height,
                                      const unsigned char *rgba) = 0;
    virtual bool        destroyTexture(TextureId) = 0;
    /// How many mip levels the texture actually has (1 = base only, 0 = no such
    /// texture). The diagnostic half of `mipmaps` above: a chain that silently
    /// stopped being built is invisible in a still frame and obvious here.
    virtual unsigned    textureMipmaps(TextureId) const = 0;
    /// Binds (or, with 0, clears) a texture slot on a PBR material.
    virtual bool        setPbrTexture(MaterialId, PbrTextureSlot, TextureId) = 0;

    // ---- Overlay primitives (step 8): gizmos, light wires, animation paths ----
    /// Flat colour, unlit. With depthTest=false it draws on top of everything —
    /// what gizmo handles need. Alpha < 1 blends.
    /// `wireframe` draws only the triangle edges — the selection outline uses it.
    virtual MaterialId  createUnlitMaterial(const Colour &, bool depthTest, bool wireframe = false) = 0;
    /// The colour, LIVE — and the alpha with it (GIZMO-2 item 4). Passing an
    /// alpha below 1 to a material created opaque turns it into a blended one
    /// (and back): whether a surface blends is rasterizer state rather than a
    /// shader constant, so the engine swaps that state here instead of
    /// silently ignoring the alpha. A blended overlay is drawn after the opaque
    /// ones of its own render queue, back to front, by Ogre's own sort — the
    /// host passes a Colour and nothing else changes. Depth behaviour is NOT
    /// touched: an on-top overlay already has depth write off, and a
    /// depth-tested one keeps what it was created with.
    virtual bool        setUnlitMaterial(MaterialId, const Colour &) = 0;
    /// Selection silhouette: unlit colour drawn on BACK faces only, so a copy of the
    /// mesh scaled up slightly (~4%) renders as a clean outline band around the
    /// original (inverted hull). Depth-tested, so occluders still hide it.
    ///
    /// `skinnable` picks the variant a SKINNED character needs. HlmsUnlit has no
    /// skeletal path at all in this engine (`hlms_skeleton` lives only in the Pbs
    /// templates), so an unlit hull over a rigged mesh draws the BIND POSE
    /// forever: the moment the character animates, its "outline" peels off and
    /// stands there as a solid selection-coloured twin (found 2026-09-06 on a
    /// Mixamo character — it read as "the character renders twice"). The
    /// skinnable variant is the same silhouette built on HlmsPbs with black
    /// diffuse/specular and the colour as EMISSIVE, which is unlit in effect and
    /// skins; pair it with shareSkeleton so the shell rides the character's own
    /// pose instead of a second, un-posed skeleton.
    virtual MaterialId  createOutlineMaterial(const Colour &, bool skinnable = false) = 0;
    /// Makes `follower`'s renderable copy `source`'s POSE every frame — the
    /// selection silhouette over an animating character, and anything else that
    /// needs a second renderable of the same rig to move with the first.
    ///
    /// A COPY, not Ogre's Item::useSkeletonInstanceFrom: a shared instance
    /// carries the source's WORLD transforms in its bone matrices, so the
    /// follower renders exactly where the source is and its own scene node stops
    /// meaning anything — which is fatal for a silhouette, whose whole existence
    /// is a few percent of scale on that node. Copying the LOCAL bone transforms
    /// keeps each renderable's own node in charge of where it lands.
    ///
    /// The copy happens after each rendered frame, so the follower rides ONE
    /// FRAME behind. That is deliberate: the alternative is forcing a second
    /// skeleton update per frame before rendering, and a selection band 16 ms
    /// behind the character is not visible while a second full animation pass is
    /// measurable. Both rigs must have the same bone count.
    ///
    /// The pairing is REMEMBERED across re-attaches on either end, and drops
    /// itself when either node goes away. Passing source = 0 stops following.
    virtual bool        followSkeleton(NodeId follower, NodeId source) = 0;
    /// SHARING, which is the other thing entirely (AVATAR_RIG_PERF_SPEC §3.2):
    /// `follower` renders from `source`'s SkeletonInstance — Ogre's
    /// Item::useSkeletonInstanceFrom — so the pose is evaluated ONCE for both,
    /// clips are pushed once, and a character made of five skinned pieces costs
    /// one animation update instead of five.
    ///
    /// THE PRICE, and it is not negotiable: the shared bones carry the SOURCE's
    /// node transform, so the follower renders WHERE THE SOURCE IS. Its own
    /// scene node still decides its culling AABB and nothing else. Share only
    /// pieces whose world transform equals the source's — the caller keeps them
    /// equal, or does not share.
    ///
    /// Both ends must be rigged to the SAME rig (Ogre throws on a skeleton-name
    /// mismatch; this refuses before it does). Also refused: a node sharing with
    /// itself, a source that is itself a follower, a follower that has followers
    /// of its own, and an unrigged end.
    ///
    /// `source = 0` stops sharing: the follower gets its own instance back,
    /// carrying the pose it was rendering, re-attached to its own node.
    ///
    /// The share is LIVE STATE, not an intent: any re-attach on either end
    /// (a material swap, a mesh swap) drops it — safely, in the one order that
    /// does not hand the master's rig a null parent node — and the host re-arms
    /// it on the next sync.
    virtual bool        shareSkeleton(NodeId follower, NodeId source) = 0;
    /// True when this node's Item is rendering from another node's instance.
    virtual bool        sharesSkeleton(NodeId) const = 0;

    // ---- Bone attachments: engine TAG POINTS (AVATAR_RIG_PERF_SPEC §4) ----
    /// Hangs `rider`'s node off `owner`'s bone, through an engine TagPoint whose
    /// local transform is `offset` (position/rotation/scale, in BONE space).
    ///
    /// ZERO LAG, which is the whole point: Ogre resolves tag points INSIDE the
    /// threaded scene update, after the skeletons it reads
    /// (updateAllTransforms -> updateAllAnimations -> updateAllTagPoints), so a
    /// camera or a sword on a bone is in the right place in the frame that
    /// renders it. The host's alternative — read the pose back, run FK, write
    /// world transforms — is one frame late by construction and costs a
    /// read-back per rigged node per frame.
    ///
    /// The rider keeps its OWN local transform, now RELATIVE TO THE SOCKET: the
    /// tag is the socket, and the node under it is where the user nudged the
    /// sword.
    ///
    /// Refuses: an owner with no rig, a bone the rig has not, a rider that is
    /// the owner, and a rider that is an ANCESTOR of the owner (a circular
    /// dependency, which Ogre calls "undefined, probably very wonky").
    ///
    /// NOTE for readers between frames: a tag point's world transform is the one
    /// the LAST RENDERED FRAME produced. Ogre cannot resolve a tag on demand
    /// (TagPoint::updateFromParentImpl is `assert(false)`), so
    /// iris::graph::globalTransform reads the cached transform for a tagged
    /// node — identical latency to the read-back path this replaces, while the
    /// RENDER is exact.
    virtual bool attachToBone(NodeId rider, NodeId owner, const std::string &bone,
                              const Vec3 &position, const Quat &rotation, const Vec3 &scale) = 0;
    /// Takes the rider off its bone and puts it back under `parent` (0 = the
    /// scene root), keeping the world transform it had at the last rendered
    /// frame — the fail-soft path when a socket, a bone or an owner goes away.
    virtual bool detachFromBone(NodeId rider, NodeId parent) = 0;
    /// Re-writes the tag's local transform — the socket was edited.
    virtual bool setBoneAttachmentOffset(NodeId rider, const Vec3 &position,
                                         const Quat &rotation, const Vec3 &scale) = 0;
    /// The owner this rider is attached to, or 0 — and the bone, through `bone`.
    virtual NodeId boneAttachment(NodeId rider, std::string *bone = nullptr) const = 0;
    /// A line list (pairs of points) or, with `strip`, a connected polyline.
    /// Attach with attachMesh like any mesh. One pixel wide.
    virtual MeshId      createLineMesh(const std::vector<Vec3> &points, bool strip) = 0;

    // ---- Particles: externally-simulated particles drawn as camera-facing quads.
    // The set rides on a node for ownership (removeNode frees it) but instance
    // positions are WORLD-space — the document simulates in world space.
    /// Creates (or replaces) the node's billboard set: up to `capacity` quads,
    /// textured by `texture` (0 = untextured white), additive (src-alpha, one) or
    /// alpha-blended.
    ///
    /// `layer` decides WHERE in the frame it lands (BillboardLayer):
    ///   * Scene   — depth test on, depth write off, drawn with the opaques and
    ///               graded by every post effect. What an emitter wants.
    ///   * Overlay — no depth test, drawn in the on-top overlay pass AFTER the
    ///               post chain. What an editor helper (the light icons) wants.
    virtual bool createBillboardSet(NodeId, TextureId texture, bool additiveBlend,
                                    unsigned capacity,
                                    BillboardLayer layer = BillboardLayer::Scene) = 0;
    /// Replaces the set's instances each frame; count above capacity is clamped.
    virtual bool setBillboards(NodeId, const BillboardInstance *, size_t count) = 0;
    /// Removes the node's billboard set (removeNode does this too). KEPT as the
    /// explicit counterpart of createBillboardSet: the document turns a particle
    /// system off without destroying its node, and tests/particles pins that.
    virtual bool destroyBillboardSet(NodeId) = 0;

    // ---- Particles (PARTICLES_FX2_SPEC.md): the engine SIMULATES these ----
    // Unlike billboard sets, the host pushes PARAMETERS, not particles: emission,
    // forces, colour-over-life and spin run inside the engine (SIMD, on worker
    // threads) and advance on every renderOneFrame. The document owns the
    // authoring values and its own clock scalar; it never integrates anything.
    //
    // One node = one particle-system DEFINITION. The definition carries the quota,
    // the material and the visibility flag, so two emitters in a scene are fully
    // independent. Definitions cannot be individually destroyed by the backend
    // (there is no such API), so the engine keeps each node's topology FROZEN —
    // one emitter of the chosen shape plus a fixed, defaults-neutral affector set —
    // and recycles abandoned definitions through a per-scene pool.

    /// Creates or updates the node's particle system. Scalar changes (rate,
    /// velocity, colour keys, forces...) are applied in place. A TOPOLOGY change —
    /// emitter shape or count, affector kinds, quota bucket, orientation, blend
    /// mode or texture — rebuilds the definition and recycles the old one.
    /// The system rides on the node: it moves with it, hides with it, and
    /// removeNode frees it. False with lastError() set; never throws.
    virtual bool setParticleSystem(NodeId, const ParticleSystemDesc &) = 0;
    /// KEPT as the explicit counterpart of setParticleSystem: a node may stop
    /// being an emitter without being removed. Live particles vanish with it.
    virtual bool removeParticleSystem(NodeId) = 0;
    /// How many particles are currently alive in the node's system, for tests and
    /// the properties panel. 0 when the node has no system. SIMD-rounded up.
    virtual unsigned particleCount(NodeId) const = 0;
    /// DIAGNOSTIC: how many particle definitions this Scene has ever created.
    /// Definitions cannot be destroyed before the Scene is, so this number never
    /// falls — it is the leak the recycling pool exists to bound, and the
    /// particle suites assert on it. Never call it from UI code.
    virtual unsigned particleDefinitionsCreated() const = 0;

    // ---- Lights (step 5): a node may carry one light. Directional and spot lights
    // shine down the node's -Y (the document's convention: identity = straight down).
    virtual bool        setLight(NodeId, const LightDesc &) = 0;   // creates or updates
    /// KEPT as the explicit counterpart of setLight: a node may stop being a light
    /// without being removed (the document changes a node's type in place).
    virtual bool        removeLight(NodeId) = 0;

    // ---- Decals (DECALS_SPEC.md): a node may carry one projected-texture decal.
    // A decal is an oriented box that overwrites base colour / roughness /
    // metalness on the surfaces inside it, projecting down the node's -Y (the
    // same convention as lights). It draws nothing itself: the PBR shader
    // consumes it through the Forward+ clustered list.
    /// Creates or updates the node's decal. False (lastError()) when the desc
    /// carries no diffuse texture, or one that did not come from
    /// loadDecalTexture().
    virtual bool        setDecal(NodeId, const DecalDesc &) = 0;
    /// KEPT as the explicit counterpart of setDecal: a node may stop being a
    /// decal without being removed (the document changes a node's type, or the
    /// user clears the image).
    virtual bool        removeDecal(NodeId) = 0;
    /// Loads an image into the DEDICATED, fixed-geometry decal atlas for `kind`
    /// and returns a texture id usable in DecalDesc. Images are resampled into
    /// the atlas geometry (aspect preserved, padded with transparent pixels —
    /// alpha is the decal mask, so padding is invisible).
    ///
    /// NOT interchangeable with loadTexture(): decals sample one Type2DArray
    /// per channel and carry a slice index into it, so every decal image must
    /// share one resolution/format/mip-count pool. loadTexture() puts images in
    /// pool 0 alongside ordinary PBR maps (wrong slices) and its grayscale
    /// branch produces a non-batched texture the backend refuses outright.
    ///
    /// Returns 0 with a clear lastError() when the atlas is FULL — never a
    /// silent fallback: an overflowing decal would sample another decal's image
    /// with no warning at all.
    virtual TextureId   loadDecalTexture(const std::string &path, DecalMap kind) = 0;
    /// How many slices the `kind` atlas has, and how many are already taken.
    /// The UI surfaces "decal image budget full" from this rather than guessing.
    virtual unsigned    decalAtlasCapacity(DecalMap kind) const = 0;
    virtual unsigned    decalAtlasUsed(DecalMap kind) const = 0;

    // ---- Global illumination (GI_SPEC.md). Scene-level, like fog and sky. ----
    /// Applies the GI state and BUILDS IT FROM SCRATCH — voxels, the probe grid
    /// (placement captures every probe) and the irradiance field — every call,
    /// equal params included: that is the explicit "rebuild now", and suites
    /// use it as such. Hosts therefore push on CHANGE only (SceneMirror
    /// compares GiParams by value), re-solve through refreshGlobalIllumination,
    /// and take the process-wide binding back after another scene built GI with
    /// reassertGiBinding — never by re-pushing. GiMode::Off tears everything
    /// down. Modes the
    /// backend has not implemented yet degrade to Off (true is still returned so a
    /// document saved with a future mode keeps loading).
    virtual bool        setGlobalIllumination(const GiParams &) = 0;
    /// THE TUNING PUSH — the GI values that take effect WITHOUT a rebuild
    /// (PHOTON_SPEC §7 E2 (8), audit A F6). `ddgiIntensity`, `ddgiAmbient` and
    /// `rayMarchStepScale` are read per frame (the first two by the irradiance
    /// field's shader constants, the third by the next light injection), so
    /// moving one is a constant write and not a re-solve — and they are
    /// deliberately OUT of `GiParams::operator==` so that a host comparing by
    /// value does not see a slider tick as a configuration change. Before this,
    /// every tick of those three sliders tore the whole arm down and
    /// re-voxelised the scene (under a cascade chain, N times).
    ///
    /// Every OTHER field in GiParams still belongs to setGlobalIllumination,
    /// including the probe-placement tolerances the audit grouped with these
    /// (`probeOverlap`, `probeSnap*`): those are consumed while the probe grid
    /// is PLACED, so pushing one without a rebuild would change nothing at all
    /// and a silently ignored slider is worse than an expensive one.
    ///
    /// No-op with GI off or nothing built. Returns false only on an engine
    /// error.
    virtual bool        setGiTuning(const GiParams &) = 0;
    /// Re-runs the active GI solution against the scene's current state (a light
    /// moved, geometry changed). No-op when GI is off. The engine picks the
    /// cheapest correct arm for what actually changed — under a cascade chain an
    /// edit costs the cascades that can see it, one per frame — so callers may
    /// invoke this per edit.
    virtual void        refreshGlobalIllumination() = 0;

    // ---- a world that is not on screen yet (OPEN_COVER_SPEC §2 A) ---------
    /// THE LOAD IS A STRETCH, NOT A FRAME. While this is true the scene starts
    /// no FIRST-TIME global-illumination build at all: the request stays armed
    /// in the usual way and the first frame the user can actually see takes it.
    /// A build already standing is untouched, and every cheap path (the
    /// per-cascade dirty queue, a re-injection) runs exactly as it does
    /// normally — what is refused is the from-scratch arm, which is the single
    /// longest thing this engine does on the UI thread.
    ///
    /// WHY IT IS STICKY AND NOT A FRAME FLAG: a load draws frames from half a
    /// dozen places — the open runner's slice boundaries, the loading cover's
    /// inline presents, the shader warm-up's 4x4 target, the project tile's
    /// offscreen shot — and ONE of them left at `FramePace::Complete` builds
    /// the whole arm in the middle of the load (measured: it was the frame
    /// after the warm-up set was recorded).
    ///
    /// The host raises it when a world starts arriving and lowers it when that
    /// world is on screen. Default false, so an engine nobody tells is an
    /// engine that behaves exactly as it always did.
    virtual void        setLoading(bool loading) = 0;
    virtual bool        isLoading() const = 0;
    /// The LIGHT-ONLY refresh (REFLECTIONS_ADOPTION_SPEC.md P2): re-injects the
    /// scene's lights into the EXISTING voxel volume and leaves the geometry
    /// alone. Orders of magnitude cheaper than the full call — no
    /// re-voxelization, no probe re-render — and it is what a light being
    /// DRAGGED needs: the bounce follows the light live while the expensive
    /// rebuild waits for the drag to stop.
    ///
    /// It is a partial answer on purpose and the caller must know which parts it
    /// does not update: reflection PROBES capture lighting, so probe reflections
    /// stay as they were until the next full refresh. Under Instant Radiosity
    /// there is no cheaper path than the re-trace, so there this IS the
    /// re-trace. Returns false when nothing could be done (GI off, or nothing
    /// built yet), so a caller can tell "cheap refresh done" from "no-op".
    ///
    /// `inMotion` says whether the thing that moved is STILL MOVING, and it
    /// buys the drag economy: in motion the injection runs ONE bounce and the
    /// coarser ray march, because the picture it produces is replaced by the
    /// next tick a few frames later; at rest it runs the scene's full bounce
    /// count and the fine march, so the frame the user is left looking at is
    /// the one the full solve would have produced. A host that drives this on a
    /// cadence must therefore fire ONE `inMotion = false` call after the motion
    /// stops — the settle's own re-solve covers the drag path, but a MOVABLE
    /// lamp (REALTIME_REFLECTIONS_SPEC §3.3, O2) never arms a settle at all,
    /// and without that last call its room stays lit at one bounce for good.
    virtual bool        refreshGiLighting(bool inMotion) = 0;
    /// What GI actually ACHIEVED, as opposed to what was requested — probe
    /// count and whether the probe/VCT bindings are live on this scene. The
    /// hybrid can degrade to plain VCT (a missing probe workspace definition);
    /// without this nothing, not even a pixel test, could tell the difference.
    /// Cheap: reads live pointers, renders nothing.
    virtual GiStatus    giStatus() const = 0;
    /// WHAT THE RAY-QUERY TIER HOLDS FOR THIS SCENE (PHOTON_SPEC §7 R1) — the
    /// acceleration structures, their size, and what the last frame's update
    /// cost. The same "what it ACHIEVED, not what was asked for" contract as
    /// giStatus() above, and it is reported beside it (`world.giStatus()
    /// .rayQuery`). Cheap: reads counters, renders nothing.
    ///
    /// NOT part of GiStatus itself on purpose: the tier is not global
    /// illumination — it is a geometry service GI happens to be the first
    /// consumer of.
    virtual RayQueryStatus rayQueryStatus() const { return RayQueryStatus(); }
    /// WHAT THE VOXEL LIGHTING VOLUME HOLDS (PHOTON-M3) — a TEST AND TOOL
    /// readback of one cascade's light volume: its peak, its mean over lit
    /// voxels and how many voxels sit on the storage format's top bin.
    ///
    /// It exists because the bounce's fixed point L = D + rho * G(L) is a
    /// PHYSICAL quantity kept in a FIXED-RANGE store, and whether it fits
    /// cannot be read from the picture — a clipped voxel draws a picture that
    /// is merely dimmer, which is indistinguishable from a scene with less
    /// bounce in it (the mechanism PHOTON-M2's F1 read as a "contraction").
    /// So the guard that the store has headroom is a readback, and this is it.
    ///
    /// NOT a per-frame path, ever: it flushes the render system's commands and
    /// BLOCKS on a download of the whole volume (8 MB at 128^3), exactly like
    /// `traceRays` above. Answers `available = false` — and nothing else —
    /// without a VCT arm, without that cascade, or on a device that refuses
    /// the download.
    virtual GiVoxelStats giVoxelStats(int cascade) { (void)cascade; return GiVoxelStats(); }
    /// TRACE A BATCH OF RAYS against this scene's acceleration structure and
    /// wait for the answer — a TEST AND TOOL path, never a per-frame one.
    ///
    /// It exists because the honest proof that a ray tier works is a ray whose
    /// hit distance can be compared with the analytic answer, and because R2's
    /// probe-visibility array is this call with a different set of directions.
    /// The product consumers (R3's sun contact, R5's reflections) will trace
    /// from a compute pass INSIDE the frame instead and never come through
    /// here; this one submits its own command buffer and BLOCKS on a fence.
    ///
    /// `rays` is 12 floats per ray — origin.xyz, tMin, direction.xyz, tMax,
    /// instance mask (bit 0 casters, bit 1 movers, bit 2 still world; 0xFF =
    /// everything), and three unused. `hits` comes back as 4 floats per ray:
    /// distance to the first hit (< 0 = miss), the hit node's index in the
    /// scene's item order, the hit triangle's index, and 1 or 0.
    ///
    /// False when the tier is off, unavailable, or the scene has no structure
    /// yet (render one frame first).
    virtual bool traceRays(const std::vector<float> &rays, std::vector<float> &hits) {
        (void)rays;
        hits.clear();
        return false;
    }
    /// "THIS SCENE IS ON SCREEN AGAIN" — re-points the process-wide HlmsPbs GI
    /// binding (voxel lighting, reflection-probe grid, irradiance field) at
    /// this scene's own arms, WITHOUT rebuilding anything
    /// (ENGINE_CACHE_POLICY_SPEC §2 P10).
    ///
    /// The binding is "last scene to build wins" (OgreGi.cpp), so when the
    /// player page builds its own GI and the editor comes back, the editor's
    /// scene would render with the player's voxels and probes. The host used to
    /// answer that by re-pushing setGlobalIllumination, which rebuilds the whole
    /// arm from scratch — 2-3 s of blocked UI on every page return. This is the
    /// whole of what a page return needs: the arms this scene already built are
    /// still valid, only the pointer the shader reads is not.
    ///
    /// A scene with no arm of a kind unbinds that kind (another scene's probes
    /// must not light this one). A no-op, returning false, when this scene
    /// already owns the binding; true when it re-pointed anything.
    virtual bool        reassertGiBinding() = 0;

    /// "Has any object LEFT the volume that is currently lit?" — 0 when every
    /// GI item is inside it, otherwise a hash of the escapees' quantized world
    /// AABBs (LIGHTING_FIX fix 2).
    ///
    /// A HASH, NOT A BOOL, and that is the whole design. The host debounces
    /// expensive re-solves by watching a signature: it restarts a stability
    /// window whenever the value changes and re-solves once it has held still.
    /// A bool would stay true for the entire duration of a drag, so the window
    /// would either fire on every frame of it or never fire at all. A hash of
    /// WHERE the escapee is changes on each frame the object moves and freezes
    /// the moment the user lets go — one re-solve per gesture, which is exactly
    /// the contract a light's transform signature already has. Zero when GI is
    /// off, when nothing has been built yet, and whenever the document typed
    /// its own bounds box (then the volume is the user's statement, not a fit).
    ///
    /// Cheap: one world-AABB read per GI item, no allocation, renders nothing.
    virtual unsigned long long giEscapeSignature() const = 0;
    /// "Has any GI geometry MOVED?" — a quantized hash of every GI item's world
    /// AABB (FIX WAVE B3). Same contract and the same debounce as
    /// giEscapeSignature and the host's light-transform signature: it changes on
    /// every frame of a drag and freezes when the drag stops, so a host that
    /// watches it spends the CHEAP paths during the gesture and exactly one full
    /// re-solve at the end of it. Stateless — reading it twice in a frame is
    /// free of side effects. Zero only when GI is off.
    ///
    /// Cheap: one world-AABB read per GI item, no allocation, renders nothing.
    virtual unsigned long long giGeometrySignature() const = 0;
    /// "Has a material GI converted changed?" (ENGINE_CACHE_POLICY_SPEC P7) — a
    /// generation that moves when a parameter the voxelizer (or Instant
    /// Radiosity's trace) reads — albedo, emissive, alpha, workflow, the albedo
    /// or emissive map — changes on a material GI geometry wears. Its own term,
    /// not part of giGeometrySignature: a host debounces it into ONE re-solve
    /// when the edit settles, and must NOT run the light re-inject cadence for
    /// it (a material edit changes nothing a re-inject reads). 0 when GI is off;
    /// an idle scene never moves it. Cheap: returns a counter.
    virtual unsigned long long giMaterialSignature() const = 0;
    /// "This object must not define WHERE global illumination happens"
    /// (REFLECTIONS_ADOPTION_SPEC.md P1a). The object still voxelizes and still
    /// bounces light — it is only kept out of the two AABB reductions, the lit
    /// volume and the reflection-probe region. The case it exists for is the
    /// ground plane: 200 units of it under a 2-unit scene drags the voxel
    /// volume and the probe grid over empty air. Takes effect on the next GI
    /// (re)build, exactly like moving the geometry would.
    virtual void        setNodeGiBoundsExcluded(NodeId, bool) = 0;
    virtual bool        nodeGiBoundsExcluded(NodeId) const = 0;
    /// "This node is an EDITOR HELPER" (REFLECTIONS_ADOPTION_SPEC.md P1b): the
    /// ground grid, light icons, range wires, camera helpers — geometry the
    /// user must see in the viewport and a reflection probe must never capture.
    /// It renders in the main view exactly as before; it is excluded from
    /// reflection-probe captures, and from any future pass that opts into the
    /// same channel. Applies to the node's mesh, billboards and particles, and
    /// takes effect immediately whether it is set before or after they attach.
    /// Not inherited: set it on each node that carries helper geometry.
    virtual void        setNodeHelper(NodeId, bool) = 0;
    virtual bool        nodeHelper(NodeId) const = 0;

    /// "This node is a BACKDROP" — a helper that is part of the PICTURE.
    ///
    /// Same exclusions as setNodeHelper (no reflection-probe capture, no shadow
    /// map, no GI geometry), and it IMPLIES setNodeHelper — but a view that
    /// hides the editor's furniture (View::setHelpersVisible(false), what the
    /// Player page is) still draws it. The case it exists for is the ground's
    /// 2 km horizon plane: real picture, and nothing that size may ever size a
    /// shadow atlas or a voxel volume.
    ///
    /// setNodeHelper(id, false) clears it too — one flag pair, one meaning.
    /// Not inherited; may be set before the geometry arrives.
    virtual void        setNodeBackdrop(NodeId, bool) = 0;
    virtual bool        nodeBackdrop(NodeId) const = 0;

    /// "THIS HELPER IS DRAWN IN THE HEADSET TOO" (SPECS/VR_SPEC.md §5 phase 4).
    ///
    /// Editor furniture comes in two kinds once the editor's scene is being
    /// worn, and the split is about WHOSE furniture it is rather than what kind
    /// of object it is. Most of it belongs to the DESK — the ground grid, the
    /// light and camera icons, the mouse gizmo, the SELECTION OUTLINE, the GI
    /// volume boxes — and a VR eye draws all of that or none of it according to
    /// the host's mode (the editor's preview opens the desk's channel because
    /// watching the editor work is the mode's whole purpose; the Player opens
    /// none of it, exactly as the desktop Player shows none).
    ///
    /// What THIS flag marks is the WEARER'S own furniture: the two CONTROLLER
    /// PROXIES today, and phase 4b's controller ray and hit marker. Every VR
    /// eye draws those, in BOTH modes — a player needs to see their own hands
    /// as much as an author does — and so does the desktop editor viewport, so
    /// the person at the desk can see where the wearer is reaching. It is
    /// ADDITIVE, never instead-of: a node that sets it is drawn by the desktop
    /// AND by the VR session's view.
    ///
    /// Only meaningful on a node that is already a helper; every capture
    /// (probes, shadow maps, GI, the planar mirrors) excludes it exactly as it
    /// excludes an ordinary helper, and a view that opens NEITHER channel — the
    /// Player, a thumbnail, a preview, the offscreen view a user's screenshot
    /// renders through — draws none of it.
    ///
    /// THE TWO CHANNELS ARE INDEPENDENT (corrected 2026-09-17, VR-4-FIX's
    /// second read): `View::setHelpersVisible(false)` drops kHelperBit ALONE
    /// and `View::setVrHelpersVisible(false)` drops this bit alone
    /// (`helperBitsToDrop`, OgreChain.cpp). A node carrying both — which the
    /// controller proxies and the ray do — is therefore drawn by a view that
    /// keeps EITHER, and kept out of a picture only by a view that opens
    /// neither. The VR channel is off by default on every view, which is what
    /// keeps a screenshot clean; it is not a consequence of the desk switch.
    virtual void        setNodeVrHelper(NodeId, bool) = 0;
    virtual bool        nodeVrHelper(NodeId) const = 0;

    /// THE WEARER'S CONTROLLER PROXIES, BY NODE (SPECS/VR_SPEC.md §5 phase 4).
    ///
    /// The host CREATES the two markers — they are its geometry, its materials
    /// and its visibility, and nothing here changes any of that — and names
    /// them once. A running VR session then PLACES them inside the frame,
    /// immediately after it has located the wearer's hands, so a proxy is drawn
    /// at the pose that same frame renders.
    ///
    /// WHY THE ENGINE AND NOT THE HOST. The poses do not exist until
    /// xrWaitFrame has returned, which happens inside renderOneFrame — after
    /// every host tick of that frame has run. A host pushing them itself can
    /// therefore only push what it knew before the frame began: measured at two
    /// frames of lag (~22 ms at 90 Hz), which on a hand is visible.
    ///
    /// 0 for either id unregisters it. Pass the LEFT hand's node first.
    virtual void        setVrProxyNodes(NodeId left, NodeId right) = 0;
    virtual void        vrProxyNodes(NodeId out[2]) const = 0;

    /// THE CONTROLLER'S RAY AND ITS HIT MARKER, BY NODE (VR_INPUT_SPEC §3,
    /// phase 4b stage 1) — the same arrangement as the proxies above, for the
    /// same reason.
    ///
    /// The host makes both nodes (the mirror does: a unit line down -Z and a
    /// small cross, on kHelperBit | kVrHelperBit) and names them once. A
    /// running session then places them INSIDE its frame from the state the
    /// host pushed with `Engine::setVrRay` — because a ray drawn from a pose
    /// the host knew before the frame began leaves the wearer's own hand.
    ///
    /// WHAT "PLACES" MEANS, EXACTLY (VR-INPUT-1E-FIX finding 2): the marker
    /// stands at `VrRayState::hitPoint` — the place in the world the pick
    /// found, which has not moved — and the LINE runs from THIS frame's aim
    /// pose to that point, so both of its ends are true. With nothing hit the
    /// line runs this frame's aim direction for `length` (0 = ten metres), and
    /// with no ray at all both nodes are hidden.
    ///
    /// AND THE RAY IS NOT A HAND MARKER. It is the pointing tool, so it is
    /// INDEPENDENT of the host's own controller-proxy switch
    /// (`SceneMirror::setVrProxies` / the `vr.proxies` verb): a wearer who
    /// turns the wands off still has to see what they are about to select.
    /// Its nodes are built once and HIDDEN at the session's end (they live for the scene).
    ///
    /// 0 for either id unregisters it. Pass the LINE first.
    virtual void        setVrRayNodes(NodeId line, NodeId marker) = 0;
    virtual void        vrRayNodes(NodeId out[2]) const = 0;

    /// THE WEARER'S OWN HAND, BONE BY BONE (VR_INPUT_SPEC §7, phase 4b stage
    /// 3) — the third arrangement of the same shape, for the third time for the
    /// same reason: the host makes the nodes and owns their mesh, their
    /// material and their visibility, and a running session PLACES them inside
    /// the frame that draws them, because a tracked hand's joints do not exist
    /// until xrWaitFrame has returned.
    ///
    /// `count` nodes for one hand, in the order of `kVrHandBones` (24 of them):
    /// each is a UNIT SEGMENT down -Z that the writer stands at one joint,
    /// turns onto the next and scales to the distance between them
    /// (`vrBoneTransform`, the one definition both writers call). A hand the
    /// runtime is not tracking has every one of its bones HIDDEN, and so has a
    /// hand that is holding a controller — the controller model is then the
    /// honest drawing, and the two never show at once.
    ///
    /// Passing `count` 0 (or a null list) unregisters that hand's bones.
    virtual void        setVrHandBoneNodes(unsigned hand, const NodeId *nodes,
                                          unsigned count) = 0;
    /// How many bone nodes that hand has registered, written into `out` (up to
    /// `count`). 0 = none registered.
    virtual unsigned    vrHandBoneNodes(unsigned hand, NodeId *out,
                                        unsigned count) const = 0;

    /// WHERE A NODE IS RIGHT NOW, in WORLD space, as the graph holds it.
    ///
    /// The read half of setNodeTransform, for the things a host cannot compute
    /// back: a node the ENGINE moved (the controller proxies above), or one
    /// whose parent chain the host does not own. Forces the derived transform
    /// up to date, so the answer is this frame's rather than the last frame the
    /// scene manager happened to walk.
    ///
    /// False (and the outputs untouched) for an unknown id.
    virtual bool        nodeWorldPose(NodeId, Vec3 &position, Quat &rotation) const = 0;

    /// "DOES THIS THING MOVE?" — the document's resolved MOBILITY for one node
    /// (SPECS/REALTIME_REFLECTIONS_SPEC.md §3.3). The host decides it
    /// PREDICTIVELY (a physics body, an avatar, a socket rider, a playing clip,
    /// a rig with a clip, a particle emitter, or anything whose parent moves)
    /// and pushes it on CHANGE; an editor drag is not a promotion, because
    /// flipping an object's GI class costs a full rebuild.
    ///
    /// WHAT IT BUYS (lane R2): a movable object leaves the STILL-WORLD layer.
    /// It is not captured by the reflection probes, it does not voxelize or
    /// bounce light, it is in none of the GI signatures, it stales no probe by
    /// moving and it renders no probe-kind shadow map — while the view, the
    /// planar mirrors, SSR and the view/reflect shadow maps keep drawing it
    /// every frame, in the frame it moves. So a scene full of moving things
    /// costs the room's lighting nothing.
    ///
    /// WHAT IT COSTS: the GI half of the class (voxelize or not) is a GI edge,
    /// so RE-classifying an object the scene has already been lit with costs
    /// one from-scratch GI rebuild — reported in MobilityStatus::
    /// mobilityRebuilds. Push mobility BEFORE the node's geometry (the way a
    /// document walk naturally does) and it costs nothing. The play-time SOFT
    /// promotion costs nothing by construction: see MobilityChange.
    virtual void        setNodeMovable(NodeId, bool,
                                       MobilityChange = MobilityChange::Authoring) = 0;
    virtual bool        nodeMovable(NodeId) const = 0;
    /// How many nodes this scene has been told are movable, split by what they
    /// carry. A pure read of the records — no walk of the graph, no allocation.
    virtual MobilityStatus mobilityStatus() const = 0;

    /// LIGHTING CHANNELS, object side (LightDesc::lightMask is the light side).
    ///
    /// A light lights an object when `light.lightMask & object.lightMask` is
    /// non-zero. Both are born 0xFFFFFFFF, so by default every light lights
    /// every object and nothing here costs anything. There are NO reserved bits
    /// — all 32 are the host's to spend (unlike the visibility/query flags,
    /// where the engine owns specific bits).
    ///
    /// WHAT IT DOES AND DOES NOT COVER, at this engine pin:
    ///   * it filters DIRECT lighting from every light path — directional,
    ///     shadow-casting, Forward+ clustered point/spot, and both area kinds;
    ///   * it does NOT filter SHADOW CASTING. A masked-off object still renders
    ///     into that light's shadow map and therefore still casts a shadow onto
    ///     objects the light does light. The shadow map is one texture shared by
    ///     every receiver, so the caster pass has no receiver to compare a mask
    ///     against; `setNodeCastShadow` below is the escape hatch that DOES
    ///     work, and lights.masks case 6 is the measurement;
    ///   * it does NOT filter INDIRECT light. GI (Instant Radiosity VPLs, VCT)
    ///     bakes/propagates before the mask is consulted, so a masked-off object
    ///     still receives that light's bounce.
    ///   * it applies to the node's ITEM (mesh geometry). Billboards and
    ///     particle systems are not masked: PFX2 definitions are POOLED and
    ///     shared between nodes, so a per-node mask on a def would leak.
    ///
    /// Applies immediately, survives an Item rebuild (the engine re-applies it
    /// on attach), and is remembered for a node whose geometry has not arrived
    /// yet. Not inherited: set it on every node that carries geometry.
    virtual void        setNodeLightMask(NodeId, unsigned mask) = 0;
    virtual unsigned    nodeLightMask(NodeId) const = 0;

    /// PER-OBJECT SHADOW CASTING (Unreal's `Cast Shadow` tick).
    ///
    /// False takes this node's geometry out of EVERY shadow map — the sun's
    /// PSSM splits, every lamp's focused map, the planar mirrors' and the
    /// probes' — while leaving it fully lit and fully visible. True (the
    /// default) is Ogre's own. It is the escape hatch for the things that
    /// should not darken a room: a ground plane, a sky dome, a backdrop, a
    /// decorative interior shell.
    ///
    /// Ogre stores it as the LAYER_SHADOW_CASTER visibility bit, which our own
    /// setVisibilityFlags calls do not touch — so it survives a helper/movable
    /// reclassification. It does NOT survive an Item rebuild (a material swap
    /// destroys and recreates the Item), so the engine remembers it per node
    /// and re-applies it on attach, exactly as it does for the light mask, and
    /// it may be set before any geometry has arrived. Not inherited: set it on
    /// every node that carries geometry. Cached lamp maps that could see the
    /// node re-render in the frame it changes.
    virtual void        setNodeCastShadow(NodeId, bool) = 0;
    virtual bool        nodeCastShadow(NodeId) const = 0;

    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md). Scene-level, like GI. ----
    /// Applies the reflection state idempotently. Pushing the same params twice is
    /// free; a CHANGE rebuilds the whole arm (render targets, cameras, private
    /// workspaces) and a budget change additionally recompiles PBS shaders, so
    /// hosts may call this every frame but must not animate the values.
    /// `budget == 0` tears everything down and costs nothing.
    ///
    /// Only ONE scene per process can have reflections at a time: the receiving
    /// half lives on the process-wide HlmsPbs, exactly like VCT/PCC. The last
    /// scene to enable owns the binding; enabling on a second scene disables the
    /// first (which is why the host arms this on the editor scene only).
    virtual bool        setPlanarReflections(const PlanarReflectionParams &) = 0;
    /// Makes (or un-makes) a node a reflection plane. The node must already have
    /// a mesh attached, and that mesh must be PLATE-LIKE — its thinnest local
    /// extent no more than a tenth of the next — because the plane, its size and
    /// its normal are all derived from the mesh's own bounds. A sphere or a cube
    /// is refused (false, lastError()); the 20-degree matching rule would make it
    /// look broken rather than merely wrong.
    ///
    /// The plane's normal is the node's thin axis in the POSITIVE direction: the
    /// top of a floor reflects, the underside does not. The reflector is excluded
    /// from its own reflection render, so a mirror never contains itself.
    /// Reflectors survive `setPlanarReflections` changes; the flag is remembered
    /// even while the budget is 0.
    virtual bool        setNodePlanarReflector(NodeId, bool) = 0;
    virtual bool        nodePlanarReflector(NodeId) const = 0;
    /// How many reflection planes actually rendered last frame — the "achieved"
    /// number against the requested budget (planes off screen do not render).
    /// 0 when reflections are off or nothing has rendered yet.
    virtual int         activePlanarReflectors() const = 0;

    // ---- Hardware ray tracing (owner, 2026-09-15; ledger §425) -----------
    /// WHAT THE SCENE WAS AUTHORED FOR (RayTracingMode). Scene-level, like GI
    /// and planar reflections, and pushed by the host from the document — ray
    /// tracing is a property of the PROJECT, and whether it happens is that
    /// property met with what the machine can do. Idempotent and free: it
    /// stores an enum, builds nothing and destroys nothing.
    virtual void           setRayTracing(RayTracingMode) = 0;
    /// What was last pushed (Auto until a host says otherwise).
    virtual RayTracingMode rayTracingMode() const = 0;
    /// THE ONE PREDICATE EVERY RAY-CONSUMING STAGE READS: does THIS scene trace
    /// on THIS machine? = the scene's state is not Off, AND the device
    /// advertises ray queries, AND the process is not latched off
    /// (`--no-ray-query` / JAHSHAKA_NO_RAY_QUERY, the diagnostic switch that
    /// makes a ray-capable box render the picture a machine without the
    /// hardware renders). False in every headless engine.
    ///
    /// Auto and On answer identically here, on purpose: they render the same
    /// picture and differ only in whether the EDITOR tells the author that this
    /// machine fell short (SceneIssues, "rays.absent").
    virtual bool           rayTracingResolved() const = 0;
};

/// A view onto a Scene, rendering into a native window supplied by the host or
/// into an offscreen texture. Owned by the Engine: destroy with Engine::destroyView().
class View {
public:
    virtual ~View() = default;
    virtual const std::string &name() const = 0;
    /// Binds a Scene to this View. Call after createScene(); a View renders nothing
    /// until a Scene is attached. A View holds at most one Scene: binding a second
    /// while one is attached fails (false, lastError()). Pass null to detach.
    virtual bool setScene(Scene *) = 0;
    virtual Scene *scene() const = 0;
    /// Full camera state in one call (step 5). The document camera is pushed
    /// through this every frame. This is the ONLY way to move a View's camera.
    virtual void setCamera(const CameraDesc &) = 0;
    /// Rides the view's camera ON a scene node, instead of positioning it from
    /// the pushed CameraDesc (AVATAR_RIG_PERF_SPEC §4.6, phase P2b).
    ///
    /// WHY IT EXISTS. A CameraDesc is filled from the camera node's world
    /// transform read BEFORE the frame, so a camera on a socket is one frame
    /// behind the bone it rides even though the rider NODE is exact — the tag
    /// point resolves inside the frame, the desc was read outside it. Attaching
    /// Ogre's camera to that node closes the gap: the camera derives its
    /// transform from the node hierarchy, tag points included, in the frame that
    /// renders.
    ///
    /// While a camera node is set, setCamera's POSITION and ORIENTATION are
    /// ignored (the node is the pose); every other field — the lens, the clip
    /// planes, the projection, the letterbox, the shift — still applies.
    /// `node = 0` returns the camera to the pushed pose. The node must belong to
    /// this view's scene.
    virtual bool setCameraNode(NodeId node) = 0;
    /// The node the camera rides, or 0.
    virtual NodeId cameraNode() const = 0;
    /// Clear colour behind the scene (the document's flat sky colour). Cheap to
    /// call with the same value; a change rebuilds the view's compositor workspace.
    virtual void setBackground(const Colour &) = 0;
    /// KEPT: cheap introspection the host needs to avoid redundant (workspace-
    /// rebuilding) setBackground calls, and pinned by the engine suites.
    virtual Colour background() const = 0;
    /// Shadow maps for this view (PSSM for directional, focused for point/spot;
    /// lights opt in with LightDesc::castShadows). Off by default; toggling rebuilds
    /// the view's workspace.
    virtual void setShadows(bool) = 0;
    virtual bool shadows() const = 0;
    /// A disabled View is skipped by renderOneFrame(). Hidden viewports MUST be
    /// disabled — the backend otherwise keeps drawing them at full cost.
    virtual void setEnabled(bool) = 0;
    /// KEPT: cheap introspection — hosts query it before pushing per-frame state
    /// into a View they may have disabled; pinned by the engine suites.
    virtual bool isEnabled() const = 0;
    /// Asks for a new size in LOGICAL POINTS (see Engine::createView on units).
    /// Cheap and idempotent: for an on-screen View this only records the request
    /// — it is applied once, at frame time, by the next renderOneFrame(), so a
    /// layout burst costs one swapchain rebuild rather than one per event. An
    /// offscreen View's texture is replaced immediately (an RTT cannot resize in
    /// place), which also drops whatever was rendered into it.
    virtual void resize(unsigned width, unsigned height) = 0;
    /// The render target's ACTUAL size, in PIXELS — not what resize() was last
    /// asked for. Exactly like sampleCount() reports the ACHIEVED sample count:
    /// a swapchain follows the native window (on X11 the surface's currentExtent
    /// wins outright), a request made this frame lands at the next frame, and a
    /// window manager may never grant it at all. Hosts that need to know what is
    /// really being drawn — and every test that asserts a resize took — must read
    /// these, not their own request.
    virtual unsigned width() const = 0;
    virtual unsigned height() const = 0;
    /// Hardware anti-aliasing (MSAA) for this view's render target: 1 = off,
    /// 2/4/8 typical (values are rounded down to a power of two and clamped).
    /// NOT cheap on change: the render target is recreated — on-screen at the
    /// next frame (the resize path), offscreen immediately. Calling again with
    /// the value already requested is free, so hosts may push it per frame.
    /// The driver may clamp the request (Vulkan only guarantees 1 and 4);
    /// sampleCount() reports the ACHIEVED count once the target exists.
    virtual void setSampleCount(unsigned samples) = 0;
    virtual unsigned sampleCount() const = 0;
    /// KEPT: cheap introspection — readPixels() only works offscreen, so callers
    /// (thumbnails, tests) branch on this; pinned by the engine suites.
    virtual bool isOffscreen() const = 0;

    /// The post-processing chain for this View (POST_CHAIN_SPEC.md): HDR +
    /// filmic tonemap, bloom, SSAO, SMAA, SSR, refractive glass.
    ///
    /// IGNORED ON OFFSCREEN VIEWS, always and by construction — postFx() then
    /// reports what was asked for, and the chain stays the simple one. That is
    /// what keeps thumbnails, material previews and every pixel suite exact.
    ///
    /// Cheap to call with an unchanged value (hosts may push per frame). A
    /// change to an ENABLE flag rebuilds the workspace; a change to a tuning
    /// value is a uniform and rebuilds nothing.
    virtual void setPostFx(const PostFxDesc &) = 0;
    virtual const PostFxDesc &postFx() const = 0;

    /// CUT THE EXPOSURE INSTEAD OF FADING IT (CAMERA_LENS_SPEC §4).
    ///
    /// The HDR chain's automatic exposure is a TEMPORAL filter: a 1x1 history
    /// texture that converges on the measured luminance at ~75% per second, so
    /// a cut from one camera to another with a different exposure re-adapts
    /// over a visible one to two seconds. That is right for a light coming on
    /// and wrong for a CUT — a camera change is a new shot, not a new lighting
    /// condition, and even a MANUAL exposure fades across one (the clamp pins
    /// what the chain measures, not what the history holds).
    ///
    /// This re-seeds that history from the view's CURRENT exposure setting, so
    /// the next frame starts at the new grade instead of arriving at it. Call
    /// it AFTER pushing the new camera's post description, and only on a cut:
    /// calling it while a slider moves would turn a ramp into a series of
    /// steps.
    ///
    /// A NO-OP unless this view's chain has the automatic HDR exposure — no
    /// HDR, a fixed tonemap, an offscreen view with no chain, or no workspace
    /// yet: nothing to re-seed, no error.
    virtual void resetExposureHistory() = 0;

    /// THE SAME RE-SEED, TO A KNOWN VALUE (lane PLAYER-1). `scale` is the
    /// tonemapper's own multiplier — i.e. exactly what measuredExposureScale()
    /// reports — and 0 means "the descriptor's own seed", which is what
    /// resetExposureHistory() is.
    ///
    /// WHAT IT IS FOR: a SECOND on-screen view of the same scene taking the
    /// screen. The adaptation history is per view by design (two views may be
    /// pointed at different parts of a world), but a view that has never
    /// presented starts from the authored exposure midpoint and walks to the
    /// scene's real luminance over the next second — visibly, on the frame the
    /// user switched pages. Seeding it from the view that was just showing
    /// removes that walk without making the two views share a history.
    ///
    /// IT SURVIVES A CHAIN THAT CANNOT TAKE IT YET, which is the case it exists
    /// for: a view that has not been shown carries the PASSTHROUGH chain, which
    /// has no seed pass at all, and its HDR chain is built a moment later by
    /// the host's first world push. A value handed over in that window is
    /// REMEMBERED and spent on the chain the view next builds, before that
    /// chain has rendered anything — exactly once; a build with no automatic
    /// exposure drops it rather than holding a stale value for some later
    /// rebuild. `scale` <= 0 (or non-finite) clears anything remembered and
    /// means "the descriptor's own seed", which is resetExposureHistory().
    ///
    /// Otherwise it takes effect on the next rendered frame (the seed is a
    /// clear pass).
    virtual void seedExposureHistory(float scale) = 0;

    /// DOES THIS VIEW DRAW THE EDITOR'S FURNITURE? (lane PLAYER-1.)
    ///
    /// The grid, the light/camera/decal wires and icons, the gizmo, the
    /// selection shell, the GI volume boxes — everything a host marked with
    /// Scene::setNodeHelper. True by default; false is what the Player page
    /// asks for, because the Player is a second View on the EDITOR'S scene and
    /// the furniture is in that scene.
    ///
    /// It is a per-pass visibility mask, so it costs nothing per frame and
    /// cannot desynchronise from the scene — but it IS graph shape, so setting
    /// it rebuilds this view's workspace (and restarts its adaptation history:
    /// see seedExposureHistory). Set it once, when the view is created.
    ///
    /// NOT affected: backdrops (Scene::setNodeBackdrop — the ground's horizon),
    /// the sun disc, and every piece of real scene content.
    virtual void setHelpersVisible(bool) = 0;
    virtual bool helpersVisible() const = 0;

    /// ...AND DOES IT DRAW THE VR CHANNEL? (SPECS/VR_SPEC.md §5 phase 4; owner
    /// 2026-09-17.)
    ///
    /// A SECOND, INDEPENDENT helper channel (Scene::setNodeVrHelper) for the
    /// furniture that belongs to the WEARER rather than to the desk: the
    /// controller proxies today, and phase 4b's controller ray, hit marker and
    /// in-VR gizmo. Every VR eye draws it in BOTH modes — a player needs to see
    /// their own hands as much as an author does — and so does the desktop
    /// EDITOR viewport, so a person at the desk can see where the wearer is
    /// reaching.
    ///
    /// Off by default, which is what keeps it out of everything else: a
    /// thumbnail, a preview, the Player's desktop window and the offscreen view
    /// a user's screenshot renders through never open it. Same mechanics as
    /// setHelpersVisible — a per-pass mask, graph shape, set it once at view
    /// creation.
    virtual void setVrHelpersVisible(bool) = 0;
    virtual bool vrHelpersVisible() const = 0;

    /// DOES THIS OFFSCREEN VIEW GET THE LOD SWITCH BAND? (ogre-patch 0075,
    /// ATOM-3; lane ENGINE-SMALL-A's LOD-LATCH-1, 2026-09-18.)
    ///
    /// The band (`ChainDesc::lodHysteresis`) holds an object's LOD level across
    /// a window of the switch distance so a camera dithering on a threshold does
    /// not pop, and it belongs to a picture somebody WATCHES OVER TIME: it is on
    /// for every ON-SCREEN view and for the VR session's view (offscreen only
    /// because both eyes share one texture), and OFF for every thumbnail,
    /// preview, screenshot and pixel suite, which must take the level their own
    /// value asks for so one pose is always one set of pixels.
    ///
    /// This is the deliberate exception, per view, and it is here because
    /// `View::readPixels` refuses an on-screen view (its target is a swapchain)
    /// — so the one kind of view whose pixels a test can READ is the one kind
    /// that has no band, and a suite that wants to see what the band does has no
    /// reachable subject without it. It replaces the process-wide env latch
    /// `JAHSHAKA_LOD_HYSTERESIS_OFFSCREEN`, which is DELETED: an environment
    /// variable read once per process could not be scoped to a view, could not
    /// be set by a host at all, and was invisible in the description of the
    /// picture it changed — the same reason the post chain, the overlay and the
    /// PiP each carry their own `allowOffscreen`.
    ///
    /// False by default, and nothing in Studio sets it. GRAPH SHAPE (the band is
    /// written onto the pass definitions), so setting it rebuilds this view's
    /// workspace and restarts its adaptation history — set it once, when the
    /// view is created. Ignored on an on-screen view, which has the band anyway.
    virtual void setLodHysteresisOffscreen(bool) = 0;
    virtual bool lodHysteresisOffscreen() const = 0;

    /// WHAT THIS VIEW'S AUTOMATIC EXPOSURE HAS ACTUALLY CONVERGED ON, as the
    /// tonemapper's own multiplier (SS1, 2026-09-13) — the number the shader
    /// samples as `fInvLumAvg`, read back off the GPU's 1x1 adaptation history.
    ///
    /// WHAT IT IS FOR. A one-shot offscreen view (a screenshot, an export
    /// frame) lives about two frames and can therefore NEVER converge: its own
    /// automatic exposure would grade at whatever it was seeded with. Handing
    /// it this value through `PostFxDesc::exposureScale` + `tonemapFixed` makes
    /// the shot grade exactly like the view the user is looking at, and makes
    /// it deterministic while it is at it, because by then it is a constant.
    ///
    /// COSTS A GPU STALL (a 1x1 download with accurate tracking). Call it once
    /// per picture, never per frame.
    ///
    /// 0 means there is nothing to read: no HDR in this view's chain, the fixed
    /// (already-constant) form, an offscreen view with no chain, or a view whose
    /// CURRENT workspace has not presented a frame yet — a rebuild (any post-fx
    /// SHAPE change: switching SSR or SSAO off, resizing) destroys and recreates
    /// the adaptation history, so "has drawn a frame" is asked of the graph, not
    /// of the view. 0 is not an error, and a caller should fall back to the
    /// grade it would have used anyway.
    virtual float measuredExposureScale() const = 0;

    /// The engine-drawn overlay for this View (STATS_OVERLAY_SPEC.md §5.1):
    /// a corner stats readout and/or a full-view loading cover.
    ///
    /// IGNORED ON OFFSCREEN VIEWS unless ViewOverlayDesc::allowOffscreen — the
    /// same guarantee, in the same one place, as setPostFx. overlay() still
    /// reports what the host asked for.
    ///
    /// Cheap to call with an unchanged value, and cheap to TOGGLE: showing or
    /// hiding the overlay is element state, never a workspace rebuild
    /// (workspaceGeneration does not move). Only a change to `allowOffscreen`
    /// on an offscreen view changes the graph.
    ///
    /// The captions are recomposed once per frame from the ENABLED view's desc:
    /// Ogre's overlay set is process-wide, so two on-screen Views cannot show
    /// different text simultaneously (see ViewOverlayDesc's constraint note).
    virtual void setOverlay(const ViewOverlayDesc &) = 0;
    virtual const ViewOverlayDesc &overlay() const = 0;

    /// The picture-in-picture inset for this View (CAMERAS_SPEC §7.7): a second
    /// camera's view of the SAME scene, composited into a rectangle of this
    /// View's target. See ViewPipDesc for the mechanism and every rule it obeys.
    ///
    /// This is the spec's `setPipCamera(CameraDesc|null, rect)` in this
    /// boundary's own idiom: the camera, the rect, the inset's background and
    /// the offscreen opt-in travel together in one value, exactly like
    /// setPostFx and setOverlay — "null" is `ViewPipDesc::enabled = false`.
    ///
    /// IGNORED ON OFFSCREEN VIEWS unless ViewPipDesc::allowOffscreen — the same
    /// guarantee, in the same one place, as setPostFx and setOverlay. pip()
    /// still reports what the host asked for.
    ///
    /// Cheap to call with an unchanged value (hosts may push per frame), and
    /// cheap to MOVE: changing only the rect's POSITION, the camera or the
    /// exposure is a live viewport modifier / camera / clear-colour write,
    /// never a workspace rebuild (neither workspaceGeneration nor
    /// pipGeneration moves). Three things are structural, and only three:
    /// turning the inset on or off, flipping `tonemap`, and RESIZING the rect
    /// (Route C's local texture is sized from it — see ViewPipDesc).
    virtual void setPip(const ViewPipDesc &) = 0;
    virtual const ViewPipDesc &pip() const = 0;
    /// How many times the INSET's own workspace has been created — the
    /// counterpart of workspaceGeneration for the second workspace, and the
    /// number that proves a steady inset costs nothing structural per frame.
    /// It moves when the inset is switched on, when its local texture has to
    /// be re-sized (a rect resize or a `tonemap` flip), and each time the main
    /// workspace is rebuilt (the inset must be re-appended to stay LAST on the
    /// target — there is no reorder API). Never on a rect MOVE, a camera move
    /// or an exposure change. 0 while there is no inset.
    virtual unsigned pipGeneration() const = 0;
    /// How many times this View has (re)built its compositor workspace — the
    /// structurally expensive operation behind setShadows(), setBackground(),
    /// resize(), setSampleCount() and the engine's shadow-atlas rebuild. Starts
    /// at 0 and reaches 1 when a Scene is first bound. Hosts use it to verify
    /// that a per-frame push really was free; tests use it to pin the fact that
    /// there is exactly ONE place a workspace is created (POST_CHAIN_SPEC.md).
    virtual unsigned workspaceGeneration() const = 0;
    /// How many frames this View has actually drawn AND presented since its
    /// current Scene was bound — the honest "are there real pixels in that
    /// window yet?" signal. A frame counts only when the View was enabled, had
    /// a live workspace and a bound Scene while Engine::renderOneFrame ran, so
    /// a hidden, scene-less or workspace-less View never inflates it. Binding a
    /// Scene (or detaching one) resets it to 0; a workspace REBUILD does not —
    /// the pixels of the previous frame are still on screen.
    ///
    /// Hosts use it to know when the window stopped showing stale pixels: the
    /// editor's loading cover (src/viewport/viewportcover.h) is on screen until
    /// this passes its threshold.
    virtual unsigned long long framesPresented() const = 0;
    /// HOW MANY TIMES THIS VIEW'S PER-FRAME CHAIN GLOBALS HAVE BEEN PUSHED
    /// (lane EYE-GRADE-1's fix round; the mechanism is one workspace listener
    /// per view, firing immediately before that view's passes execute).
    ///
    /// WHY IT IS WORTH AN ACCESSOR. Most of the post chain is SHAPE — built
    /// into the compositor graph — and a few values are written into the graph
    /// the moment a host pushes them (the fixed exposure's clear colour). But
    /// the METER's uniforms, the automatic exposure's terms, the bloom
    /// threshold, the AO and SSR camera terms and every look's parameters are
    /// PROCESS-WIDE material parameters, pushed per view per frame in that
    /// listener, and a view whose count does not climb is a view rendering with
    /// whatever the last workspace to update happened to leave in them. That is
    /// invisible in a picture until two views disagree — which is exactly the
    /// case a headset introduced — so it is countable rather than inferable.
    ///
    /// 0 for a view with no chain (every thumbnail, preview and pixel suite:
    /// they have no effects, so no listener is ever created for them).
    virtual unsigned long long globalsPushes() const = 0;
    /// Reads this View's rendered pixels back to the CPU. Offscreen Views only —
    /// returns false for on-screen windows. This is the thumbnail path, and what
    /// makes the engine testable without a window.
    virtual bool readPixels(Image &out) = 0;

    /// Compiles every shader this View's SCENE needs, now, without drawing it
    /// (SHADER_CACHE_SPEC.md §5 — the PSO-precache half).
    ///
    /// The engine builds a shader per renderable, on first draw. Left alone,
    /// that means the first frames of a freshly-opened world stutter through
    /// dozens of compiles while the user is looking at them. This does the same
    /// work early: it renders a WARM-UP pass over this view's scene — Ogre
    /// walks the live render queues itself, so there is no material list to
    /// build or maintain — into a 4x4 target, which forces every variant the
    /// scene actually needs (including shadow casters) to be generated and
    /// compiled. Nothing is presented and no pixel of the real view changes.
    ///
    /// SYNCHRONOUS, and that is the point: the caller holds its loading cover
    /// up until this returns. It therefore LENGTHENS a cold open by however
    /// long the compiles take, and shortens every frame after it.
    ///
    /// Requires a scene (setScene first). Returns false with lastError() set if
    /// there is nothing to warm up or the warm-up pass could not be built; a
    /// failure is never fatal — the shaders simply compile later, as before.
    virtual bool warmUpShaders() = 0;
};

/// Owns the device and every Scene and View.
///
/// ONE PER PROCESS. The backend is a process-wide singleton; create() refuses to
/// make a second Engine while one is alive (returns null + error). Destroying it
/// and creating another later is supported (tests/engine/test_engine_recreate) —
/// this needs the Ogre-Next patch recorded in OGRE_PLATFORM_DEPS.md.
///
/// LIFETIME CONTRACT: every View and Scene pointer handed out is owned by the
/// Engine and dies with it. Hosts that cache a View* (e.g. a widget) must call
/// destroyView() before the Engine is destroyed, or must check the Engine is still
/// alive before touching the pointer — see EngineViewWidget for the pattern.
class Engine {
public:
    virtual ~Engine() = default;

    /// Creates the engine. Returns null on failure and fills `error`.
    static std::unique_ptr<Engine> create(const EngineConfig &, std::string &error);
    /// True while an Engine exists in this process.
    static bool isAlive();

    /// ORDER MATTERS. A View must be created before any Scene: the underlying engine
    /// only starts its material and buffer systems when the first render target
    /// exists, and creating a Scene before that dereferences null.
    ///   createView(...)  ->  createScene(...)  ->  view->setScene(scene)
    /// Names must be unique among live Views; a duplicate returns null (lastError()).
    ///
    /// UNITS (the contract for every size that crosses this boundary, in or out):
    /// `width`/`height` here — and every later View::resize() — are LOGICAL
    /// POINTS, the units the host's toolkit lays out in (Qt's QWidget::width()).
    /// The window backend converts: on X11 at the current pin the conversion is
    /// the identity, on macOS the Metal window multiplies by the layer's
    /// contentsScale. What comes BACK — View::width()/height() — is the render
    /// target's real size in PIXELS, which is why it can differ from what was
    /// pushed (see View::width). Points == pixels on every unscaled display, so
    /// the two only diverge on HiDPI, which is its own program and not handled
    /// anywhere in this tree yet (deep audit area 7 F4).
    virtual View  *createView(const std::string &name,
                              NativeWindowHandle, unsigned width, unsigned height,
                              const Colour &background) = 0;
    /// An offscreen View: renders to a texture instead of a window. Needs no native
    /// handle, so it works headless. Used for thumbnails, asset previews and tests.
    virtual View  *createOffscreenView(const std::string &name,
                                       unsigned width, unsigned height,
                                       const Colour &background) = 0;
    /// Releases the View's window/texture and camera. Any Scene it showed survives.
    /// Null or unknown pointers are ignored.
    virtual void   destroyView(View *) = 0;

    /// Returns null if called before the first createView()/createOffscreenView(),
    /// or if the name is already in use (lastError()).
    ///
    /// `workerThreads` sizes THIS SCENE'S OWN worker pool — the threads the
    /// backend forks culling, render-queue building and object updates across.
    /// Every scene gets its own pool (they are not shared), so the number is a
    /// per-scene decision and not a global one: the on-screen editor scene
    /// wants the machine, a 128x128 thumbnail scene wants one thread and no
    /// barriers. 0 means "the backend's default" (2), which is what every
    /// caller that does not care should pass. Clamped to [1, 32].
    ///
    /// MORE IS NOT FREE. The pool synchronises through a barrier per parallel
    /// pass, so at small scene sizes the barrier cost outweighs the split work
    /// — measure before raising it (tests/benchmarks/bench_scenegraph has a
    /// `--threads` flag for exactly this).
    ///
    /// AND LESS THAN ONE IS A REAL ANSWER: pass `kSceneMainThreadOnly`
    /// (Types.h) for a scene that should have NO worker threads at all
    /// (SPECS/THREADING_ADOPTION_SPEC.md P5). That is a different mode, not a
    /// smaller pool — the backend spawns nothing and every parallel pass runs
    /// inline with no barrier, instead of waking one thread and paying two
    /// barrier syncs to do the same serial work. It is what a staging scene
    /// manager (which is never drawn) and a 128x128 thumbnail scene actually
    /// want. It cannot be spelled `0`, because 0 has always meant "I do not
    /// care, give me the default".
    ///
    /// NOT FOR A SCENE THAT COMPILES SHADERS: parallel Hlms and warm-up compile
    /// need more than one worker (OgreRenderQueue.cpp:588), and 0 and 1 are
    /// equally serial there. The startup warm-up scene is the worked example —
    /// see the note on Tier::Utility in src/bridge/sceneworkerthreads.h.
    virtual Scene *createScene(const std::string &name, unsigned workerThreads = 0) = 0;

    /// The scene manager DETACHED document nodes live in, opaque
    /// (`Ogre::SceneManager*`). SPECS/SCENEGRAPH_SPEC.md D2: a document node IS
    /// an engine node, so one has to exist for nodes that are not (yet) in any
    /// rendered scene — every node an importer builds, everything the undo stack
    /// holds, every document that has not met a SceneMirror. It renders nothing
    /// and is never bound to a View.
    ///
    /// Creating it forces the backend's one-time preparation — the
    /// Hlms/resource registration a scene manager cannot exist without, and, on
    /// a RENDERING boot only, a surfaceless window to hang that registration
    /// on. A HEADLESS engine (EngineConfig::headless) already has both when
    /// create() returns, so asking for this costs nothing there.
    ///
    /// Hosts ask for it once, before their first document node, and hand it to
    /// iris::graph. On a rendering boot, asking BEFORE the first real View is
    /// what makes the backend create that surfaceless window ahead of the
    /// on-screen one; hosts that can wait should wait (Studio's EngineHost
    /// registers it lazily for exactly this reason).
    virtual void *documentGraphScene() = 0;
    /// True when this engine was created with EngineConfig::headless — the NULL
    /// render system, no display, no device, and no View of any kind. Hosts
    /// that decide what to show read it instead of guessing from a failed
    /// createView().
    virtual bool  isHeadless() const = 0;
    /// Destroys the Scene and every node, mesh and material it owns. Views bound to
    /// it are detached first (they stay alive, showing nothing).
    ///
    /// CALLED FROM INSIDE A FRAME IT IS DEFERRED to that frame's tail
    /// (VR-INPUT-1E-FIX finding 5) — a VR session bound to the scene cannot be
    /// ended between its own xrBeginFrame and xrEndFrame, and the render system
    /// is holding the scene's buffers for the frame in flight. Nothing in this
    /// tree does it; a host that does may treat the Scene as gone the moment it
    /// has asked, which is why the call is honoured late rather than refused.
    virtual void   destroyScene(Scene *) = 0;

    /// THE HOST'S TRANSFORM-WRITE EPOCH — how the renderer learns that nothing
    /// moved (clean-2 lane, 2026-09-13).
    ///
    /// The document owns the scene graph and writes transforms into it
    /// directly, so a dragged mesh reaches the engine through no call at all.
    /// The renderer's only answer was to LOOK: `OgreScene`'s GI movement scan
    /// reads every item's updated world AABB once a frame in any probe-lit
    /// scene — 0.4 / 2.0 / 4.3 ms at 1k / 5k / 10k nodes, still or not.
    ///
    /// Hand it the address of a counter the host bumps on every transform
    /// write (`iris::graph::transformWriteCounter()`) and the scan runs only on
    /// frames where SOMETHING was written: a still scene pays one relaxed
    /// atomic load. The engine's OWN writes (`Scene::setNodeTransform`, socket
    /// riders) are counted internally and need no help from the host.
    ///
    /// RELAXED BY DESIGN: a count of writes, not of moves, process-wide rather
    /// than per scene. Every one of those errs towards scanning when nothing
    /// moved, which costs what the unconditional scan cost. The pointer must
    /// outlive the engine (a process-lifetime counter); null (the default)
    /// means "no epoch available" and every frame scans, which is why a host
    /// that never calls this is simply as slow as before.
    virtual void setTransformWriteCounter(const std::atomic<unsigned long long> *counter) = 0;

    /// Draws every enabled View once. The host owns the loop and calls this.
    ///
    /// EVERY EXIT FROM IT CLOSES THE FRAME (lane FRAME-CATCH-1): a frame that
    /// throws still ends the OpenXR frame it opened (releasing the eye
    /// swapchain images it acquired), still closes the monitor's record, still
    /// ends a session the runtime has taken away, still latches a lost device
    /// and still honours a `destroyScene` asked for from inside it. A host
    /// therefore never has to guess whether a frame that answered
    /// `lastError()` left something open.
    virtual void renderOneFrame() = 0;

    /// TEST-FACING: MAKE THE NEXT `frames` FRAMES FAULT (`FrameFault`, lane
    /// FRAME-CATCH-1, 2026-09-18).
    ///
    /// The one thing in the engine no legal call can produce and no suite can
    /// assert without: a frame that THROWS. The fault is raised inside
    /// `renderOneFrame`, after the frame has rendered and before anything
    /// closes, which is where a device loss surfaces; `lastError()` afterwards
    /// carries the fault's own message, so a suite can prove the fault really
    /// fired rather than passing vacuously.
    ///
    /// `FrameFault::None` (or `frames` 0) disarms. Nothing in Studio calls
    /// this — it exists for `engine.frame_catch` and `vr.session`.
    virtual void setFrameFault(FrameFault fault, unsigned frames) = 0;

    /// THE GPU IS GONE AND THIS PROCESS CANNOT COME BACK FROM IT (lane XID-2,
    /// 2026-09-17). True once the render system has reported a lost device — on
    /// this driver that is an `NVRM: Xid` in the kernel log and a
    /// `VK_ERROR_DEVICE_LOST` from a fence wait. Nothing renders again after it:
    /// the render system vetoes every frame from then on.
    ///
    /// A HOST THAT SEES THIS MUST SAY SO AND END THE PROCESS WITHOUT AN ORDERLY
    /// TEARDOWN. `vkDestroyDevice` on a device whose channel the driver has not
    /// reclaimed does not return (measured: it spins at 100 % of a core for
    /// ever), so running destructors is how an application FREEZES instead of
    /// ending — which is what the owner saw. Log, tell the user, `_exit`.
    virtual bool deviceLost() const = 0;

    /// ADVANCES THE RENDERER'S RESOURCE BOOKKEEPING WITHOUT DRAWING ANYTHING
    /// (lane OPEN-FRAMES-1, 2026-09-15).
    ///
    /// A FRAME IS NOT ONLY PIXELS. Every mesh upload, every texture and every
    /// buffer this engine frees is handed back through per-frame machinery that
    /// only a frame turns: the texture manager's worker command buffer and its
    /// staging recycle, and the buffer manager's frame counter, which is what
    /// releases the blocks a destroyed mesh left behind. The host's render tick
    /// is the only thing that calls it.
    ///
    /// SO A HOST THAT DOES GPU WORK WITHOUT RENDERING IS ACCUMULATING IT. An
    /// install that runs over many event-loop turns — Studio's threaded project
    /// open — can upload a whole world and destroy the previous one while a
    /// chain of posted events starves the render timer, and NOTHING advances
    /// until the backend's own emergency threshold trips inside an allocation.
    /// (Measured by the diagnosis: 33 crashes in 59 runs of a scripted open
    /// that renders no frame, 0 in 24 with one frame per turn; by the lane on
    /// its own build: 9/12 base, 6/12 with THIS advance alone, 0/12 with a
    /// frame per slice — so the advance is the primitive, the frame the cure.)
    /// NOTE: the backend commits only every SECOND bare call (see OgreEngine.cpp).
    ///
    /// THIS IS THAT ADVANCE, AND NOTHING ELSE. It updates no scene graph,
    /// culls nothing, submits no draw and presents no window; it does not wait
    /// for a texture to finish streaming (`waitForTextureLoads` is that call).
    /// It is what the frame does with the resources and none of what the frame
    /// does with the picture, so it is safe at any point a frame would be safe
    /// and costs microseconds when there is nothing outstanding.
    ///
    /// Call it at the boundaries of work that allocates or frees GPU
    /// resources outside the render loop. A no-op before the render system
    /// exists (a headless engine, or between Root and initialise).
    virtual void advanceResources() = 0;

    // ---- VR (SPECS/VR_SPEC.md v3 phase 2) ---------------------------------
    /// Did the boot reach an OpenXR runtime? False on every engine booted with
    /// `EngineConfig::vr == VrMode::Disabled` (the default), on a build with no
    /// loader, and on a box whose runtime refused — vrInfo().reason says which.
    /// Fixed for the life of the process.
    virtual bool vrAvailable() const = 0;
    /// The runtime's identity and what it wants. Always safe to read; every
    /// field is empty or zero when unavailable.
    virtual const VrInfo &vrInfo() const = 0;
    /// Begins ONE session on `scene` (there is one per engine, like Root).
    ///
    /// What it creates: an XrSession on the Vulkan device the runtime made at
    /// boot, a reference space (STAGE where the runtime offers one — a FLOOR
    /// origin, VR_SPEC §0's phase-1b lesson — LOCAL otherwise), one swapchain
    /// per eye, a both-eyes render target of 2w x h, and a View of `scene`
    /// drawn with INSTANCED STEREO into it (one scene pass, two eyes). The
    /// session's View becomes the scene's GI driver for as long as it runs, so
    /// the cascades follow the HEAD and not some other camera.
    ///
    /// From the next frame on, `renderOneFrame()` is PACED BY THE RUNTIME: it
    /// blocks in xrWaitFrame at the top, draws both eyes, copies each into the
    /// runtime's swapchain image and submits one projection layer. A host's
    /// timer should go to zero interval and its mirror window's vsync off
    /// (Engine::setVsync) for the duration; nothing breaks if it does not,
    /// the loop simply paces to the slower of the two.
    ///
    /// False = no runtime, a session already running, no such scene, or the
    /// runtime refused; `lastError()` says which. Never throws, never hangs.
    virtual bool beginVrSession(Scene *scene, const VrConfig &cfg) = 0;
    /// Ends the session and puts everything back: the mirror, the View, the
    /// target, the swapchains, the frame's pacing and the render profile the
    /// session imposed. Safe when none is running.
    virtual void endVrSession() = 0;
    /// The runtime's lifecycle state (VrState::Unavailable when no session).
    virtual VrState vrState() const = 0;
    /// Everything a caller can ask about the live session without an Ogre or
    /// OpenXR type crossing the boundary.
    ///
    /// AN INJECTED HAND READS BACK IMMEDIATELY, WITH OR WITHOUT A SESSION
    /// (VR-INPUT-1E-FIX, the lead's item): `input[]` and `hands[]` carry the
    /// injection store's sample as soon as `vrInjectInput` has taken it, not
    /// from the next rendered frame. With a session the frame is still where
    /// the runtime's own answer is REPLACED (readInput, so the proxies and the
    /// ray follow) — but a host that injects a gesture and reads the state back
    /// between frames must see what it just wrote, or every headless gesture
    /// test has to render a frame it does not otherwise need. A hand whose
    /// runtime has bound a real profile is NOT overlaid: the refusal rule is
    /// the same here as everywhere (see vrInjectInput).
    virtual VrStatus vrStatus() const = 0;
    /// The View the session draws into — the both-eyes target. Null when no
    /// session runs. Hosts use it for nothing but introspection; the session
    /// owns its lifetime.
    virtual View *vrView() const = 0;
    /// WHERE THE WEARER IS STANDING, IN THE WORLD (phase 3, the Player's VR
    /// mode). The reference space the session took is a ROOM — a floor origin
    /// (STAGE) with the wearer somewhere on it — and until this is called that
    /// room sits at the world origin, facing down -Z. This places it: `position`
    /// is the world point the room's origin occupies and `yawDegrees` is the
    /// room's heading about +Y, so a wearer who walks a metre north in the room
    /// walks a metre along the rotated north of the world.
    ///
    /// POSITION AND YAW ONLY, and that is physics rather than economy: a room
    /// has a floor and gravity, so pitching or rolling the rig would tilt the
    /// horizon under a standing person, which is the one thing a VR renderer
    /// must never do. Locomotion belongs to the HOST (the Player's fly moves
    /// this); the engine only composes what it is given with the runtime's
    /// pose, and `vrStatus()` reports both halves back.
    ///
    /// Takes effect on the next located frame. A NO-OP WITH NO SESSION, and
    /// not remembered either: where the wearer stands is a property of the RUN
    /// a host started, so a call made BEFORE `beginVrSession` is dropped
    /// rather than inherited by whatever session comes next (a host sets it
    /// right after beginVrSession, and reads it back from `vrStatus().origin`
    /// — the engine's rig is the one truth).
    virtual void setVrOrigin(const Vec3 &position, float yawDegrees) = 0;
    /// Which on-screen (or offscreen) View shows the mirror. Null clears it.
    /// Takes effect on the next frame; the View keeps its own picture
    /// underneath and the mirror is copied over it (VR_SPEC §4.3).
    virtual void setVrMirrorView(View *view) = 0;
    /// Which View the mirror is currently pointed at (null = none). A host with
    /// several pages needs to know whether the mirror is on the one it is about
    /// to hide — a mirror is a workspace of its OWN over that view's target and
    /// does not stop when the view does.
    virtual View *vrMirrorView() const = 0;
    /// ONE EYE OF THE RUNNING SESSION, RENDERED MONO AND READ BACK — the
    /// picture that eye is seeing, at the eye's own size, through the eye's own
    /// pose and projection.
    ///
    /// Two uses, and the second is why it is worth its weight. It is the VR
    /// SCREENSHOT (what did I see in there), and it is the one place where the
    /// stereo path's arithmetic is checked against the engine's ordinary one:
    /// the eyes are drawn from a `VrData` pair the session converts by hand,
    /// this is drawn through `Camera`'s own projection path, and a session that
    /// ever stops converting produces two pictures that disagree about DEPTH.
    ///
    /// It RENDERS FRAMES (a fresh chain has to settle before it can be read),
    /// so it is a tool and a test call, not something to put in a loop. False
    /// when no session is running or the eyes have not been located yet;
    /// `lastError()` says which.
    virtual bool vrEyeScreenshot(unsigned eye, Image &out) = 0;

    // ---- VR INPUT (SPECS/VR_INPUT_SPEC.md §2.4, phase 4b stage 1) ---------
    /// TEST-FACING: WRITE ONE HAND'S SAMPLE AS IF THE RUNTIME HAD REPORTED IT.
    ///
    /// This is the backbone of every gesture test in the tree, and it exists
    /// because the interaction logic is arithmetic on two poses and four
    /// booleans: given this hook it runs — and is asserted — with no headset,
    /// no controller and no runtime at all, which is the only way a VR editor
    /// gets tested on a box in a rack.
    ///
    /// WHAT IT REPLACES. The whole `VrHandState` for that hand, poses
    /// included, from the moment it is called until the next call or until
    /// `vrInjectInput(hand, VrHandState())` clears it (a default state — `valid`
    /// false — is the "stop injecting" spelling). The injected poses are
    /// already in WORLD space, i.e. in the frame `vrStatus()` reports: the rig
    /// is NOT applied to them a second time. A hand under injection is also
    /// the hand `VrStatus::hands[i]` and the controller proxy follow, so a
    /// script can put a wand anywhere and watch what the wearer would see.
    ///
    /// THE REFUSAL RULE, and it is not optional: while a session is running
    /// AND the runtime has bound a real interaction profile for that hand
    /// (`VrStatus::profile`), this REFUSES (false, lastError says so) unless
    /// the process was started with `JAHSHAKA_VR_TEST_INJECT=1`. A smoke in a
    /// headset can therefore never be fooled by a stale injection left behind
    /// by a script — the wearer's own hardware always wins.
    ///
    /// AND THE RULE IS A GUARANTEE, NOT A CHECK ON ONE CODE PATH
    /// (VR-INPUT-1E-FIX finding 1). Three things hold it up:
    ///
    ///   * a WITHDRAWAL (a default state) is NEVER refused — taking a fake
    ///     hand away cannot fool anybody, and refusing it meant the safe
    ///     direction was the one that needed permission;
    ///   * a sample written while nothing was bound (with no session at all,
    ///     or in a session's first frames before the runtime answers) is
    ///     IGNORED AND FORGOTTEN the moment a real profile arrives for that
    ///     hand — with one line in the log — rather than standing in for the
    ///     wearer's hand for the life of the session;
    ///   * the store is EMPTIED at both ends of a session: none inherits a
    ///     script's leftovers and none leaves its own behind.
    ///
    /// With no session and no profile an injection is always accepted and
    /// reported through `vrStatus().input[]` with `fromInjection` true. That is
    /// the headless backbone and it is load-bearing for the Studio side.
    ///
    /// `hand` is VrHandLeft or VrHandRight; anything else is false.
    virtual bool vrInjectInput(int hand, const VrHandState &state) = 0;
    /// TEST-FACING: THE INPUT FOCUS AN INJECTED SESSION HAS
    /// (`VrStatus::inputFocused`; VR-INPUT-1E-FIX).
    ///
    /// Focus is the SESSION's, not a hand's — a runtime takes it away for the
    /// whole application — so this is one bit for the process and not a field
    /// on a sample. It is what `inputFocused` reports while ANY hand is
    /// injected, with a session or without one, and it is how the focus-loss
    /// rule (a gesture in flight is cancelled, never committed) is driven with
    /// no dashboard to raise.
    ///
    /// True by default, and reset to true whenever the store is emptied (both
    /// ends of a session): a test that says nothing about focus means "the
    /// wearer was there", and a `false` cannot outlive the session it was
    /// written for.
    virtual void vrInjectFocus(bool focused) = 0;
    /// WHERE ONE HAND'S JOINTS ARE, in WORLD space through the rig (stage 3,
    /// `XR_EXT_hand_tracking`). Writes up to `count` poses in the extension's
    /// own joint order (see `kVrHandJointCount`) and returns how many it wrote:
    /// 0 when that hand is not being tracked this frame, which is the normal
    /// answer for a wearer holding controllers and for every runtime that has
    /// no hand tracking at all.
    ///
    /// NOT ON `VrStatus`, deliberately: fifty-two poses is two kilobytes on a
    /// value every host copies several times a frame, for an answer only a
    /// drawer and a test ever want. `VrHandState::jointsTracked` is the cheap
    /// bit that says whether asking is worth it.
    ///
    /// Each pose carries its own `valid` (a runtime locates a hand joint by
    /// joint, and a half-occluded hand really does report some and not others).
    virtual unsigned vrHandJoints(int hand, VrPose *out, unsigned count) const = 0;
    /// TEST-FACING: STAND IN FOR A TRACKED HAND'S JOINTS (`vr.inject`'s
    /// `joints`), on the same terms as `vrInjectInput` — this box's simulated
    /// runtime has no hands at all (Monado: `hand_tracking_supported = false`),
    /// so the wearer's own skeleton is the one thing in stage 3 that cannot be
    /// driven by any runtime we can gate on.
    ///
    /// `count` poses in the extension's joint order, up to `kVrHandJointCount`;
    /// `count` 0 (or a null list) stops injecting that hand's joints. THE
    /// WEARER'S HARDWARE WINS, exactly as it does for the controls: a hand the
    /// runtime is really tracking ignores the injection unless
    /// JAHSHAKA_VR_TEST_INJECT is set.
    virtual bool vrInjectJoints(int hand, const VrPose *joints, unsigned count) = 0;
    /// EVERY SUGGESTED-BINDING BLOCK AND WHAT THE RUNTIME DID WITH IT
    /// (`VrBindingBlock`; stage 3's fix round). Writes up to `count` blocks in
    /// the order they were offered and returns how many there are — 0 with no
    /// session, and 0 for a session whose runtime refused the action set.
    ///
    /// Bulk introspection rather than a field on `VrStatus`, for the same
    /// reason the hand joints are: only a report and a suite ever ask, and the
    /// status is copied several times a frame.
    virtual unsigned vrBindingBlocks(VrBindingBlock *out, unsigned count) const = 0;
    /// THE ONE OUTPUT: buzz a controller (product, not a test hook).
    ///
    /// `amplitude01` is clamped to 0..1 and `seconds` to a sane pulse; the
    /// runtime decides what that feels like. False when no session is running,
    /// when the hand is not a hand, or when the runtime refused the call —
    /// NOT when nothing buzzed: a profile with no haptic output (hand
    /// tracking, Monado's simulated controllers) takes the call and does
    /// nothing, which is a supported controller rather than an error.
    virtual bool vrHaptic(int hand, float amplitude01, float seconds) = 0;
    /// THE CONTROLLER'S RAY, AS THE HOST COMPUTED IT (VrRayState) — pushed
    /// every frame while a gesture is live and pushed once with `visible` false
    /// when it is not. The engine only DRAWS it, into the two nodes
    /// `Scene::setVrRayNodes` named, inside the frame; the pick behind
    /// `hitPoint` is the document's and stays the host's business.
    ///
    /// Safe with no session and with no nodes registered (it is then a store).
    virtual void setVrRay(const VrRayState &ray) = 0;
    /// What the engine last stored (introspection, and what the suite asserts).
    virtual const VrRayState &vrRay() const = 0;

    /// RESOLVES ONE SCENE'S GRAPH WITHOUT DRAWING ANYTHING — transforms,
    /// skeletal animations, tag points, bounds and the light list, exactly the
    /// pass `renderOneFrame` runs for the scenes it draws.
    ///
    /// WHY THIS EXISTS (SPECS/THREADING_ADOPTION_SPEC.md P3). Bone transforms,
    /// world AABBs and derived transforms are resolved by the scene-graph
    /// update, which is part of a FRAME. Until P3 the frame updated every scene
    /// manager in the process, so a host that wanted a pose resolved could get
    /// one by calling `renderOneFrame()` with every view disabled — a frame
    /// that drew nothing and updated everything. That is no longer true: the
    /// frame now updates only the scenes an enabled View draws. Hosts that need
    /// a resolved graph WITHOUT pixels — reading a bone after setting a clip
    /// time is the real case — say so here instead.
    ///
    /// Strictly cheaper than the old trick (one scene, no render system work at
    /// all) and correct whether or not the scene's page is on screen.
    /// `clearFrameData` runs with it, so repeated calls do not accumulate the
    /// global light list.
    ///
    /// False = no such scene, or the backend refused; `lastError()` says which.
    virtual bool updateScene(Scene *scene) = 0;

    /// True while at least one View is enabled (View::setEnabled) — i.e. while
    /// renderOneFrame() has anything at all to draw.
    ///
    /// The host's render loop asks this BEFORE calling renderOneFrame, and skips
    /// the frame entirely when the answer is false (deep audit area 7 F8). A
    /// frame with nothing enabled is not free: it still walks every scene, runs
    /// the per-frame interlocks, submits a command buffer and takes a slot in
    /// the present queue — ~62 of them a second while the user sits on a page
    /// that shows no viewport at all. Asking is O(views) and allocates nothing.
    ///
    /// Only View::setEnabled moves this, so nothing else has to change to stay
    /// correct: the tick after a viewport is shown sees `true` and renders.
    virtual bool hasEnabledViews() const = 0;

    /// Every live View, in creation order (the vector is cleared first).
    ///
    /// Hosts need this for the ONE thing hasEnabledViews() cannot express:
    /// temporarily quieting the on-screen views around an offscreen render.
    /// A thumbnail or screenshot readback calls renderOneFrame() twice, and
    /// renderOneFrame draws EVERY enabled View — so each readback also redraws
    /// the whole editor twice and burns two vsync presents on frames nobody
    /// asked for (fps audit F5; a thumbnail queue paced this way holds the
    /// editor at ~20 fps). Disabling the on-screen views for the duration is
    /// the whole fix, and it needs the list.
    ///
    /// The pointers are the Engine's and die with it — hold them for the
    /// duration of a call, never across one that could destroy a View.
    virtual void listViews(std::vector<View *> &out) const = 0;

    // ---- Texture streaming (SPECS/THREADING_ADOPTION_SPEC.md P2) -----------
    //
    // WHAT CHANGED, in one sentence: `loadTexture` used to block the calling
    // thread until that ONE texture was read, decoded and uploaded; now it only
    // SCHEDULES the load, and the wait happens ONCE, at the frame edge, inside
    // renderOneFrame(). N textures therefore decode concurrently instead of one
    // at a time, and the frame that draws them still sees every one of them
    // resident — which is what keeps the pixel suites byte-exact (decision
    // D-C(1); option (2), "do not wait for the on-screen view either", is a
    // visible-quality decision that was deliberately NOT taken here).
    //
    // A HOST NEEDS THESE THREE ONLY FOR ONE-SHOT RENDERS. Anything that draws
    // through the ordinary loop is already covered by renderOneFrame's own
    // wait. What is NOT covered is a caller that renders a fixed number of
    // frames and then reads the pixels back — a thumbnail, an asset snapshot, a
    // screenshot — because a texture whose load request arrives DURING that
    // frame is resident only for the next one. Upstream's own recipe for that
    // (OgreTextureGpuManager.h:849-868) is: wait, snapshot the request counter,
    // render, and if the counter moved, wait and render again.

    /// True when nothing is queued or in flight in the streaming worker(s).
    /// Cheap — one flag and a queue size behind a mutex.
    virtual bool texturesDoneStreaming() const = 0;

    /// Blocks until texturesDoneStreaming() is true, pumping the streaming
    /// worker's completion queue while it waits. Returns the milliseconds spent
    /// waiting (0.0 when there was nothing to wait for), which is the number the
    /// A/B harness reports and the only honest way to say what a "batched" open
    /// actually cost.
    ///
    /// NOT NEEDED before a normal frame — renderOneFrame does it. It is here for
    /// one-shot renders and for scripts that want a provably complete image.
    virtual double waitForTextureLoads() = 0;

    /// A monotonic count of texture LOAD REQUESTS this process has made. Only
    /// differences mean anything: snapshot it, render, compare. It moving across
    /// a render is exactly the condition upstream's double-render guard tests.
    virtual unsigned long long textureLoadRequests() const = 0;

    /// How many threads the multiload pool has (0 = the feature is off and
    /// loading is the single background streaming thread). Set once at engine
    /// init from the machine's core count, overridable with JAH_TEXTURE_MULTILOAD
    /// for measurement — see the A/B protocol in THREADING_ADOPTION_SPEC G2-c.
    virtual unsigned textureMultiLoadThreads() const = 0;

    // ---- The texture wait's watchdog (defect 2026-09-08) ------------------
    //
    // THE CONTRACT OF THE WAIT, stated where hosts can read it: waiting for
    // texture streaming CANNOT block the caller indefinitely. The backend's own
    // `waitForStreamingCompletion` can and did — a load request that no worker
    // will ever complete parked a UI thread for twelve minutes — so both wait
    // paths here (`waitForTextureLoads` and the one at the head of
    // `renderOneFrame`) run a bounded drain instead: they keep draining while
    // the pending set SHRINKS, and give up when it has not moved for
    // `textureWaitBudgetMs`. Giving up is loud (the pending textures are logged
    // by name) and it is remembered — a host or a suite can assert on it.
    //
    // A HEALTHY RUN NEVER TRIPS THIS. The budget is a no-progress budget, so a
    // slow disk or a hundred-texture scene extends it indefinitely; only a
    // stalled queue expires it.

    /// How many times a bounded wait gave up. MUST be 0 in a healthy process:
    /// any non-zero value means textures were left unfinished and at least one
    /// frame was drawn without them.
    virtual unsigned textureWaitTimeouts() const = 0;

    /// HOW MANY TIMES THE DRAIN ADVANCED THE RENDERER'S RESOURCE BOOKKEEPING
    /// (lane ENGINE-SMALL-A / DRAIN-1, audit ON-17). Monotonic, never reset.
    ///
    /// The drain polls the texture manager every millisecond and, on a cadence
    /// of one frame's worth of time, calls `VaoManager::_update()` — which
    /// retires staging buffers, semaphores and delayed blocks, and whose
    /// COMMIT is a pair: a bare `_update` outside a frame commits at the top of
    /// the call and only when the previous one left the fence unflushed (the
    /// pin's issue #433), so the first advance of a drain ARMS and the second —
    /// one cadence later — commits and advances the frame index. It used to be
    /// called on EVERY poll, which is an empty command buffer plus a fence per
    /// millisecond of waiting (up to ~1,000/s while a scene loads). Only
    /// differences mean anything; it is here so a suite can assert the cadence
    /// rather than trusting it.
    virtual unsigned long long textureWaitAdvances() const = 0;

    /// The longest single bounded wait this process has performed, in ms.
    /// A timing observation, not a budget — useful for a suite that wants to
    /// say "the import path never blocked the UI for more than N ms".
    virtual double textureWaitWorstMs() const = 0;

    /// The resolved no-progress budget (ms). Set once at engine init from
    /// JAH_TEXTURE_WAIT_MS, default 8000. 0 means the wait is disabled
    /// entirely, which is a measurement mode and not a supported one.
    virtual unsigned textureWaitBudgetMs() const = 0;

    /// Rows in the backend's texture METADATA cache (P2 item 6): resolution,
    /// format, mipmaps and pool per texture path, remembered across launches so
    /// the main thread can reserve the right pool slice before the worker has
    /// decoded anything. Derived data with the same delete-and-rebuild contract
    /// as the shader cache, in the same directory. Not free to ask — the backend
    /// exposes no size() and this exports the map to count it.
    virtual unsigned textureMetadataCacheEntries() const = 0;

    /// Rows in OUR channel sidecar (P2 item 7, decision D-D(b)): path ->
    /// {numComponents, compressed}, remembered so that asking "is this file
    /// single-channel?" — which used to mean fully decoding every image on the
    /// calling thread and throwing the result away — costs a map lookup. Free to
    /// ask: it is a container size.
    virtual unsigned textureChannelCacheEntries() const = 0;

    /// Writes the texture cache (metadata + the channel sidecar) now. Called on
    /// clean shutdown beside saveShaderCache(); a no-op when the cache is off.
    virtual bool saveTextureCache() = 0;

    // ---- Presentation pacing (fps audit F1) --------------------------------
    /// Vertical sync for every ON-SCREEN View, now and for every window created
    /// afterwards (a window rebuilt by an MSAA change or a resize keeps it).
    /// Offscreen Views never present and are unaffected.
    ///
    /// ON (the default, EngineConfig::vsync) the backend presents in a
    /// vsync-respecting mode and the swapchain acquire BLOCKS until the display
    /// releases an image — which, combined with a host loop that ticks on a
    /// timer, is why the frame rate steps to refresh/n rather than sliding.
    /// OFF asks for an immediate (tearing) present mode: frames go out as fast
    /// as the loop produces them, which is what "unlimited" means and the only
    /// honest way to see what the renderer can actually do.
    ///
    /// NOT FREE TO TOGGLE: each on-screen View's swapchain is destroyed and
    /// rebuilt, exactly as a resize does. Call it on a user's change of mind,
    /// never per frame. A no-op when the value is unchanged.
    virtual void setVsync(bool) = 0;
    virtual bool vsync() const = 0;

    // ---- The hardware ray-query tier (PHOTON_SPEC §7 R1) -------------------
    /// THE NO-RAYS SWITCH at runtime. True lets the tier run wherever the
    /// device advertises VK_KHR_ray_query; false tears its structures down and
    /// renders the picture a machine WITHOUT ray tracing renders — which is the
    /// point: the fallback is not a second authoring path, it is the same
    /// scene with one term computed differently, and every ray-consuming suite
    /// runs both on this GPU.
    ///
    /// A no-op where the device has no rays (there is nothing to switch off).
    /// Boots from EngineConfig::rayTracing.
    virtual void setRayTracing(bool) = 0;
    virtual bool rayTracing() const = 0;
    /// The DEVICE's answer, once and for all: were the extensions and features
    /// enabled at vkCreateDevice? False on macOS, on a pre-RTX GPU, on
    /// lavapipe builds without ray query, and in a headless (NULL render
    /// system) engine.
    ///
    /// ASK IT AFTER THE FIRST VIEW EXISTS. Ogre creates the VkDevice with the
    /// first render target, not with Root — the same startup-order law that
    /// makes a render window a prerequisite for registerHlms() and
    /// createSceneManager() — so between Engine::create() and the first
    /// createView()/createOffscreenView() there is no device to ask and this
    /// reads false on hardware that has rays.
    virtual bool rayQueryAvailable() const = 0;

    // ---- Simulation clock (PARTICLES_FX2_SPEC.md; ENGINEERING_DEBT_SPEC A4.2) ----
    // The engine advances its own simulations inside renderOneFrame — the
    // particle systems and the shader `time` auto-params — by ONE frame delta
    // it never measures itself: THE HOST'S CLOCK IS THE ONLY CLOCK. There is
    // no wall-clock mode. The value is PROCESS-WIDE, not per scene and not per
    // view (the backend has exactly one frame-time source), so "freeze the
    // editor's particles while the player window runs" is not expressible;
    // the document's SimulationClock (iris::Scene::advance) decides how many
    // seconds a frame simulated and the host pushes that product here, every
    // frame, scaled by the scene's particle time scale.
    /// The seconds EVERY renderOneFrame advances the engine-side simulation
    /// by, until the next call. 0 freezes it (a paused scene, a frame that
    /// bought no clock step). Boots at kDefaultFrameDelta so a host that never
    /// pushes (thumbnails, previews, engine-only suites) still simulates one
    /// deterministic grid step per frame instead of the milliseconds an
    /// offscreen frame takes on the wall clock.
    virtual void setFixedFrameDelta(float seconds) = 0;
    virtual float fixedFrameDelta() const = 0;
    /// 1/60 — the document's SimulationClock grid (simulationclock.h).
    static constexpr float kDefaultFrameDelta = 1.0f / 60.0f;

    /// Shadow filter quality for EVERY shadowed light in EVERY scene — the
    /// backend's PBR pipeline has one global filter, not a per-light one
    /// (Hard = PCF 2x2, Soft = PCF 4x4, VerySoft = PCF 6x6). Callers with
    /// per-light document settings push the strongest requested quality.
    /// Cheap: takes effect next frame, no material rebuild. Default: Soft.
    virtual void setShadowFilter(ShadowFilter) = 0;
    virtual ShadowFilter shadowFilter() const = 0;

    /// Shadow-map resolution for EVERY shadowed light in EVERY scene — global,
    /// like the filter: the backend renders all shadow maps into one fixed atlas
    /// (PSSM splits + two focused maps) whose sizes derive from this base value
    /// (split 0 and the focused maps at `pixels`, further splits at half).
    /// Callers with per-light document settings push the LARGEST requested size.
    /// NOT cheap: changing it tears down and rebuilds the shadow node and every
    /// workspace that references it — call on change only, never per frame.
    /// Clamped to [256, 8192]. Default: 2048.
    virtual void setShadowResolution(unsigned pixels) = 0;
    virtual unsigned shadowResolution() const = 0;

    /// HOW MANY POINT/SPOT LIGHTS MAY HAVE A SHADOW MAP AT ONCE
    /// (SPECS/SHADOW_TOOLING_SPEC.md §4.1). The atlas holds one PSSM block for
    /// the closest directional light plus N focused maps, and Ogre fills those
    /// N slots with the casters closest to the camera and silently drops the
    /// rest — so before this existed, a scene with three shadow-casting lamps
    /// had one lamp with no shadow, and WHICH lamp changed as the camera moved.
    ///
    /// This is a CEILING, not an allocation: the engine derives the count from
    /// the scenes it draws, steps it {2, 4, 8, 16} and only grows (VRAM is
    /// returned when the process ends, and a rebuild costs every workspace that
    /// names the shadow node). Empty maps cost no shader permutation — Ogre
    /// counts ACTIVE casters — so head-room is free until a light fills it.
    ///
    /// Clamped to [2, 16], and clamped again by the resolution: 16 maps at a
    /// 4096 base do not fit inside the 16384 texture limit. `shadowStatus()`
    /// reports what survived both clamps. Default 8.
    virtual void setShadowMapBudget(unsigned maps) = 0;
    virtual unsigned shadowMapBudget() const = 0;

    /// What the shadow atlas ACTUALLY is: its layout, which light holds which
    /// map, and which shadow-casting lights got none. Cheap (reads live
    /// pointers, renders nothing); `live == false` on a headless engine.
    /// ASKING ARMS THE PASS COUNTERS for the frames that follow; they come off
    /// the render path again after ~120 frames without a call.
    virtual ShadowStatus shadowStatus() const = 0;

    /// Re-render EVERY cached point/spot shadow map in every scene, once, on
    /// the next frame — the "I do not know what changed" button, and the shadow
    /// twin of refreshGi(). The engine re-renders a lamp's map by itself when
    /// the lamp moves or changes reach, and when a caster inside its reach
    /// moves, appears, disappears or changes shape (ENGINE_CACHE_POLICY_SPEC
    /// P3); this is for what it cannot see. Returns false when there is
    /// nothing to refresh.
    virtual bool refreshShadows() = 0;

    /// Shadow-caster geometry optimization — see EngineConfig::optimizeShadowMeshes.
    /// PROCESS-WIDE and consumed when a mesh is BUILT: changing it re-decides the
    /// question for meshes created afterwards and leaves existing ones alone.
    /// That is why it is an application preference, not a per-scene setting.
    virtual void setShadowMeshOptimization(bool on) = 0;
    virtual bool shadowMeshOptimization() const = 0;

    // ---- Persistent shader cache (SHADER_CACHE_SPEC.md) ----
    // Three layers behind one fingerprinted container: the Vulkan pipeline
    // cache (driver ISA), the microcode cache (SPIR-V), and the Hlms disk cache
    // (preprocessed shader source). All of it is DERIVED DATA: on any doubt the
    // backend deletes the directory and starts cold rather than feed a
    // half-written blob to a driver.
    //
    // The cache is loaded once, inside the first createView() — nothing here
    // needs calling to make it work. These verbs exist so the application can
    // SHOW what it did and let a user throw it away.

    /// What is on disk and what happened this run. Cheap enough to call from a
    /// settings page; it stats a handful of files.
    virtual ShaderCacheStats shaderCacheStats() const = 0;

    // ---- The engine's own log, forwarded (SESSION_LOG_SPEC fork F3-B) ----
    //
    // Ogre writes thousands of LML_NORMAL lines per boot into its OWN
    // per-session file, and folding all of that into the application's session
    // log would destroy exactly the signal-to-noise the log exists for. What
    // the application DOES want is the criticals — the ~55 in OgreMain and the
    // 15 in the Vulkan render system, which include every Vulkan validation
    // error — where a human will actually see them.
    //
    // THREE RULES the sink must obey, all of them properties of Ogre's Log:
    //   1. It is called UNDER Ogre's log mutex, from whatever thread logged
    //      (including the background streaming and texture threads). It must be
    //      thread-safe.
    //   2. It must NEVER call back into Ogre. That is a deadlock, not a risk.
    //   3. It must not block: a slow sink slows every log line in the process.
    //
    // `level` is 0 for ordinary messages and 1 for LML_CRITICAL. Ogre's own
    // file and console output are untouched — the listener never sets
    // skipThisMessage.
    using LogSink = EngineLogSink;
    virtual void setLogSink(LogSink sink) = 0;

    /// Which GPU, which driver, which API version — filled once the render
    /// system has a device. Empty strings before that, and under the NULL
    /// render system everything but `renderSystem` is legitimately empty.
    virtual DeviceInfo deviceInfo() const = 0;

    /// What the renderer measured (STATS_OVERLAY_SPEC.md §4). Cheap: it reads
    /// counters the backend already keeps, and copies no buffers.
    ///
    /// LAZY BY DESIGN. Geometry counting (draws/batches/triangles) is OFF in
    /// the backend by default and costs integer adds per draw call, so the
    /// FIRST call to this switches it on and reports metricsRecording=false
    /// with zeroed counters. Every call after a rendered frame reports real
    /// numbers. Nothing that never asks for stats ever pays for them.
    ///
    /// Returns false only when there is no backend to ask; `out` is then left
    /// default-constructed.
    virtual bool renderStats(RenderStats &out) const = 0;

    /// What the renderer is HOLDING (see ObjectCounts). The companion to
    /// renderStats: that one answers "what did the frame cost", this one
    /// answers "what is alive", which is the question a slow leak makes
    /// people ask an hour too late.
    ///
    /// Not lazy and not measured — every field is a container size() plus one
    /// walk of the (tiny) view and scene vectors, so it is safe to call every
    /// frame and costs nothing when nobody does.
    ///
    /// Returns false only when there is no backend to ask; `out` is then left
    /// default-constructed.
    virtual bool objectCounts(ObjectCounts &out) const = 0;

    /// WHAT THE ENGINE IS THREADING (see EngineThreading;
    /// SPECS/THREADING_ADOPTION_SPEC.md P1). Reads the backend's own capability
    /// answer and the live per-scene worker counts — no measurement, no state.
    ///
    /// The reason this is a verb rather than a build-time constant: the
    /// multithreaded-shader-compilation flag lives in the ENGINE INSTALL, not
    /// in Studio, so the only honest way to know whether this binary is talking
    /// to a mode-2 engine is to ask the render system at run time. A tree that
    /// forgot to re-run `irisgl/scripts/build-ogre.sh` reports false here and
    /// nowhere else.
    ///
    /// Returns false only when there is no backend to ask.
    virtual bool threading(EngineThreading &out) const = 0;

    /// The renderer's memory pools right now (MemoryStats says what each row
    /// is and who owns it). Cheap: reads the pool tables, renders nothing.
    virtual bool memoryStats(MemoryStats &out) const = 0;
    /// Every texture the renderer's texture manager knows, one entry each,
    /// UNSORTED (the host orders). The attribution behind
    /// MemoryStats::gpuPoolCapacityBytes on Vulkan, where textures share the
    /// pools: a default scene boots at ~3.2 GB of pool capacity and this is
    /// how to see what (shadow atlases, GI volumes, the samples' images).
    /// Cheap: walks the entry table, renders nothing. False only without a
    /// backend.
    virtual bool textureMemory(std::vector<TextureMemoryEntry> &out) const = 0;
    /// RECLAIM (riders lane R4): shrinks every scene manager's SIMD pools to
    /// what is live (SceneManager::shrinkToFitMemoryPools — the pools never
    /// shrink by themselves, they hold the high-water mark of nodes ever
    /// alive). The right moments are after a large REMOVAL — a project close,
    /// an undone import — never after growth, which it cannot help. Safe
    /// between frames; slots are relocated through Ogre's rebase listener, and
    /// no host holds a slot index. The GPU pools need no call: the Vulkan
    /// VaoManager frees a pool that emptied by itself (see MemoryStats).
    /// `before`/`after` are filled when given, so a caller can log the delta.
    virtual bool reclaimMemory(MemoryStats *before = nullptr, MemoryStats *after = nullptr) = 0;
    // ---- THE RENDER-LOOP MONITOR (SPECS/RENDER_LOOP_MONITOR_SPEC.md) ----
    //
    // A DATA COLLECTOR for the lead's engine reviews, and nothing else: it
    // records what each frame did and WHY, judges none of it, and never draws.
    // It replaced an opt-in pass profiler that is GONE from this boundary
    // (`EngineConfig::profile`, `setProfiling`/`profiling` and the Studio
    // `app.profiling` verb were all deleted with it): it measured one number
    // per profiling id, mis-nested every scene pass that owned a shadow node
    // (a single start time, overwritten by each nested pass) and logged every
    // C++-built shadow pass as "(unnamed pass)".
    //
    // OFF BY DEFAULT AND FREE WHEN OFF. At `MonitorLevel::Off` nothing is
    // attached to any workspace, no clock is read, the ring is freed and no GPU
    // query pool exists. `monitorStatus()` reports each of those four so a test
    // can assert it rather than trust it.
    //
    // FORWARD ONLY. Turning the monitor on starts recording from that frame;
    // there is no background history, by the owner's decision.

    /// Switch the monitor on (Review) or off. Attaches/detaches every listener
    /// on every live workspace — the view's, each planar mirror's and each
    /// reflection probe's — and allocates/frees the frame ring. Idempotent.
    /// Listeners ride the workspace seams, so they survive an atlas rebuild, a
    /// GI rebuild and a workspace recreation.
    virtual void setFrameMonitor(MonitorLevel level) = 0;
    virtual MonitorLevel frameMonitor() const = 0;
    /// What the monitor is doing, including the four zero-cost assertions and
    /// the state of both GPU-timing off-switches.
    virtual MonitorStatus monitorStatus() const = 0;

    /// Drains the frame ring into `out` (appending) and returns how many
    /// records were moved. The host drains on its own tick; anything it does
    /// not drain is overwritten, counted by `MonitorStatus::framesDropped`.
    ///
    /// TWO THINGS TO KNOW, both consequences of GPU timing:
    ///  * A frame is not published the instant it ends. GPU samples come back
    ///    two frames late, so a record waits a few frames for them; drain in a
    ///    loop, never "render one frame, expect one record".
    ///  * STOPPING the monitor flushes everything still waiting, and the NEXT
    ///    call to this returns it even though the monitor is off. That makes
    ///    the natural host order — stop the capture, then drain — lossless. The
    ///    call after that returns 0 and the storage is freed.
    virtual unsigned takeFrameRecords(std::vector<FrameRecord> &out) = 0;
    /// The same for discrete events (GI rebuilds, atlas changes, compiles,
    /// texture loads, VRAM flushes, device-lost, and the host's own).
    virtual unsigned takeMonitorEvents(std::vector<MonitorEvent> &out) = 0;
    /// THE HOST'S HOOK. Page switches, UI-thread gaps, script marks, toast
    /// lifetimes — anything the host knows and the engine cannot. Ignored when
    /// the monitor is off, so a caller never has to check first. `frame` and
    /// `startMs` are filled in by the engine when left at 0.
    virtual void noteMonitorEvent(const MonitorEvent &event) = 0;
    /// A host stage (tick, mirror sub-stages, UI gap) folded into the NEXT
    /// frame record's stage list, so `frames.jsonl` carries one stage tree per
    /// frame. Ignored when the monitor is off.
    virtual void noteHostStage(const std::string &name, float ms) = 0;
    /// Why the next frame is being rendered — the driver's tick, a script's
    /// `editor.frame`, an offscreen readback, the warm-up gate. Consumed by the
    /// next `renderOneFrame` and reset to `Driver`; an offscreen scope sets it
    /// so analysis can tell a frame nobody saw from one the owner watched.
    virtual void setNextFrameCause(FrameCause cause) = 0;

    // ---- streaming a world in (SPECS/OPEN_COVER_SPEC.md §2.1) -------------
    /// What the NEXT frame may put off — see `FramePace`. Consumed by the next
    /// `renderOneFrame` and reset to `Complete`, so a host that sets nothing
    /// renders complete frames exactly as it always did. Monitor-independent:
    /// this changes what the frame DOES, not what it records.
    virtual void setNextFramePace(FramePace pace) = 0;
    /// Does any scene feeding an enabled view still owe first-time work that a
    /// `Streaming` frame would take a step of? The host reads it to decide how
    /// long to keep asking (and, in phase 2b, to draw the indicator).
    virtual bool framePaceOwesWork() const = 0;

    /// THE ENGINE, AT ONE INSTANT (§4.8) — every GI parameter and what it
    /// resolved to, the probe grid, the shadow setup with per-light cache
    /// state, the light list, the object/VRAM/Hlms census, the texture
    /// streaming queue and THE COMPOSITOR GRAPH (every live workspace, its
    /// nodes and passes, and which scene each renders).
    ///
    /// `scene` null = the scene of the first enabled on-screen view. Works with
    /// the monitor off (it renders nothing and allocates nothing persistent) —
    /// the host takes one at the start of a capture and one at the end.
    virtual bool captureSnapshot(EngineSnapshot &out, const std::string &label,
                                 Scene *scene = nullptr) const = 0;

    // ---- PHOTON SHARED INFRASTRUCTURE (SPECS/NANITE_SPEC.md §4.2-§4.3) ----

    /// Runs the indirect-dispatch chain once and reports what the GPU did
    /// (IndirectDispatchProbe says what each field means). `survivors` is how
    /// many entries of a 4096-long input list are non-zero, i.e. how many thread
    /// groups the second job must end up running.
    ///
    /// This is the PROOF of ogre-patch 0032 and, for now, its only caller: the
    /// capability exists for the Photon arms, which are not built yet. It
    /// allocates three small UAV buffers, dispatches twice, reads back and frees
    /// everything again, so it is safe to call at any time — but it is a
    /// measurement, not a render path.
    ///
    /// False means the jobs are missing (unstaged media) or the backend cannot
    /// do it; `out.supported` distinguishes the two.
    virtual bool indirectDispatchProbe(unsigned survivors, IndirectDispatchProbe &out) = 0;

    /// What pyramid `view` is building, if any (HzbStatus). Cheap: reads the
    /// live texture's shape, renders nothing. False when the view has none.
    virtual bool hzbStatus(View *view, HzbStatus &out) const = 0;

    /// Reads one mip level of `view`'s pyramid back to the CPU, row-major, one
    /// float per texel (the raw depth value in the engine's own convention —
    /// see HzbStatus::reverseDepth). A MEASUREMENT: it flushes the command
    /// buffer and stalls on the copy, so it belongs in a suite or a spike, never
    /// in a frame. False when there is no pyramid or no such level.
    virtual bool readHzbLevel(View *view, unsigned level, std::vector<float> &out,
                              unsigned &width, unsigned &height) = 0;

    /// Serializes the cache now and writes it OFF THE CALLING THREAD. Called on
    /// clean shutdown and once a compile burst has settled; safe (and a no-op)
    /// when the cache is disabled or nothing is dirty.
    ///
    /// WHAT "NOW" MEANS (FSYNC-1). Serializing is the engine's half and happens
    /// before this returns — it reads Ogre's caches, so it can happen nowhere
    /// else. The FILE half (about a megabyte, an `fsync` and an atomic rename)
    /// is handed to the engine's writer thread, because that fsync waits behind
    /// every other dirty page the filesystem is holding: 17 ms on an idle disk,
    /// 403 ms measured with a stream of writeback in front of it — on the UI
    /// thread, in the middle of an archive the user was watching.
    ///
    /// True therefore means "serialized and handed over", not "on the disk".
    /// flushShaderCache() is how a caller that needs the second thing waits for
    /// it; the engine's own destructor waits too, so a clean quit never loses a
    /// save. False means nothing was handed over (disabled, nothing dirty, or
    /// the serialization failed) — the previous cache, if any, is untouched.
    virtual bool saveShaderCache() = 0;
    /// Waits up to `budgetMs` for the write saveShaderCache() handed off. True
    /// when the writer is idle (nothing in flight, or it finished); false on
    /// timeout, with the write still running. A no-op when nothing is in
    /// flight.
    virtual bool flushShaderCache(unsigned budgetMs) = 0;
    /// Deletes every cached file. The next launch is cold. Always safe: the
    /// running process keeps its in-memory shaders.
    virtual bool clearShaderCache() = 0;
    // ---- Recorded warm-up sets (SHADER_CACHE_SPEC.md §2.7b / phase 3) ----
    // Unreal's ".rec" recordings, our shape. A warm-up SET is not shaders and
    // not SPIR-V — it is the list of {vertex format, render queue, one
    // representative material per distinct shader} a scene actually used. That
    // makes it small, and unlike the microcode and pipeline blobs it is
    // platform- and driver-independent, so it is the only one of these
    // artifacts that could ever be shipped.
    //
    // The point of the indirection: applying a set compiles every permutation
    // in it against DEGENERATE 4-vertex buffers, so nothing is loaded from disk
    // and nothing reaches VRAM. A recorded session's shaders can be rebuilt
    // without its meshes, its skeletons or its textures.

    /// Adds everything `scene` currently draws to this process's warm-up set,
    /// or EVERY LIVE SCENE when `scene` is null. ACCUMULATES — call it for
    /// every scene a session touches and the set is their union, which is what
    /// makes "merging recordings" a no-op rather than a tool. Duplicate
    /// permutations are folded by the engine.
    virtual bool recordWarmUpSet(Scene * = nullptr) = 0;
    /// Writes the accumulated set. False if nothing has been recorded or the
    /// file cannot be written.
    virtual bool saveWarmUpSet(const std::string &file) = 0;
    /// Loads a set and compiles every permutation in it, using `scene` as the
    /// host for the degenerate renderables it creates (null = the first live
    /// scene; they exist for one frame and are destroyed again, so any will
    /// do). Returns how many shaders were built — 0 is a legitimate answer on a
    /// warm cache. The scene is left exactly as it was found.
    virtual unsigned applyWarmUpSet(const std::string &file, Scene * = nullptr) = 0;

    /// The startup progress counter's source: shaders compiled so far, shaders
    /// served from the cache so far, and how many the last saved run needed in
    /// total (0 = never saved, so no denominator exists yet). Two atomic reads;
    /// no disk, safe to poll on a timer.
    virtual void shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                                     unsigned &expected) const = 0;

    /// Reason for the most recent failure; empty if none.
    ///
    /// NOTE this is a PEEK: the sink is never cleared on success, so a stale
    /// reason outlives the call that set it. Callers that check a specific
    /// verb's return value read this immediately and are fine; anything that
    /// POLLS must use takeLastError() instead, or it cannot tell a fresh
    /// failure from one that happened at startup.
    virtual const std::string &lastError() const = 0;

    /// Reason for the most recent failure, AND clears the sink — so the next
    /// call reports only what has failed since. Empty when nothing has.
    ///
    /// This exists because the backend swallows failures by design: every
    /// backend virtual is wrapped in a try/catch that records the reason here
    /// and returns a refusal value, and most callers (SceneMirror above all)
    /// ignore that value. Nothing ever read the sink, so ~86 catch sites had no
    /// reader at all — a mesh with no tangents, a texture that will not decode,
    /// a full decal atlas all produced a wrong picture and ZERO log lines.
    /// Draining this once a frame is what turns them back into diagnostics
    /// (src/services/engineerrorpump.h).
    ///
    /// There is ONE sink per process: every Scene and View holds a reference to
    /// the Engine's string, so this drains all of them.
    virtual std::string takeLastError() = 0;
};

}}  // namespace jahshaka::engine
