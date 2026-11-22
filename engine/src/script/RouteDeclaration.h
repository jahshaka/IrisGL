#pragma once
// THE ROUTE DECLARATION's JUDGEMENT (SCRIPT-IN-ENGINE-1, the final read's F2): engine.view.declareRoutes is judged after
// every frame the host draws EXACTLY as the C++ fixtures' requireRoutes (tests/support/enginetesthelpers.h, URGENT #146):
//   * THE TWO DOORS AGREE (PHOTON-7-9A): the view's PUBLISHED route row says what the engine's own record (the inspect
//     door) says on the frame judged, or the declaration is BROKEN by name, both sides printed;
//   * THE CHAIN EXACT: gather, integrate, reflection and paused equal the declaration on every frame judged;
//   * THE PRODUCTS LIVE IF DECLARED: field, cards and store, each given inspect::kRouteProductFrames frames after the
//     declaration to come live (WAIT until then, while the chain holds).
// A pure function of what was declared and what the frame's two doors say: script.host proves it on the CPU.
#include "jahshaka/engine/EngineInspect.h"

#include <string>

namespace jahshaka { namespace engine { namespace script {

/// The route words: the view's chain (gather, integrate, reflection), the scene's products (field, cards, store) and
/// GI paused (its update budget 0).
struct RouteWords {
    bool gather = false, integrate = false, reflection = false;
    bool field = false, cards = false, store = false;
    bool paused = false;
};

/// "gather reflection store", or "none".
inline std::string routeWordsText(const RouteWords &r)
{
    std::string t;
    const auto add = [&t](bool on, const char *w) {
        if (on) t += (t.empty() ? "" : " ") + std::string(w);
    };
    add(r.gather, "gather"); add(r.integrate, "integrate"); add(r.reflection, "reflection");
    add(r.field, "field"); add(r.cards, "cards"); add(r.store, "store"); add(r.paused, "paused");
    return t.empty() ? std::string("none") : t;
}

enum class RouteJudgement { Wait, Held, Broken };

/// The declaration of view `who` judged on the `frame`-th frame after it (1 = the first): `ran` = the engine's own
/// record of that frame (the inspect door's chain, the scene's products, the pause); `published` = the view's published
/// row exists, `pub` its chain. `line` = the judged text (Held / Broken).
inline RouteJudgement judgeRoutes(const std::string &who, const RouteWords &declared, const RouteWords &ran,
                                  bool published, const RouteWords &pub, int frame, std::string &line)
{
    if (published && (pub.gather != ran.gather || pub.integrate != ran.integrate || pub.reflection != ran.reflection)) {
        RouteWords p;
        p.gather = pub.gather; p.integrate = pub.integrate; p.reflection = pub.reflection;
        RouteWords e;
        e.gather = ran.gather; e.integrate = ran.integrate; e.reflection = ran.reflection;
        line = "ROUTES " + who + ": the PUBLISHED routes {" + routeWordsText(p) + "} differ from the engine's own {" +
               routeWordsText(e) + "} on frame " + std::to_string(frame) + " after the declaration (PHOTON-7-9A: the two "
               "doors must agree)";
        return RouteJudgement::Broken;
    }
    const bool chain = ran.gather == declared.gather && ran.integrate == declared.integrate &&
                       ran.reflection == declared.reflection && ran.paused == declared.paused;
    const bool products = (!declared.field || ran.field) && (!declared.cards || ran.cards) && (!declared.store || ran.store);
    if (chain && !products && frame < inspect::kRouteProductFrames) return RouteJudgement::Wait;
    line = "ROUTES " + who + ": declared {" + routeWordsText(declared) + "}, the frame ran {" + routeWordsText(ran) +
           "} (frame " + std::to_string(frame) + " after the declaration)";
    if (chain && products) return RouteJudgement::Held;
    line += " - MISMATCH (the view's chain gather/integrate/reflection exact; the scene's field/cards/store live if "
            "declared, within " + std::to_string(inspect::kRouteProductFrames) + " frames; paused = GI's update budget 0): "
            "fix the row's premise or its declaration before any law (URGENT #146)";
    return RouteJudgement::Broken;
}

}}}  // namespace jahshaka::engine::script
