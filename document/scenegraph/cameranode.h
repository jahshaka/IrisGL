/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef CAMERANODE_H
#define CAMERANODE_H


#include "core/math/mat4.h"
#include "core/math/vec.h"
#include "irisglfwd.h"
#include "document/scenegraph/cameralens.h"
#include "document/scenegraph/scenenode.h"

#include <QJsonArray>
#include <QJsonObject>


namespace iris
{

enum class CameraProjection {
	Orthogonal,
	Perspective
};

/// Which of the two views of the SAME angle of view the user authored
/// (CAMERAS_SPEC §2, Blender's model). The document stores the angle; the focal
/// length is derived from it through the sensor. The flag decides which of the
/// pair survives a sensor-size change — nothing else.
enum class CameraAuthorMode {
	Degrees,
	Millimeters
};

/// How the focus distance is decided (CAMERAS_SPEC §2, D7). `Manual` uses
/// focusDistance as written; `Track` derives it from focusTarget's world
/// position every frame; `Off` means no focus is computed at all (DOF may still
/// be flagged on and simply does nothing).
enum class CameraFocusMode {
	Manual,
	Track,
	Off
};

/// How this camera's EXPOSURE is decided (CAMERA_LENS_SPEC §4).
///
/// `Inherit` — the default, and the only value that changes nothing: the world's
/// own exposure settings reach the view untouched, exactly as they did before
/// per-camera exposure existed. `Auto` and `Manual` both SUBSTITUTE this
/// camera's block for the world's while this camera is the one driving a view
/// (piloted, played through, or the subject of an opted-in screenshot);
/// `Manual` additionally pins the adaptation clamp so the grade is a number and
/// not a measurement (iris::lens::manualExposureClamp explains the pin).
///
/// NEVER on a thumbnail, a preview or a pixel suite: those render through
/// OFFSCREEN views, which discard the whole post description unless a caller
/// deliberately opts in (PostFxDesc::allowOffscreen). The determinism law is
/// enforced in the engine, in one place, so nothing here has to remember it.
enum class CameraExposureMode {
	Inherit,
	Auto,
	Manual
};

/// The per-camera post-process override keys, and what kind of value each one
/// holds (CAMERA_LENS_SPEC §5). THE DOCUMENT IS THE AUTHORITY on which keys
/// exist, because the document is what stores them; Studio's row tables
/// (services/worldmodes.h) are the panel/verb view of the same set and a test
/// asserts the two agree.
///
/// The three exposure ids the world's parameter table also carries — exposure,
/// exposureMin, exposureMax — are DELIBERATELY ABSENT: a camera's exposure is
/// the §4 block above (stops, with a mode), not an override slot, and having
/// both would be two dials for one value.
/// Toggle = a bool stored as 0/1; Enum = an int from a fixed set; Number = a
/// double; Stack = a WHOLE JSON ARRAY (POST_LOOKS_SPEC.md §4.1 / D4 — the looks
/// stack is the only one, and it is a replacement rather than a value because
/// an override over an ORDERED LIST is only well defined as one: a camera that
/// overrode "radialBlur's centre" while the world removed Radial Blur would
/// mean nothing).
enum class CameraPostKeyType { Toggle, Enum, Number, Stack };

struct CameraPostKey {
	const char *id;
	CameraPostKeyType type;
};

/// The table, and its size. Stable order: the panel and the verbs report it
/// as-is.
const CameraPostKey *cameraPostKeys(int &count);
/// The entry with this id, or null.
const CameraPostKey *cameraPostKey(const QString &id);

class CameraNode : public SceneNode
{
public:
    float aspectRatio;
    /// VERTICAL field of view, in DEGREES, end to end (CAMERAS_SPEC §1:
    /// angle -> CameraDesc.fovDegrees -> Ogre setFOVy). Unreal's basic camera
    /// FOV is horizontal; converting an Unreal value means going through the
    /// aspect ratio first.
    ///
    /// This is THE stored angle of view. `focalLength()` is a second view of
    /// it, not a second field, so a direct write here (the reader, the
    /// controllers, the preview framing) can never leave the two disagreeing.
    float angle;

    /// THE WIDE-ASPECT FRAMING HOLD — TRANSIENT, and every word of that
    /// matters (owner report 2026-09-07, the picking half of the defect;
    /// re-scoped from a fixed 95-degree horizontal cap to an ASPECT on
    /// 2026-09-08, see iris::lens::verticalFovDegForFramingAspect).
    ///
    /// A HOST'S STATEMENT ABOUT A CAMERA IT OWNS, not a property of the shot:
    /// "this one is a free explorer, do not let a 32:9 window fisheye it".
    /// Zero — the default, and what EVERY authored camera keeps forever — is
    /// off. Only the app's two FREE cameras (the editor explorer, which the
    /// player flies too) ever carry a non-zero value, set by the viewport that
    /// owns them; see src/viewport/freecamerapolicy.h for the policy and the
    /// number (16:9).
    ///
    /// AT OR BELOW THIS ASPECT IT DOES NOTHING AT ALL — the stored `angle` is
    /// the angle rendered and picked through, bit for bit. Above it the
    /// vertical angle narrows to hold the horizontal extent the shot has at
    /// this aspect.
    ///
    /// It lives on the camera and NOT in the picker's arguments because the
    /// projection is what has to change: `updateCameraMatrices` narrows the
    /// vertical angle exactly as the engine does when it draws
    /// (iris::lens::verticalFovDegForFramingAspect), so the document's
    /// projMatrix IS the frustum on screen and every consumer of it — the four
    /// ScenePicker::screenSegment call sites (object pick, gizmo drag, drop,
    /// vertex snap), the gizmo's screen-constant sizing and the player's mouse
    /// controller — agrees with the image without knowing the hold exists.
    /// Before this, a click on a wide window unprojected through a WIDER
    /// frustum than the one drawn and selected whatever sat behind the thing
    /// the user aimed at.
    ///
    /// NEVER SERIALIZED, NEVER KEYFRAMED, NEVER DUPLICATED: it is not part of
    /// the document (scenewriter/scenereader do not mention it, it is absent
    /// from getProperties, and createDuplicate leaves the copy at 0). A saved
    /// scene reopened in a host that does not set it behaves exactly as it did
    /// before this field existed.
    float framingAspectRatio;

    float nearClip;
    float farClip;
    float vrViewScale;
	float orthoSize;
	bool isPerspective;

	CameraProjection projMode;

    // ---- CAMERAS_SPEC §2: the physical-camera settings -------------------
    // All of them are reflected through getProperties/get/setPropertyValue, so
    // they are keyable by PropertyAnim and reachable from scripts with no new
    // machinery. None of them change what the ENGINE renders in phase 1 except
    // `angle` (which always did) — the body, the frustum, the letterbox and the
    // DOF pass are later phases; the document is the deliverable here.

    /// Sensor size in millimetres. 36 x 24 = full frame — OURS, not a
    /// reproduction of any engine's published default (Epic publishes none).
    /// The angle of view is VERTICAL; WHICH sensor dimension it binds through is
    /// `sensorFit` below (CAMERA_LENS_SPEC §3 — the phase that finally made
    /// `sensorWidth` do something; it was stored and inert before).
    float sensorWidth;
    float sensorHeight;

    // ---- CAMERA_LENS_SPEC §3: the filmback block -------------------------

    /// Which sensor dimension the focal length binds through (see cameralens.h
    /// for the whole model). VERTICAL is the default because it is what this
    /// class always did: no existing scene's projection moves because these
    /// fields arrived.
    CameraSensorFit sensorFit;

    /// Anamorphic squeeze factor: the effective sensor WIDTH is
    /// `sensorWidth * anamorphicSqueeze`. 1.0 (spherical) is the default and is
    /// inert; a 2x anamorphic sees as wide as a half-length spherical lens.
    /// Only participates when the binding axis is horizontal.
    float anamorphicSqueeze;

    /// LENS SHIFT (rise/fall and cross), as a FRACTION OF THE FRAME: 0.5 slides
    /// the image half a frame width / height without tilting the camera — the
    /// architectural-photography move that keeps verticals parallel. The
    /// conversion into a projection offset is re-derived from fov/aspect/near
    /// wherever it is applied (never stored), because it depends on all three.
    float lensShiftX;
    float lensShiftY;

    /// Which view of the angle the user authored. Only consulted when the
    /// SENSOR changes: in Degrees the angle is kept and the focal length
    /// follows, in Millimeters the focal length is kept and the angle follows.
    CameraAuthorMode authorMode;

    /// Letterbox the view to `aspectRatio` instead of filling it. Stored and
    /// serialized in phase 1; the bars are drawn in phase 3.
    bool constrainAspect;

    // Focus / depth of field. Stored, animated and exported in v1; the live
    // DOF pass is the program's second act (D7).
    bool dofEnabled;
    CameraFocusMode focusMode;
    float focusDistance;      // metres, used when focusMode == Manual
    QString focusTarget;      // node guid, used when focusMode == Track
    float fStop;

    // ---- CAMERA_LENS_SPEC §3 P2: the focus block -------------------------
    //
    // RENDER-INERT, all of it: no pass reads these (the DoF pass is deferred by
    // the owner). What they DO drive is the tracking arithmetic, the focus
    // numbers `camera.focusInfo` reports, and the debug plane — i.e. everything
    // needed to author a focus pull correctly before anything renders it.

    /// Added to the tracked distance in Track mode (metres, may be negative):
    /// "focus a little in front of his eyes" is a real instruction.
    float focusOffset;
    /// Ease the tracked distance instead of snapping it (a focus puller's hand,
    /// not a servo). Off by default so tracking stays exactly predictable.
    bool smoothFocus;
    /// How fast the eased distance converges, in e-folds per second
    /// (lens::smoothTowards). 8 is roughly "settles in half a second".
    float focusSmoothingSpeed;
    /// The closest the lens can focus, in metres. Tracking clamps to it.
    float minFocusDistance;
    /// Diaphragm blades — the shape of the bokeh. Stored for the deferred DoF
    /// pass and for export; nothing samples it yet. Clamped to 3..16.
    int bladeCount;
    /// Draw the focus plane as a wire rectangle inside the frustum helper
    /// (an editor helper like the body: hidden in play/game view, never in a
    /// render). Off by default.
    bool focusPlaneVisible;

    /// The output height in pixels for RENDERS and EXPORTS; with aspectRatio it
    /// derives the whole output size. The main view and the PiP ignore it
    /// (CAMERAS_SPEC §2 — no engine puts resolution on the camera; this is a
    /// named Jahshaka concept, not a claim about anyone else's model).
    int outputHeight;

    /// Draw the editor-only camera body + frustum wires for this camera
    /// (phase 2). Like every editor helper it is hidden in play/game view.
    bool bodyVisible;

    // ---- CAMERA_LENS_SPEC §4: the exposure block -------------------------
    //
    // STOPS, all three of them, and the conversion into the post chain's own
    // natural-log axis happens ONCE, at the mirror, through
    // iris::lens::exposureStopsToChain. Nothing in the document ever holds a
    // chain-unit exposure: the world's `Scene::exposure` does (it is the value
    // the chain has always taken), the camera's does not, and mixing them up is
    // the one mistake this pair of units invites.

    /// Inherit (default) / Auto / Manual. Inherit is bit-for-bit "as if this
    /// block did not exist".
    CameraExposureMode exposureMode;
    /// The exposure, in STOPS. 0 is the default world grade; +1 is one
    /// doubling. In Manual mode it IS the exposure; in Auto it is the midpoint
    /// the adaptation works around.
    float exposure;
    /// The window automatic exposure may adapt within, in stops, on the same
    /// axis. Ignored in Manual mode (which pins the clamp to a fixed reference
    /// instead — see iris::lens::manualExposureClamp for why that is not the
    /// exposure value). Kept ordered by the writers: min <= max.
    float exposureMin;
    float exposureMax;

    // ---- CAMERA_LENS_SPEC §5: per-camera post overrides ------------------

    /// TRI-STATE, one key at a time: a key PRESENT here overrides the world's
    /// value while this camera drives a view; a key ABSENT inherits. There is
    /// no blend weight and there will not be one — our post is compositor
    /// SHAPE (a workspace rebuild), not a float anybody can cross-fade, and a
    /// weight that silently snapped at 0.5 would lie (§2, "REJECTED — would
    /// lie"). UE's own bOverride model is the same shape.
    ///
    /// Keys are `cameraPostKeys()` and nothing else; values are ints for the
    /// Toggle/Enum kinds, doubles for Number, and a JSON ARRAY for the one
    /// Stack key (`looks`). Written through setPostOverride, which refuses
    /// anything else — a QJsonObject read from a file is sanitised the same way
    /// by the reader.
    QJsonObject postOverrides;

    /// Is this key overridden, and what does it say? `postOverride` returns an
    /// invalid QVariant when the key is absent (i.e. inherited).
    bool hasPostOverride(const QString &id) const;
    /// Number/Toggle/Enum keys only. A Stack key (`looks`) has no scalar
    /// reading and returns an invalid QVariant here even when it is set — read
    /// it with postOverrideStack, whose type is the honest one.
    QVariant postOverride(const QString &id) const;
    /// The array a Stack key holds; empty when the key is absent (which is NOT
    /// the same as an override to the EMPTY stack — use hasPostOverride to tell
    /// "inherit the world's looks" from "this camera has none").
    QJsonArray postOverrideStack(const QString &id) const;
    /// Records an override. Returns false for an unknown key or a value the key
    /// cannot hold; a valid write always replaces whatever was there.
    bool setPostOverride(const QString &id, const QVariant &value);
    /// Drops an override (back to inherit). False when there was none.
    bool clearPostOverride(const QString &id);

    iris::Mat4 viewMatrix;
    iris::Mat4 projMatrix;

	void setProjection(CameraProjection view);
    CameraProjection getProjection();
    float getVrViewScale();
    void setVrViewScale(float viewScale);
    void setAspectRatio(float aspect);
    void setFieldOfViewRadians(float fov);
    void setFieldOfViewDegrees(float fov);

    /// Sets the transient wide-aspect framing hold (see `framingAspectRatio`)
    /// and re-derives the matrices, so the projection is correct before the
    /// next pick even if nothing else touches the camera this frame.
    /// Non-positive turns it off.
    void setFramingAspect(float aspect);
    float framingAspect() const { return framingAspectRatio; }

    /// The VERTICAL angle the PROJECTION uses at the camera's current aspect —
    /// `angle` narrowed by the framing hold on a window wider than it, and
    /// `angle` itself otherwise. THIS, never `angle`, is what anything sizing
    /// itself against the rendered frame must use: the gizmo's screen-constant
    /// scale reads it (src/viewport/gizmo.cpp), and reading `angle` there was
    /// the second half of the 2026-09-08 owner report — a gizmo 1.7x too big
    /// in the Grand Showroom. The AUTHORED angle is never touched: `angle`, `focalLength()`,
    /// `horizontalFov()` and the panel/verbs all keep reporting the lens the
    /// user chose, because the cap is a viewing constraint and not an edit.
    float effectiveFovDegrees() const;

    /// This camera's filmback as the lens math sees it — sensor pair, squeeze,
    /// fit and the authored aspect, in one struct (cameralens.h).
    iris::lens::Filmback filmback() const;

    /// The lens view of `angle`, in millimetres, through the filmback:
    ///
    ///     vertical fit:   angle = 2 * atan(sensorHeight / (2 * focalLength))
    ///     horizontal fit: hFov  = 2 * atan(sensorWidth * squeeze / (2 * f))
    ///                     angle = 2 * atan(tan(hFov / 2) / aspectRatio)
    ///
    /// Exact inverses of each other, and derived rather than stored, so the
    /// dozen places that assign `angle` directly cannot desynchronise the pair.
    float focalLength() const;
    /// Sets the angle of view from a focal length and flips authorMode to
    /// Millimeters. Values <= 0 are ignored (a zero-length lens has no angle).
    void setFocalLength(float mm);
    /// Changes the sensor and keeps whichever view of the angle was authored:
    /// in Degrees the angle survives, in Millimeters the focal length does.
    /// Non-positive dimensions are ignored.
    void setSensorSize(float widthMm, float heightMm);
    /// Same contract as setSensorSize for the other two filmback rows: the
    /// authored view of the angle survives, the derived one moves.
    void setSensorFit(CameraSensorFit fit);
    void setAnamorphicSqueeze(float squeeze);

    /// The horizontal / diagonal angles of view (degrees) for the authored
    /// aspect. Derived, never stored — reported by the verbs and the panel.
    float horizontalFov() const;
    float diagonalFov() const;

    /// Depth of field, in numbers (CAMERA_LENS_SPEC §3, P2): hyperfocal
    /// distance and the near/far limits at the CURRENT focus distance, for this
    /// lens, f-stop and sensor. Render-inert — this is what makes the stored
    /// aperture honest before any DoF pass exists. `focusMode == Off` still
    /// reports, using focusDistance, because "what would be sharp" is a
    /// question about the lens, not about the tracking mode.
    iris::lens::FocusInfo focusInfo() const;

    /// Turns the camera (in its OWN space, from its local position) to face
    /// `target`, with world +Y as the frame's up. Straight up or straight down
    /// — where +Y is parallel to the view and gives no roll — the camera's
    /// current HEADING is the frame's up instead (top of the frame looking
    /// down, bottom looking up), so the pose is defined and keeps the
    /// direction the camera was facing. A target AT the camera changes nothing.
    void lookAt(iris::Vec3 target);
    void updateCameraMatrices();
	void setOrthagonalZoom(float size);
    void update(float dt) override;

    static CameraNodePtr create() {
        return QSharedPointer<CameraNode>(new CameraNode());
    }

    /**
     * Calculate picking ray given the screen position.
     * Assumes the ray's origin is the camera's position.
     * @param viewPortWidth
     * @param viewPortHeight
     * @param pos point in screen space
     * @return
     */
    iris::Vec3 calculatePickingDirection(int viewPortWidth, int viewPortHeight, QPointF pos);

    virtual QList<Property*> getProperties() override;
    virtual QVariant getPropertyValue(QString valueName) override;
    virtual bool setPropertyValue(QString valueName, const QVariant &value) override;

	SceneNodePtr createDuplicate() override;

protected:
    /// A camera's focus TARGET is a node guid (Track mode), so a duplicated or
    /// pasted camera must follow the COPY of the thing it was following — not
    /// the original (CLIPBOARD_SPEC §3.2, the recorded gap). A target outside
    /// the copied subtree keeps its guid, which is what "this camera watches
    /// that character" means.
    void remapOwnNodeReferences(const QHash<QString, QString> &guidMap) override;
public:

private:
    CameraNode()
    {
        // THE line this class never had (CAMERAS_SPEC §1, the type-enum trap).
        // Until cameras became scene-graph citizens nothing assigned it, so
        // every switch on getSceneNodeType() read a camera as `Empty` and the
        // exporters had to dynamic_cast around it. Setting it re-routes every
        // one of those switches — the phase-1 sweep audited each.
        sceneNodeType = SceneNodeType::Camera;
        angle = 45;         // Degrees are always used internally
        framingAspectRatio = 0.0f;   // no hold: every authored camera, forever
        nearClip = 0.1f;
        farClip = 500.0f;
        aspectRatio = 1.0f; // Assumes a square viewport by default
		orthoSize = 10.0f;
        exportable = false;
		projMode = CameraProjection::Perspective;
        // CAMERAS_SPEC §2 defaults. 36x24 is full frame; with the historical
        // 45 degree angle that is a ~28.97 mm lens.
        sensorWidth = 36.0f;
        sensorHeight = 24.0f;
        authorMode = CameraAuthorMode::Degrees;
        // CAMERA_LENS_SPEC §3 defaults, chosen so a camera made today projects
        // EXACTLY what a camera made yesterday did: vertical fit is the old
        // hard-coded binding, a squeeze of 1 is spherical, and zero shift is a
        // centred frustum.
        sensorFit = CameraSensorFit::Vertical;
        anamorphicSqueeze = 1.0f;
        lensShiftX = 0.0f;
        lensShiftY = 0.0f;
        constrainAspect = false;
        dofEnabled = false;
        focusMode = CameraFocusMode::Manual;
        focusDistance = 10.0f;
        fStop = 2.8f;
        focusOffset = 0.0f;
        smoothFocus = false;
        focusSmoothingSpeed = 8.0f;
        minFocusDistance = 0.1f;
        bladeCount = 5;
        focusPlaneVisible = false;
        outputHeight = 1080;
        bodyVisible = true;
        // CAMERA_LENS_SPEC §4/§5 defaults. Inherit + an empty override map is
        // the "this block does not exist" state, so a camera made today pushes
        // exactly the post description a camera made yesterday did.
        exposureMode = CameraExposureMode::Inherit;
        exposure = 0.0f;
        // +-3.5 stops: a window close to the world's own default (+-2.5 on the
        // chain's axis) and a round number in the unit the camera speaks.
        exposureMin = -3.5f;
        exposureMax = 3.5f;
		// Was left indeterminate (only setProjection() wrote it): any consumer
		// of a camera that never called setProjection read garbage — the engine
		// mirror rendered preview cameras ORTHOGRAPHIC when the garbage came up
		// false. Keep it in lock-step with projMode.
		isPerspective = true;
		vrViewScale = 2.0f; // good default
        updateCameraMatrices();
    }

};

}
#endif // CAMERANODE_H
