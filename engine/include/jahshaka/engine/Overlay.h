#pragma once
// THE ENGINE's OVERLAY TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

/// Where a corner-anchored readout sits in a View.
enum class OverlayCorner { TopLeft, TopRight, BottomLeft, BottomRight };

/// ONE engine-drawn overlay per View (STATS_OVERLAY_SPEC.md §5.1). It covers
/// BOTH jobs the owner asked for: a stats readout in a corner, and/or a
/// full-view cover while nothing is being presented.
///
/// One desc, not two, because the cover and the readout are the same mechanism,
/// the same Ogre overlay, the same per-pass gate and the same offscreen rule.
/// The cover fill is drawn UNDER the stats text, so "stats visible while
/// loading" is free and correct — it is the frame you most want numbers for.
///
/// IGNORED ON OFFSCREEN VIEWS unless `allowOffscreen` is set, exactly like
/// PostFxDesc and enforced in the same single place (OgreView::chainDesc): the
/// cover is up during EVERY open, so a leak into offscreen views would put a
/// flat grey panel over every thumbnail and every pixel suite in the tree.
///
/// CONSTRAINT, documented rather than hidden: Ogre's OverlayManager is a
/// PROCESS-WIDE singleton with one overlay set, so two on-screen Views cannot
/// show DIFFERENT text at the same time. The application's shape saves us — all
/// its on-screen views live on different pages, so at most one is enabled at a
/// time, and the engine composes the overlay from the enabled view's desc once
/// per frame. If that invariant is ever broken (a tear-off viewport, a second
/// window, a VR mirror) this becomes a real limitation.
struct ViewOverlayDesc {
    // ---- stats readout ----
    bool          stats = false;
    OverlayCorner corner = OverlayCorner::TopLeft;
    /// Text size multiplier. Snapped to sensible steps by the backend; HiDPI is
    /// NOT solved anywhere in this engine, so on a Retina panel this is the
    /// only handle there is.
    float         scale = 1.0f;
    Colour        colour { 1.0f, 1.0f, 1.0f, 1.0f };
    /// The lines the HOST wants drawn, top to bottom. Empty = the engine draws
    /// its own one-line RenderStats summary, which is what makes this boundary
    /// testable with no host at all.
    ///
    /// The host supplies these because the most useful number — work per tick —
    /// is the host's, not Ogre's. Rate-limit the pushes host-side (4-10 Hz): a
    /// number that changes 62 times a second is unreadable anyway.
    std::vector<std::string> lines;

    // ---- the cover (the ported ViewportCover states) ----
    enum class Cover { None, Loading, NoScene };
    Cover       cover = Cover::None;
    std::string coverTitle;      ///< "Loading world…" / "No world open"
    std::string coverSubtitle;   ///< the world's name, or the "Open or create…" hint
    /// The fill grey from viewportcover.cpp:15, kept to the pixel.
    Colour      coverFill { 44 / 255.0f, 46 / 255.0f, 52 / 255.0f, 1.0f };

    /// THE offscreen opt-in — same meaning and same word as
    /// PostFxDesc::allowOffscreen. Only the engine suite sets it.
    bool allowOffscreen = false;

    /// THE SHADOW-ATLAS INSPECTOR (SPECS/SHADOW_TOOLING_SPEC.md §4.4): a strip
    /// of thumbnails along the bottom of the view, one per shadow map, showing
    /// what the renderer actually rasterised into each rectangle of the atlas,
    /// captioned with the light it belongs to and whether its map is static.
    ///
    /// A DIAGNOSTIC, not a feature: never persisted, off in every offscreen
    /// view unless allowOffscreen, and process-wide like the rest of this HUD —
    /// it shows the atlas the PRIMARY view rendered, so a second on-screen view
    /// displays the same tiles.
    bool shadowAtlas = false;

    /// True when this desc asks for anything to be drawn at all.
    bool anything() const { return stats || shadowAtlas || cover != Cover::None; }

    bool operator==(const ViewOverlayDesc &o) const {
        return stats == o.stats && corner == o.corner && scale == o.scale &&
               colour == o.colour && lines == o.lines && cover == o.cover &&
               coverTitle == o.coverTitle && coverSubtitle == o.coverSubtitle &&
               coverFill == o.coverFill && allowOffscreen == o.allowOffscreen &&
               shadowAtlas == o.shadowAtlas;
    }
    bool operator!=(const ViewOverlayDesc &o) const { return !(*this == o); }
};

/// THE ATOM VIEW (D0-ATOM-VIEW): a false-colour picture of what the visibility
/// buffer holds, painted over the finished frame (after the post chain, before the
/// overlays) wherever an ATOM item is the visible surface. Objects the split keeps
/// on the stock PBR shader (AtomDrawStatus: blended, two-sided, skinned, ...) keep
/// their lit picture. A property of the renderer's SCENE, never saved: every view of
/// the scene that carries the id pass shows it (a VR eye pair carries none).
///   Triangles  a hash of (object, level, triangle) -> a bright random colour
///   Levels     the LOD level the cull chose, a fixed ramp from level 0 (red) through
///              orange, yellow, lime, green, cyan, blue to level 7 (violet)
///   Buckets    a hash of the decode bucket (one colour per decode draw)
///   Objects    a hash of the GPU scene slot (one colour per object)
///   Density    the triangles the cut DREW per visible pixel, per object (ATOM-2; ATOM-8 stage 0): green under 1,
///              yellow 1 to 4, red over 4 - where the drawn geometry is finer than the pixels
enum class AtomView { Off = 0, Triangles, Levels, Buckets, Objects, Density };
/// THE COUNT OF ATOM VIEWS, kept beside the enum (as kPhotonViewCount): the F6 cycle, the View Options rows and
/// world.setAtomView's names read it, never a copied 6 (BUGS-1 item 8).
constexpr int kAtomViewCount = 6;
static_assert(int(AtomView::Density) + 1 == kAtomViewCount, "kAtomViewCount is the AtomView enum's count");

/// THE PHOTON VIEW (PHOTON-VIEW-1): the lighting's own debug pictures, one at a
/// time, over every view of the renderer's SCENE that draws the post chain. A
/// property of the scene, never saved; switching it rebuilds no workspace (its
/// passes are gated by the workspace's execution mask) and Off is byte-identical.
///   Voxels        the lit voxels of ONE cascade as cubes (PhotonVoxelLighting's debug
///                 visualizer): the finest that holds the scene's lit content, or
///                 the one Scene::setPhotonVoxelCascade names
///   Probes        the irradiance field's probes as spheres coloured by their
///                 irradiance (Ogre's IrradianceField debug visualizer)
///   Cards         every surface-cache card as a quad on the plane it is captured
///                 through: filled and coloured by capture age (a log ramp, green
///                 fresh -> red at ten minutes) where it holds a capture, outlined
///                 yellow where it waits for its first
///   ScreenProbes  the screen-probe gather's probes as discs at their positions,
///                 coloured by the irradiance each probe traced
///   Diffuse       THE OPAQUE SURFACES' indirect diffuse term alone (direct light,
///                 emission and reflections off) — a pass property on the one
///                 lighting text
///   Reflections   THE OPAQUE SURFACES' specular environment term alone (probes,
///                 voxels, SSR and the ray-traced reflection) — the same mechanism.
///                 Both are written BESIDE the radiance on a mono post chain
///                 (ChainDesc::photonTerm, PHOTON-7-9A): the picture underneath keeps
///                 the scene's light (the reflection's screen trace reads it), the
///                 term is composited over it before the tonemap, and glass and
///                 every other blended or refractive surface is not in the picture
///                 (the opaque surface behind it shows its term). A graph shape: a
///                 switch to or from either rebuilds the view's chain.
///   Hits          every traced reflection pixel coloured by what shaded its ray:
///                 green = a card, blue = the voxels, cyan = the sky, magenta = the
///                 hit list (the decode), red = a dropped record; no ray = dimmed
///   GatherHits    the SCREEN-PROBE GATHER's rays (FAR-SUN-1): every probe's cell painted
///                 ray by ray (one octahedral texel to one block of the cell) in the colour
///                 of what answered it — green a card, blue the voxels (darker at each
///                 coarser cascade), cyan the sky, magenta a decode record, orange a FAR
///                 decode record, red a dropped one; a cell with no probe dimmed. Its
///                 readback is GatherStatus::rays (the arm "gather.hitsReadback").
enum class PhotonView { Off = 0, Voxels, Probes, Cards, ScreenProbes, Diffuse, Reflections, Hits, GatherHits };
constexpr int kPhotonViewCount = 9;

}}  // namespace jahshaka::engine
