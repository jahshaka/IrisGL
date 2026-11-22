#pragma once
// THE ENGINE's CAMERA TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <algorithm>
#include <cmath>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

/// A View's camera. Position/orientation are absolute (the document composes them).
struct CameraDesc {
    Vec3  position;
    Quat  orientation;                 // camera looks down its local -Z
    float fovDegrees = 45.0f;          // vertical
    float nearClip = 0.1f, farClip = 1000.0f;
    bool  orthographic = false;
    float orthoSize = 10.0f;           // HALF the vertical extent when orthographic
                                       // (the document camera's ortho(-s..+s) convention)

    /// LETTERBOX the camera to `aspect` instead of filling the target
    /// (CAMERAS_SPEC §2/§7.4). Off (the default) is the historical behaviour:
    /// the camera's aspect follows the target, so the image always fills it.
    ///
    /// On, the drawn region is the largest `aspect`-shaped rectangle that fits
    /// in the target (or in the PiP rect), centred, with bars in the remainder —
    /// and the camera's own aspect ratio is FROZEN at `aspect` (Ogre's
    /// setAutoAspectRatio goes off), because that is what stops the image being
    /// stretched into the outer rectangle instead of fitted into the inner one.
    bool  constrainAspect = false;
    /// The authored aspect (width / height), used only when constrainAspect.
    float aspect = 16.0f / 9.0f;

    /// THE WIDE-ASPECT FRAMING HOLD (owner report 2026-09-07; RE-SCOPED
    /// 2026-09-08, see below). `fovDegrees` is VERTICAL, so the HORIZONTAL
    /// angle it produces grows with the target's aspect: the default 45-degree
    /// explorer is 75 degrees wide at 16:9 and 118 degrees wide at 32:9 — a
    /// fisheye nobody asked for, on exactly the monitors people buy to see more
    /// of a scene.
    ///
    /// A positive value is THE ASPECT THIS CAMERA'S FRAMING IS HELD AT. At or
    /// below it NOTHING HAPPENS — `fovDegrees` reaches setFOVy bit-for-bit, so
    /// the picture is exactly the one a camera that never heard of this field
    /// produces. Above it the vertical angle narrows just enough to keep the
    /// HORIZONTAL extent the shot had at that aspect
    /// (verticalFovForFramingAspect below), which is the fisheye fix, scoped to
    /// the ultra-wide and full-screen cases it was always about.
    ///
    /// WHY AN ASPECT AND NOT A DEGREE CAP (the 2026-09-08 owner-blocking
    /// defect). This field first shipped as `maxHorizontalFovDegrees`, a FIXED
    /// 95: any camera whose horizontal angle exceeded 95 had its vertical angle
    /// re-derived, and a 75-degree lens crosses 95 horizontal at 1.42:1 — so
    /// the shipped Grand Showroom rendered at 63 degrees vertical instead of 75
    /// on EVERY ordinary monitor, and every imported asset looked too big. A
    /// degree cap cannot express "leave ordinary windows alone", because what
    /// counts as ordinary depends on the camera's own lens. An aspect can: the
    /// hold point is the same 16:9 for a 30-degree lens and a 90-degree one,
    /// and identity below it is exact rather than approximate.
    ///
    /// Zero — the DEFAULT — is off, which is what every AUTHORED camera gets: a
    /// scene camera's angle is a deliberate lens choice and the engine must not
    /// second-guess it. Only the two FREE cameras (the editor explorer, the
    /// player's fly camera) set it, and only they can, because only their hosts
    /// know they are free.
    float framingAspect = 0.0f;

    /// LENS SHIFT (CAMERA_LENS_SPEC §3), as a FRACTION OF THE FRAME: +0.5 in X
    /// slides the image half a frame to the right without rotating the camera.
    /// Zero (the default) is a centred, symmetric frustum — byte-identical to a
    /// camera that never heard of lens shift.
    ///
    /// THE FRACTION IS WHAT CROSSES THE BOUNDARY, deliberately. The projection
    /// wants a WORLD-SPACE offset at the near plane, and that offset depends on
    /// the field of view, the near distance AND the aspect the view is actually
    /// rendering at — which only the engine knows (an unconstrained view adopts
    /// its target's aspect). A host that pre-multiplied would be shifting by
    /// the wrong amount on every window that is not the shape it assumed.
    float lensShiftX = 0.0f;
    float lensShiftY = 0.0f;

    bool operator==(const CameraDesc &o) const {
        return position.x == o.position.x && position.y == o.position.y &&
               position.z == o.position.z &&
               orientation.x == o.orientation.x && orientation.y == o.orientation.y &&
               orientation.z == o.orientation.z && orientation.w == o.orientation.w &&
               fovDegrees == o.fovDegrees && nearClip == o.nearClip && farClip == o.farClip &&
               orthographic == o.orthographic && orthoSize == o.orthoSize &&
               constrainAspect == o.constrainAspect && aspect == o.aspect &&
               framingAspect == o.framingAspect &&
               lensShiftX == o.lensShiftX && lensShiftY == o.lensShiftY;
    }
    bool operator!=(const CameraDesc &o) const { return !(*this == o); }
};

/// The vertical angle of view that holds this camera's HORIZONTAL extent at
/// the one it has on a frame of `framingAspect` — CameraDesc::framingAspect.
///
///     tan(v'/2) = tan(v/2) * framingAspect / aspect     when aspect > framingAspect
///     v'        = v                                     otherwise, EXACTLY
///
/// The first line is the two standard cross-axis identities composed and
/// simplified — hold h, solve for v:
///
///     h  = 2 * atan( tan(v/2) * framingAspect )    the extent being held
///     v' = 2 * atan( tan(h/2) / aspect )           the angle that holds it
///
/// The hold only ever NARROWS, and only STRICTLY ABOVE the framing aspect: on
/// a 16:9 hold every 16:10, 3:2, 4:3 and 16:9 target takes the early return
/// and the authored angle is passed through with NO ARITHMETIC AT ALL, which
/// is what lets every existing 16:9 pixel assertion stay byte-exact (the
/// aspect comparison is exact; a degree-cap round trip was not). Free (a
/// non-positive framing aspect or aspect) is likewise the identity.
///
/// A free function, in the header, deliberately: this is the whole of the
/// policy, and a suite can drive it across an aspect sweep with no engine at
/// all (tests/cameras' framing-hold case).
inline float verticalFovForFramingAspect(float vfovDeg, float aspect, float framingAspect) {
    if (!(framingAspect > 0.0f) || !(aspect > 0.0f) || !(vfovDeg > 0.0f)) return vfovDeg;
    if (aspect <= framingAspect) return vfovDeg;   // inside the hold: untouched, bit for bit
    const float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float haveHalf = std::min(vfovDeg, 179.0f) * 0.5f * kDegToRad;
    const float wantHalf = std::atan(std::tan(haveHalf) * (framingAspect / aspect));
    return std::max(1.0f, wantHalf * 2.0f / kDegToRad);
}

/// THE PICTURE-IN-PICTURE INSET (CAMERAS_SPEC §7.7): a second camera's view of
/// the SAME scene, composited into a rectangle of this View's target.
///
/// Mechanism, and why it is this one: Ogre's own split-screen recipe — a SECOND
/// CompositorWorkspace on the SAME render target, placed by addWorkspace's
/// vpOffsetScale and moved live by setViewportModifier. Route A of
/// CAMERAS_SPEC §7.2, proven on Vulkan at our pin by
/// spikes/camera-pip-vulkan/FINDINGS.md before a line of this was written.
///
/// What the spike found, in one place, because every one of them is a rule this
/// struct's implementation obeys:
///   * the inset workspace must be LAST on the target, and there is no reorder
///     API — every rebuild of the main workspace re-appends it, so the inset is
///     removed and re-added after each one (OgreView::attachWorkspace);
///   * everything it writes ON THE WINDOW must LOAD colour (a Clear on Vulkan
///     is full-target and would wipe the main frame), and the inset needs its
///     OWN BACKGROUND for the same reason: a loaded attachment shows the main
///     image wherever the inset draws nothing;
///   * addWorkspace's vpModifierMask defaults to 0x00, which silently makes the
///     rect inert — it is passed 0xFF;
///   * the main chain's final pass must keep MSAA samples AND resolve them
///     (chain::kMultiWorkspaceStore) or the inset destroys the frame at 4x;
///   * cameras are POOLED and destroyed AFTER the workspace that names them
///     (the other order segfaults on the next frame).
///
/// ROUTE C, since the inset learned to grade (`tonemap`, CAMERAS_SPEC §7.2):
/// the scene pass no longer draws into the window at all. It renders into a
/// LOCAL texture sized from the inset's rectangle — its own depth, its own
/// clear, its own colour space — and a QUAD composites that texture into the
/// window at the rect, either through `HDR/FinalToneMapping` (graded) or
/// through `Ogre/Copy/4xFP32` (not). Three consequences worth stating:
///   * the local texture is sized by FRACTION of the target, so it follows a
///     window resize for free and a RECT MOVE costs nothing; only a rect
///     RESIZE re-creates it (OgreView::applyPip, checked in whole pixels so a
///     steady inset never rebuilds anything — View::pipGeneration proves it);
///   * the camera's aspect is set EXPLICITLY from the rectangle.
///     setAutoAspectRatio would take the LOCAL TEXTURE's aspect instead, which
///     is the same number only by construction and stops being one the moment
///     the texture is rounded up to whole pixels;
///   * the inset's own background is graded with it — the swatch goes through
///     the same tonemapping quad, so the letterbox bars around a constrained
///     shot cannot disagree with the background inside it.
///
/// COST: a second cull and a second render of everything the inset camera sees.
/// The spike measured +0.33 ms/frame CPU on a trivial scene, plus (Route C) one
/// texture the size of the inset and two quads over its rectangle. It is off
/// unless a host asks for it, and the editor asks only while a camera is
/// selected.
///
/// IGNORED ON OFFSCREEN VIEWS unless `allowOffscreen` — the same guarantee, in
/// the same single place, as PostFxDesc and ViewOverlayDesc. Thumbnails,
/// previews and every pixel suite must stay byte-identical whatever a host
/// pushes (tests/engine's pip_is_ignored_offscreen_unless_asked).
struct ViewPipDesc {
    /// Draw the inset at all. False is free — the workspace is not even built.
    bool enabled = false;
    /// The camera the inset renders from. Absolute, exactly like View::setCamera.
    CameraDesc camera;
    /// The inset rectangle in NORMALISED target coordinates: (0,0) is top-left,
    /// (1,1) bottom-right. Defaults to a bottom-right inset a bit under a third
    /// of the view (CAMERAS_SPEC D3).
    float left = 0.66f, top = 0.64f, width = 0.31f, height = 0.33f;
    /// The inset's own background, painted at the rect before the scene renders
    /// — and the letterbox bars when the camera constrains its aspect. Without
    /// it the main image shows through wherever the inset scene does not draw.
    Colour background { 0.06f, 0.06f, 0.07f, 1.0f };

    /// THE offscreen opt-in — same meaning and same word as
    /// PostFxDesc::allowOffscreen and ViewOverlayDesc::allowOffscreen.
    bool allowOffscreen = false;

    /// GRADE THE INSET (CAMERAS_SPEC §7.2 Route C, POST_CHAIN_SPEC §14).
    ///
    /// The inset does not render into the window any more: it renders into a
    /// LOCAL texture (RGBA16F while this is on) and is composited through the
    /// SAME `HDR/FinalToneMapping` quad the main chain uses. Without it a view
    /// whose chain tonemaps showed a RAW linear inset beside a graded main
    /// image — everything above 1.0 clipped to flat white while the viewport
    /// rolled the same highlight off. That is item 6's fourth consumer, and
    /// this is the same secondary-surface answer the thumbnails got.
    ///
    /// A host sets it from the description the view's own chain resolved to
    /// (`PostFxDesc::hdr`), so the inset grades exactly when the main view
    /// does. It is a SHAPE flag: flipping it rebuilds the inset's workspace.
    bool tonemap = false;
    /// The inset's exposure, on the chain's natural-log axis (the same units
    /// and the same meaning as PostFxDesc::exposure, NOT stops).
    ///
    /// ALWAYS FIXED, never measured: the auto path's luminance reduction rides
    /// PROCESS-GLOBAL material parameters and a wall-clock adaptation, which on
    /// a second surface is neither assertable nor per-camera. So the inset uses
    /// the fixed-exposure form of the same tonemapper (POST_CHAIN_SPEC §14), and
    /// this number IS its grade — which is what makes a PIPPED camera's own
    /// exposure visible in the inset without touching the main view. Live: a
    /// change rewrites one clear colour, never a rebuild.
    float exposure = 0.0f;

    bool operator==(const ViewPipDesc &o) const {
        return enabled == o.enabled && camera == o.camera && left == o.left && top == o.top &&
               width == o.width && height == o.height && background == o.background &&
               allowOffscreen == o.allowOffscreen && tonemap == o.tonemap &&
               exposure == o.exposure;
    }
    bool operator!=(const ViewPipDesc &o) const { return !(*this == o); }
};

/// An offscreen view's declared contract, a REQUIRED parameter of Engine::createOffscreenView (PHOTON-7 stage 10-3: no
/// frame ever sees an offscreen view without one; View::setOffscreenContract changes it later): whether its picture has
/// a HISTORY (Live: a frame of a sequence; StillPicture: a picture alone - the exposure meter's rule). It never decides
/// the GI route (ROUTE-ROWS-1 stage 0: every view of a ray scene gathers). See the setter.
/// PresentedStandIn (TEST-STREAMLINE-1 stage 3a, item 2b): THE SCRIPTABLE HEADLESS HOST's editor view - an offscreen
/// target that STANDS IN FOR A PRESENTED VIEW: every branch that tells a presented view from an offscreen one reads it
/// as presented (View::presentsAsWindow, the one predicate), so a host's verbs see the picture the editor's window
/// shows - only the target is a texture. Declared at creation and fixed for the view's life (setOffscreenContract
/// refuses it, both ways). TWO declarers, both standing in for the editor's window: the headless host
/// (createOffscreenEditorViewport) and the startup gate's warm view (src/app/shaderbuildgate.cpp: it warms the route,
/// the chain and the pass the window draws - SHADER-SYSTEM-1 stage 7a).
enum class OffscreenContract { StillPicture, Live, PresentedStandIn };

/// Where a view's one-shot readback of its own presented frame stands
/// (View::requestFrameCapture, CLOSE-SHOT-2). Idle = nothing asked; Armed = the
/// next frame this view renders is the one copied; InFlight = that frame's copy
/// is recorded and its fence has not signalled; Ready = takeFrameCapture answers
/// without waiting.
enum class FrameCaptureState { Idle, Armed, InFlight, Ready };

}}  // namespace jahshaka::engine
