// The engine-drawn viewport overlay: the stats readout AND the loading cover.
//
// SPECS/STATS_OVERLAY_SPEC.md is the design; owner decisions D1 (adopt Ogre's
// Components/Overlay rather than hand-rolling glyph quads) and D2 (the same
// overlay IS the loading cover — the Qt ViewportCover widget is deleted) are
// what this file implements. The phase-0 evidence that it works at all on
// Vulkan at our pin is spikes/overlay-v1-vulkan/{FINDINGS.md,main.cpp}.
//
// WHAT IT DRAWS, and in what order (back to front):
//
//   Jahshaka/Hud/0Fill        an opaque full-view Panel — the loading cover
//   Jahshaka/Hud/1CoverTitle  "Loading world…" / "No world open", centred
//   Jahshaka/Hud/2CoverSub    the world's name / the "open one" hint, centred
//   Jahshaka/Hud/3StatsDrop   the stats readout's black drop shadow
//   Jahshaka/Hud/4Stats       the stats readout itself
//
// THE NAMES ARE THE DRAW ORDER, deliberately. OverlayContainer keeps its
// children in a `map<String, OverlayElement*>` and _updateRenderQueue walks it
// in iterator (i.e. ALPHABETICAL) order, so a leading digit is the only thing
// standing between "the cover fills the view" and "the cover paints over its
// own title". The submission order only survives to the GPU because attach()
// puts RQ 254 in DisableSort — see there.
//
// ONE SET FOR THE PROCESS. Ogre's OverlayManager is a singleton with a single
// overlay set, so this is a file-scope singleton too, not a per-View object.
// The engine recomposes it once a frame from whichever View is enabled and
// entitled (OgreEngine::renderOneFrame). The constraint that follows — two
// on-screen Views cannot show DIFFERENT text simultaneously — is documented on
// ViewOverlayDesc, because it is a property of the component we adopted.
//
// THREE THINGS THE SPIKE PAID FOR, all of them silent failures:
//
//   1. THE ONE-SHOT-CAPTION TRAP. A TextArea whose caption is set once, before
//      its first rendered frame, renders nothing FOR EVER. Every caption in
//      this file therefore goes through `Caption` (below), which re-applies it
//      exactly once after the frame it was set in. Read that class before
//      touching any setCaption call.
//   2. REGISTRATION ORDER. `new OverlaySystem` creates BOTH OverlayManager and
//      FontManager, so it must run after a render window exists and before any
//      resource group holding a `.fontdef` is initialised — otherwise the font
//      script is never parsed and setFontName throws. createSystem() is called
//      from the top of OgreEngine::ensureHlms for exactly that reason.
//   3. A PANEL WITH NO MATERIAL DRAWS NOTHING. PanelOverlayElement skips the
//      draw on an empty material name, and OverlayElement::_update resolves
//      that name in HLMS_UNLIT. So the cover's fill needs an HlmsUnlit
//      datablock we create and name ourselves (kFillDatablock) — colour is not
//      a property of the element. The container panel deliberately has NO
//      material, which is why it is invisible and only its children draw.
//
// TEARDOWN mirrors registration and is load-bearing: detach() per scene ->
// destroy the scenes -> destroySystem() -> delete Root. ~OverlaySystem deletes
// the FontManager, and Font::unloadResource destroys the HlmsUnlit datablock it
// created — an OverlaySystem outliving Root is the same class of bug as a
// MeshPtr outliving Root.
#include "EnginePrivate.h"

#ifdef OGRE_BUILD_COMPONENT_OVERLAY
#    include <OgreFont.h>
#    include <OgreFontManager.h>
#    include <OgreOverlay.h>
#    include <OgreOverlayContainer.h>
#    include <OgreOverlayManager.h>
#    include <OgreOverlaySystem.h>
#    include <OgrePanelOverlayElement.h>
#    include <OgreTextAreaOverlayElement.h>
#endif

#include <OgreLogManager.h>
#include <OgreRenderQueue.h>
#include <OgreResourceGroupManager.h>

#include <cstdio>
#include <string>
#include <vector>

namespace jahshaka { namespace engine { namespace detail {
namespace hud {

#ifndef OGRE_BUILD_COMPONENT_OVERLAY

// The Overlay component is not in this install. Everything degrades to nothing
// drawn — never to a crash and never to a wrong picture. This branch exists so
// a box whose Ogre was built without freetype still compiles and runs; the LOUD
// failure for that case belongs in irisgl/scripts/build-ogre.sh (which greps
// the configured build for OgreNextOverlay) and in cmake/IncludeOgre.cmake
// (which will not find the library), not here.
bool available() { return false; }
void createSystem() {}
void addFontLocation(const std::string &, const std::string &) {}
void build(Ogre::Root *) {}
void attach(Ogre::SceneManager *) {}
void detach(Ogre::SceneManager *) {}
void apply(const ViewOverlayDesc &, const RenderStats &, unsigned, unsigned) {}
void hide() {}
void afterFrame() {}
void destroySystem() {}

#else

namespace {

/// The font Ogre's own debug text uses: `DebugFont.fontdef` + Inconsolata-Bold,
/// staged out of the pin's Samples/Media/packs/DebugPack.zip by
/// irisgl/engine/CMakeLists.txt. Monospace, which is what a stats readout wants.
constexpr const char *kFontName      = "DebugFont";
/// Where the staging puts the three files, relative to the media root.
constexpr const char *kFontMediaDir  = "packs/DebugPack";
/// The HlmsUnlit datablock the cover's fill Panel binds BY NAME (trap 3 above).
constexpr const char *kFillDatablock = "Jahshaka/OverlayFill";
constexpr const char *kOverlayName   = "Jahshaka/Hud";

/// Nominal text sizes IN PIXELS at scale 1.0, chosen to match what the Qt
/// ViewportCover drew (src/viewport/viewportcover.cpp: title ~1.45x the app
/// font at DemiBold, subtitle ~0.95x). They are converted to Ogre's RELATIVE
/// overlay metrics against the view's real pixel height in apply(), so the text
/// is the same physical size in a docked viewport and a fullscreen one — which
/// plain relative metrics would not give.
constexpr float kTitlePx    = 19.0f;
constexpr float kSubtitlePx = 13.0f;
constexpr float kStatsPx    = 14.0f;
/// The drop shadow's offset, in relative units. Ogre's own DebugText uses
/// exactly this (Samples/2.0/Common/src/TutorialGameState.cpp).
constexpr float kShadowOffset = 0.002f;

// ---------------------------------------------------------------------------
/// A TextArea caption that survives THE ONE-SHOT TRAP.
///
/// spikes/overlay-v1-vulkan/FINDINGS.md, "THE GOTCHA": a TextArea whose caption
/// is set ONCE, before its first rendered frame, renders NOTHING — for ever,
/// with no warning, no exception and no validation error.
/// `TextAreaOverlayElement::_update` calls `OverlayElement::_update` FIRST,
/// which runs updatePositionGeometry() and then clears mGeomPositionsOutOfDate
/// (OgreOverlayElement.cpp:384-397; only GMM_PIXELS keeps it dirty), and only
/// AFTERWARDS does `mFont->load()`. The one and only geometry build therefore
/// happens against an unloaded font — whose getGlyphAspectRatio returns 1.0 for
/// every codepoint, so the quads are built WRONG rather than degenerate — and
/// nothing ever re-flags it. Re-setting the caption at any point after the
/// first frame fixes it permanently. Reproduced identically on GL3Plus, so it
/// is not a Vulkan issue.
///
/// Ogre's own samples never see it because TutorialGameState re-captions every
/// single frame. A LIVE stats readout is immune for the same reason. A STATIC
/// caption — the loading cover's title and subtitle, the thing a user stares at
/// while a world opens — is not, which is why this exists.
///
/// set() records the text and arms a one-shot; afterFrame() (called right after
/// Root::renderOneFrame) re-applies it exactly once and disarms. Cost: one
/// extra setCaption per CHANGE, and nothing at all when nothing changed.
class Caption {
public:
    void bind(Ogre::v1::TextAreaOverlayElement *el) { mEl = el; }
    Ogre::v1::TextAreaOverlayElement *element() const { return mEl; }

    void set(const std::string &text) {
        if (!mEl) return;
        if (mHasText && text == mText) return;   // no re-layout for an unchanged string
        mText = text;
        mHasText = true;
        mEl->setCaption(text);
        mPending = true;                          // arm the one-shot
    }

    /// Re-applies the pending caption. Called once per frame, after the frame
    /// that the caption was set in has actually been drawn.
    void afterFrame() {
        if (!mPending || !mEl) return;
        mEl->setCaption(mText);
        mPending = false;
    }

    void show() { if (mEl) mEl->show(); }
    void hide() { if (mEl) mEl->hide(); }

private:
    Ogre::v1::TextAreaOverlayElement *mEl = nullptr;
    std::string mText;
    bool        mHasText = false;
    bool        mPending = false;
};

// ---- process-wide state ---------------------------------------------------
Ogre::v1::OverlaySystem      *gSystem   = nullptr;
Ogre::v1::Overlay            *gOverlay  = nullptr;
Ogre::v1::OverlayContainer   *gRoot     = nullptr;
Ogre::v1::PanelOverlayElement *gFill    = nullptr;
Ogre::HlmsUnlitDatablock     *gFillDb   = nullptr;
Caption gTitle, gSubtitle, gStats, gStatsDrop;
bool    gBuilt = false;
Colour  gFillColour{ -1.0f, -1.0f, -1.0f, -1.0f };   // never a real colour: forces the first push

Ogre::v1::TextAreaOverlayElement *makeText(Ogre::v1::OverlayManager &om, const char *name) {
    auto *el = static_cast<Ogre::v1::TextAreaOverlayElement *>(
        om.createOverlayElement("TextArea", name));
    el->setFontName(kFontName);
    el->setCharHeight(0.03f);          // replaced every apply(); a sane default meanwhile
    el->hide();
    gRoot->addChild(el);
    return el;
}

/// The engine's own fallback readout, used when the host supplies no lines.
/// Keeps the boundary testable with no host at all (STATS_OVERLAY_SPEC §5.1).
std::string defaultLine(const RenderStats &s) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%.0f fps  %.1f ms\n%llu draws  %llu tris",
                  s.fps, s.frameMs, (unsigned long long)s.draws,
                  (unsigned long long)s.triangles);
    return buf;
}

}   // namespace

bool available() { return true; }

// ---------------------------------------------------------------------------
void createSystem() {
    if (gSystem) return;
    // ORDER (trap 2 in the file header): a render window must already exist,
    // and NO resource group holding a .fontdef may have been initialised yet —
    // this ctor is what creates the FontManager that parses them.
    gSystem = OGRE_NEW Ogre::v1::OverlaySystem();
}

void addFontLocation(const std::string &mediaDir, const std::string &group) {
    if (!gSystem) return;
    try {
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            mediaDir + kFontMediaDir, "FileSystem", group, false);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: overlay font media not registered: " + e.getFullDescription(),
            Ogre::LML_CRITICAL);
    }
}

void build(Ogre::Root *root) {
    if (gBuilt || !gSystem || !root) return;
    try {
        // EAGER FONT LOAD (STATS_OVERLAY_SPEC §2.1). `DebugFont` is
        // `type truetype`, so the first caption on it runs FT_Init_FreeType +
        // a glyph rasterization pass + a 512x512 texture upload. Under owner
        // decision D2 the first caption happens while a world is loading, which
        // is the worst possible moment to discover that. Pay for it here, once,
        // at engine start.
        Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(kFontName);
        if (!font) {
            // The staging step in irisgl/engine/CMakeLists.txt did not run, or
            // this tree has a bin/media from before it existed. Loud, because
            // the failure mode is otherwise a viewport that silently never
            // covers anything (STATS_OVERLAY_SPEC §9.3).
            Ogre::LogManager::getSingleton().logMessage(
                std::string("Jahshaka: overlay font '") + kFontName +
                    "' not found — bin/media/packs/DebugPack is missing. The stats "
                    "readout and the engine loading cover are DISABLED for this run. "
                    "Re-run the build so the font media stages.",
                Ogre::LML_CRITICAL);
            return;
        }
        font->load();

        // The fill datablock (trap 3). Depth off + cull none like Ogre's own
        // font datablock; an OPAQUE blendblock, because a cover that lets the
        // scene through is not a cover.
        auto *unlit = static_cast<Ogre::HlmsUnlit *>(
            root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
        Ogre::HlmsMacroblock mb;
        mb.mDepthCheck = false;
        mb.mDepthWrite = false;
        mb.mCullMode   = Ogre::CULL_NONE;
        Ogre::HlmsBlendblock bb;   // opaque
        gFillDb = static_cast<Ogre::HlmsUnlitDatablock *>(
            unlit->createDatablock(kFillDatablock, kFillDatablock, mb, bb, Ogre::HlmsParamVec()));
        gFillDb->setUseColour(true);

        Ogre::v1::OverlayManager &om = Ogre::v1::OverlayManager::getSingleton();
        gOverlay = om.create(kOverlayName);
        gRoot = static_cast<Ogre::v1::OverlayContainer *>(
            om.createOverlayElement("Panel", "Jahshaka/Hud/Container"));
        gRoot->setPosition(0.0f, 0.0f);
        gRoot->setDimensions(1.0f, 1.0f);
        // NO material on the container, deliberately: that is what makes it an
        // invisible grouping node rather than a black rectangle.

        gFill = static_cast<Ogre::v1::PanelOverlayElement *>(
            om.createOverlayElement("Panel", "Jahshaka/Hud/0Fill"));
        gFill->setMaterialName(kFillDatablock);
        gFill->setPosition(0.0f, 0.0f);
        gFill->setDimensions(1.0f, 1.0f);
        gFill->setTransparent(false);
        gFill->hide();
        gRoot->addChild(gFill);

        gTitle.bind(makeText(om, "Jahshaka/Hud/1CoverTitle"));
        gSubtitle.bind(makeText(om, "Jahshaka/Hud/2CoverSub"));
        gStatsDrop.bind(makeText(om, "Jahshaka/Hud/3StatsDrop"));
        gStats.bind(makeText(om, "Jahshaka/Hud/4Stats"));

        gTitle.element()->setAlignment(Ogre::v1::TextAreaOverlayElement::Center);
        gSubtitle.element()->setAlignment(Ogre::v1::TextAreaOverlayElement::Center);
        // The theme greys from viewportcover.cpp:106,112, kept to the byte.
        gTitle.element()->setColour(Ogre::ColourValue(198 / 255.0f, 203 / 255.0f, 214 / 255.0f));
        gSubtitle.element()->setColour(Ogre::ColourValue(128 / 255.0f, 134 / 255.0f, 148 / 255.0f));
        gStatsDrop.element()->setColour(Ogre::ColourValue::Black);

        gOverlay->add2D(gRoot);
        gOverlay->hide();       // nothing is being covered or measured yet
        gBuilt = true;
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: engine overlay not built: " + e.getFullDescription(), Ogre::LML_CRITICAL);
        gBuilt = false;
    }
}

void attach(Ogre::SceneManager *sm) {
    if (!gSystem || !sm) return;
    // The render-queue half of the component is registered PER SceneManager by
    // the host (OgreOverlaySystem.h:56-58) — which is exactly the per-scene
    // control we want: a scene that never shows an overlay pays nothing.
    sm->addRenderQueueListener(gSystem);
    // DRAW ORDER = SUBMISSION ORDER, and that is the whole reason for this
    // call. RQ 254 defaults to NormalSort, which orders renderables by a hash
    // of {transparency, macroblock, shader, mesh, texture, depth} — under which
    // the opaque cover fill and the alpha-blended font could land in either
    // order, i.e. the cover could paint over its own title. Ogre's own sample
    // uses StableSort here, which does not fix that (a stable sort still
    // sorts); DisableSort makes the queue render in the order elements were
    // added, which is the alphabetical child order this file's names encode.
    // Upstream uses DisableSort for its own reserved RQ the same way
    // (OgreParallaxCorrectedCubemap.cpp:371).
    sm->getRenderQueue()->setSortRenderQueue(
        Ogre::v1::OverlayManager::getSingleton().mDefaultRenderQueueId,
        Ogre::RenderQueue::DisableSort);
}

void detach(Ogre::SceneManager *sm) {
    if (!gSystem || !sm) return;
    sm->removeRenderQueueListener(gSystem);
}

// ---------------------------------------------------------------------------
void hide() {
    if (gBuilt && gOverlay) gOverlay->hide();
}

void apply(const ViewOverlayDesc &desc, const RenderStats &stats,
           unsigned viewWidth, unsigned viewHeight) {
    if (!gBuilt) return;
    if (!desc.anything()) { hide(); return; }

    const float w = viewWidth  ? float(viewWidth)  : 1.0f;
    const float h = viewHeight ? float(viewHeight) : 1.0f;
    // Integer-snapped, and never zero: HiDPI is unsolved tree-wide (deep audit
    // area 7 F4) and this is the only handle a Retina panel has.
    const float scale = desc.scale > 0.0f ? desc.scale : 1.0f;
    // Pixels -> Ogre's relative overlay metrics. charHeight is a fraction of
    // the viewport HEIGHT, so this is the conversion for every text size below.
    const auto rel = [h, scale](float px) { return px * scale / h; };

    gOverlay->show();

    // ---- the cover ---------------------------------------------------------
    if (desc.cover != ViewOverlayDesc::Cover::None) {
        if (gFillDb && desc.coverFill != gFillColour) {
            gFillDb->setColour(toOgre(desc.coverFill));
            gFillColour = desc.coverFill;
        }
        gFill->show();

        const float titleH = rel(kTitlePx);
        const float subH   = rel(kSubtitlePx);
        const bool  hasSub = !desc.coverSubtitle.empty();
        // The Qt cover's own layout (viewportcover.cpp:98-114): title, half a
        // subtitle-height of air, subtitle, the block centred vertically.
        const float gap   = hasSub ? subH * 0.5f : 0.0f;
        const float block = titleH + gap + (hasSub ? subH : 0.0f);
        const float top   = 0.5f - block * 0.5f;

        gTitle.element()->setCharHeight(titleH);
        gTitle.element()->setPosition(0.5f, top);       // Center alignment: x IS the centre
        gTitle.set(desc.coverTitle);
        gTitle.show();

        if (hasSub) {
            gSubtitle.element()->setCharHeight(subH);
            gSubtitle.element()->setPosition(0.5f, top + titleH + gap);
            gSubtitle.set(desc.coverSubtitle);
            gSubtitle.show();
        } else {
            gSubtitle.hide();
        }
    } else {
        gFill->hide();
        gTitle.hide();
        gSubtitle.hide();
    }

    // ---- the stats readout -------------------------------------------------
    if (desc.stats) {
        std::string text;
        if (desc.lines.empty()) {
            text = defaultLine(stats);
        } else {
            for (size_t i = 0; i < desc.lines.size(); ++i) {
                if (i) text += '\n';
                text += desc.lines[i];
            }
        }
        const float lineH = rel(kStatsPx);
        // Count the lines the caption will occupy so a bottom-anchored readout
        // grows UPWARDS instead of running off the view.
        size_t lines = 1;
        for (char c : text) if (c == '\n') ++lines;
        const float textH = lineH * float(lines);
        // Margins in PIXELS, converted per axis — a relative margin would be
        // visibly wider on one axis than the other on any non-square view.
        const float mx = 10.0f / w, my = 10.0f / h;

        using C = OverlayCorner;
        const bool right  = desc.corner == C::TopRight || desc.corner == C::BottomRight;
        const bool bottom = desc.corner == C::BottomLeft || desc.corner == C::BottomRight;
        const float x = right ? 1.0f - mx : mx;
        const float y = bottom ? 1.0f - my - textH : my;

        for (Caption *c : { &gStatsDrop, &gStats }) {
            c->element()->setAlignment(right ? Ogre::v1::TextAreaOverlayElement::Right
                                             : Ogre::v1::TextAreaOverlayElement::Left);
            c->element()->setCharHeight(lineH);
        }
        gStatsDrop.element()->setPosition(x + kShadowOffset, y + kShadowOffset);
        gStats.element()->setPosition(x, y);
        gStats.element()->setColour(toOgre(desc.colour));
        gStatsDrop.set(text);
        gStats.set(text);
        gStatsDrop.show();
        gStats.show();
    } else {
        gStatsDrop.hide();
        gStats.hide();
    }
}

void afterFrame() {
    if (!gBuilt) return;
    gTitle.afterFrame();
    gSubtitle.afterFrame();
    gStatsDrop.afterFrame();
    gStats.afterFrame();
}

void destroySystem() {
    // Elements and the overlay belong to OverlayManager, which ~OverlaySystem
    // deletes; the datablock belongs to HlmsUnlit, which Root deletes. Nothing
    // here is ours to free individually — the ORDER is what matters, and the
    // caller (OgreEngine's destructor) owns that: detach() per scene, destroy
    // the scenes, then this, then delete Root.
    if (gSystem) OGRE_DELETE gSystem;
    gSystem   = nullptr;
    gOverlay  = nullptr;
    gRoot     = nullptr;
    gFill     = nullptr;
    gFillDb   = nullptr;
    gBuilt    = false;
    gFillColour = Colour(-1.0f, -1.0f, -1.0f, -1.0f);
    gTitle = Caption(); gSubtitle = Caption(); gStats = Caption(); gStatsDrop = Caption();
}

#endif   // OGRE_BUILD_COMPONENT_OVERLAY

}   // namespace hud
}}}  // namespace jahshaka::engine::detail
