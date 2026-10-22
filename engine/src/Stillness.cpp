// THE STILLNESS DOOR (PHOTON-2 stage 0) — Stillness.h says what it measures and why. Built on the engine's PUBLIC
// door only (Engine / Scene / View): the frame monitor's popped records, the published snapshots, the readbacks, the
// arm registry. No Ogre here: the door reads what any host could read, in one place both hosts share.
#include "jahshaka/engine/Stillness.h"

#include "jahshaka/engine/Engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace jahshaka {
namespace engine {

namespace {

/// Where the path check sends the camera between its two windows: past every cascade's box (Epic's outermost is
/// 60 m) and every card's residency radius, so the return is a placement from nothing — a teleport.
constexpr float kFarTeleportMetres = 4096.0f;
/// Frames spent there (the chain re-placed around the far point one cascade a frame).
constexpr unsigned kFarFrames = 4u;
/// The reflection count's bins (StillnessClassFrame::reflectBins): no answer, the warm seed (kWarmupFrames + 1),
/// 2 x, 4 x it, beyond.
constexpr unsigned kReflectBinEdges[3] = { 9u, 17u, 33u };

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parseNumbers(const std::string &text, std::vector<float> &out)
{
    out.clear();
    std::string cur;
    for (char c : text + ",") {
        if (c == ',') {
            if (cur.empty()) return false;
            char *end = nullptr;
            const double v = std::strtod(cur.c_str(), &end);
            if (!end || *end || !std::isfinite(v)) return false;
            out.push_back(float(v));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return true;
}

bool parseCount(const std::string &text, unsigned &out)
{
    char *end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || !end || *end || v < 1 || v > 100000) return false;
    out = unsigned(v);
    return true;
}

Vec3 lerp(const Vec3 &a, const Vec3 &b, float t)
{
    return Vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

std::string hex(const std::string &s) { return s.empty() ? std::string("-") : s; }

}   // namespace

bool parseStillnessScript(const std::string &text, std::vector<StillnessMove> &out, std::string &err)
{
    out.clear();
    std::vector<std::string> segs;
    std::string cur;
    for (char c : text + "/") {
        if (c == '/' || c == ';' || c == '\n') {
            const std::string t = trim(cur);
            if (!t.empty() && t[0] != '#') segs.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    for (const std::string &s : segs) {
        StillnessMove m;
        std::vector<float> n;
        const size_t colon = s.find(':');
        const std::string verb = s.substr(0, colon);
        const std::string rest = colon == std::string::npos ? std::string() : s.substr(colon + 1);
        if (verb == "teleport") {
            if (!parseNumbers(rest, n) || n.size() != 6) {
                err = "stillness: '" + s + "' — teleport:x,y,z,tx,ty,tz";
                return false;
            }
            m.kind = StillnessMove::Kind::Teleport;
            m.eye = Vec3(n[0], n[1], n[2]);
            m.target = Vec3(n[3], n[4], n[5]);
            m.frames = 1;
        } else if (verb == "walk") {
            const size_t last = rest.rfind(':');
            if (last == std::string::npos || !parseNumbers(rest.substr(0, last), n) || n.size() != 6 ||
                !parseCount(rest.substr(last + 1), m.frames)) {
                err = "stillness: '" + s + "' — walk:x,y,z,tx,ty,tz:N";
                return false;
            }
            m.kind = StillnessMove::Kind::Walk;
            m.eye = Vec3(n[0], n[1], n[2]);
            m.target = Vec3(n[3], n[4], n[5]);
        } else if (verb == "hold") {
            if (!parseCount(rest, m.frames)) {
                err = "stillness: '" + s + "' — hold:N";
                return false;
            }
            m.kind = StillnessMove::Kind::Hold;
        } else {
            err = "stillness: unknown segment '" + s + "' (teleport:..., walk:...:N, hold:N)";
            return false;
        }
        if (out.empty() && m.kind != StillnessMove::Kind::Teleport) {
            err = "stillness: the script's first segment must place the camera (teleport:x,y,z,tx,ty,tz)";
            return false;
        }
        out.push_back(m);
    }
    if (out.empty()) {
        err = "stillness: an empty script";
        return false;
    }
    return true;
}

bool stillnessFinalPose(const std::vector<StillnessMove> &moves, Vec3 &eye, Vec3 &target)
{
    bool placed = false;
    for (const StillnessMove &m : moves)
        if (m.kind != StillnessMove::Kind::Hold) {
            eye = m.eye;
            target = m.target;
            placed = true;
        }
    return placed;
}

// ---------------------------------------------------------------------------------------------------------------------

struct StillnessRun::Impl {
    Engine &engine;
    Scene &scene;
    View &view;
    StillnessSpec spec;

    /// THE PLAN: a pose per frame and what the frame is.
    enum class Part { Move, Window, Bridge };
    struct Planned {
        Vec3 eye, target;
        Part part = Part::Move;
        int window = -1;        ///< which window (Part::Window); for a Move / Bridge frame, the window it leads to
        unsigned index = 0;     ///< its frame in the window (0 = the stop)
    };
    std::vector<std::vector<unsigned long long>> moveFrames;   ///< per window: the engine frames of its move
    std::vector<Planned> plan;
    size_t next = 0;            ///< the frame nextPose() hands out next
    bool begun = false, finished = false;

    // ---- what the run switched, restored at the end -----------------------------------------------------------
    bool monitorOurs = false;
    double gatherArmWas = 0.0, reflectArmWas = 0.0;
    bool gatherArmOn = false, reflectArmOn = false;   ///< the run has switched it on (and not off since)
    bool onscreen = false;

    // ---- the readings ---------------------------------------------------------------------------------------------
    StillnessReport report;
    std::vector<std::vector<unsigned char>> masks;   ///< per window: the counted pixels (1), at the picture's size
    std::vector<bool> maskKnown;
    std::vector<unsigned> stopGatherFrame;            ///< per window: the gather frame of the stop
    std::vector<bool> stopGatherFrameKnown;
    std::vector<bool> stopSeen;                       ///< per window: its stop frame has been rendered
    Image prev;                                       ///< the previous frame's picture (inside a window)
    unsigned picW = 0, picH = 0;
    std::vector<unsigned char> classAt;               ///< per picture pixel: its class (0 = unclassified)
    /// A window frame's per-pixel change, held until the window's mask is known (the history count arrives a few
    /// frames after the stop).
    struct Queued { int window; size_t frame; std::vector<unsigned char> change; };
    std::vector<Queued> queued;
    /// Engine frame -> (window, frame) and the traced reflection frame -> (window, frame).
    std::map<unsigned long long, std::pair<int, size_t>> byEngineFrame;
    std::map<unsigned, std::pair<int, size_t>> byTraced;
    std::map<unsigned long long, std::vector<std::pair<std::string, unsigned>>> rowsByFrame;
    std::vector<MonitorRecord> records;

    Impl(Engine &e, Scene &s, View &v) : engine(e), scene(s), view(v) {}

    // THE ENGINE'S OWN ENTRIES (the arm registry, the monitor) ARE ASKED, never called from the host's thread: a
    // REQUEST runs them on the engine's side (Engine::request; the affinity law of a render-thread engine).
    double armNow(const char *name)
    {
        double v = 0.0;
        engine.request([&](Engine &e) {
            for (const ArmInfo &a : e.arms())
                if (a.name == name) v = a.value;
        });
        return v;
    }
    bool setArm(const char *name, double value)
    {
        bool ok = false;
        engine.request([&](Engine &e) { ok = e.setArm(name, value); });
        return ok;
    }
    void setMonitor(MonitorLevel level)
    {
        engine.request([&](Engine &e) { e.setFrameMonitor(level); });
    }

    void restore()
    {
        if (!begun || finished) return;
        finished = true;
        setArm("gather.historyReadback", gatherArmWas);
        setArm("reflect.historyReadback", reflectArmWas);
        if (monitorOurs) {
            setMonitor(MonitorLevel::Off);   // flushes everything waiting into the queue
            drainRecords();
        }
    }

    unsigned classes() const { return unsigned(report.classNames.size()); }

    bool readPicture(Image &img, std::string &err)
    {
        if (!onscreen) {
            if (!view.readPixels(img)) {
                err = "stillness: readPixels refused: " + engine.lastError();
                return false;
            }
        } else if (!view.takeFrameCapture(img, true)) {
            err = "stillness: the frame capture did not answer: " + engine.lastError();
            return false;
        }
        if (!img.width || !img.height) {
            err = "stillness: an empty picture";
            return false;
        }
        return true;
    }

    /// The class image resampled to the picture's size (nearest).
    void buildClassAt(unsigned w, unsigned h)
    {
        classAt.assign(size_t(w) * h, 0u);
        if (spec.classes.empty() || !spec.classW || !spec.classH) {
            std::fill(classAt.begin(), classAt.end(), (unsigned char)1u);
            return;
        }
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                const unsigned sx = std::min(spec.classW - 1, unsigned((unsigned long long)x * spec.classW / w));
                const unsigned sy = std::min(spec.classH - 1, unsigned((unsigned long long)y * spec.classH / h));
                classAt[size_t(y) * w + x] = spec.classes[size_t(sy) * spec.classW + sx];
            }
    }

    void drainRecords()
    {
        std::vector<MonitorRecord> got;
        engine.popMonitorRecords(got);
        for (MonitorRecord &r : got) {
            auto &rows = rowsByFrame[r.frame];
            for (const CacheWork &w : r.cacheWork) {
                const std::string &n = w.detail;
                const bool gi = n.rfind("vct.", 0) == 0 || n.rfind("ifd.", 0) == 0 || n.rfind("cards.", 0) == 0 ||
                                n.rfind("rq.blas", 0) == 0;
                if (gi) rows.push_back({ n, w.units });
            }
        }
    }

    /// Reduces one window frame's per-pixel change over the window's mask.
    void reduce(int win, size_t fi, const std::vector<unsigned char> &change)
    {
        StillnessWindow &w = report.windows[size_t(win)];
        StillnessFrame &f = w.frames[fi];
        const std::vector<unsigned char> &mask = masks[size_t(win)];
        const unsigned n = classes();
        std::vector<double> sum(n + 1, 0.0);
        if (w.heat.size() != change.size()) {
            w.heat.assign(change.size(), 0u);
            w.heatLate.assign(change.size(), 0u);
            w.heatW = picW;
            w.heatH = picH;
        }
        const bool late = fi * 4u >= 3u * (w.frames.size() - 1u);
        const unsigned char *mk = mask.data(), *ca = classAt.data(), *chg = change.data();
        for (size_t i = 0, np = change.size(); i < np; ++i) {
            if (!mk[i]) continue;
            const unsigned c = ca[i];
            const unsigned v = chg[i];
            if (v > w.heat[i]) w.heat[i] = (unsigned char)v;
            if (late && v > w.heatLate[i]) w.heatLate[i] = (unsigned char)v;
            // every counted pixel is in "all" ([0]); a classified one in its class too
            const unsigned ks[2] = { 0u, c };
            for (unsigned j = 0; j < (c ? 2u : 1u); ++j) {
                StillnessClassFrame &cf = f.classes[ks[j]];
                ++cf.pixels;
                cf.maxCodes = std::max(cf.maxCodes, v);
                if (v >= 1u) ++cf.changed;
                sum[ks[j]] += v;
            }
        }
        for (unsigned k = 0; k <= n; ++k)
            f.classes[k].meanCodes = f.classes[k].pixels ? sum[k] / f.classes[k].pixels : 0.0;
    }

    void flushQueued()
    {
        for (size_t q = 0; q < queued.size();) {
            if (maskKnown[size_t(queued[q].window)]) {
                reduce(queued[q].window, queued[q].frame, queued[q].change);
                queued.erase(queued.begin() + std::ptrdiff_t(q));
            } else {
                ++q;
            }
        }
    }

    /// The reflection history's copy of a traced frame the run planned: its bins per class, over the window's mask.
    void takeReflectHistory(const RayQueryStatus &rq)
    {
        if (rq.reflectHistoryCount.empty() || !rq.reflectHistoryW || !rq.reflectHistoryH) return;
        const auto it = byTraced.find(rq.reflectHistoryFrame);
        if (it == byTraced.end()) return;
        const int win = it->second.first;
        const size_t fi = it->second.second;
        if (!maskKnown[size_t(win)]) return;   // the mask first (a later copy is read then)
        StillnessFrame &f = report.windows[size_t(win)].frames[fi];
        if (f.classes[0].reflectBins[0] >= 0) return;
        for (StillnessClassFrame &cf : f.classes)
            for (int &b : cf.reflectBins) b = 0;
        const std::vector<unsigned char> &mask = masks[size_t(win)];
        for (unsigned y = 0; y < picH; ++y)
            for (unsigned x = 0; x < picW; ++x) {
                const size_t i = size_t(y) * picW + x;
                if (!mask[i]) continue;
                const unsigned tx = std::min(rq.reflectHistoryW - 1, unsigned((unsigned long long)x * rq.reflectHistoryW / picW));
                const unsigned ty = std::min(rq.reflectHistoryH - 1, unsigned((unsigned long long)y * rq.reflectHistoryH / picH));
                const unsigned n = rq.reflectHistoryCount[size_t(ty) * rq.reflectHistoryW + tx];
                const int bin = n == 0u ? 0 : n <= kReflectBinEdges[0] ? 1 : n <= kReflectBinEdges[1] ? 2
                                : n <= kReflectBinEdges[2] ? 3 : 4;
                ++f.classes[0].reflectBins[bin];
                if (classAt[i]) ++f.classes[classAt[i]].reflectBins[bin];
            }
    }

    /// The window's mask once the stop frame's history count is back (or every pixel with no gather).
    void tryMask(int win, const GatherStatus &g)
    {
        if (maskKnown[size_t(win)] || !stopSeen[size_t(win)]) return;   // its stop frame first (the picture's size)
        StillnessWindow &w = report.windows[size_t(win)];
        std::vector<unsigned char> &mask = masks[size_t(win)];
        mask.assign(size_t(picW) * picH, 0u);
        if (w.name == "teleport" || !g.running || !g.temporal) {
            // A TELEPORT keeps no history (every pixel is new); a view with no gather (or its history off) has no
            // test of its own — every pixel counts, and the report says so.
            std::fill(mask.begin(), mask.end(), (unsigned char)1u);
            w.visibleFromGather = false;
            maskKnown[size_t(win)] = true;
            return;
        }
        if (!stopGatherFrameKnown[size_t(win)] || g.historyCount.empty() || g.historyCountFrame != stopGatherFrame[size_t(win)])
            return;
        for (unsigned y = 0; y < picH; ++y)
            for (unsigned x = 0; x < picW; ++x) {
                const unsigned gx = std::min(g.historyCountW - 1, unsigned((unsigned long long)x * g.historyCountW / picW));
                const unsigned gy = std::min(g.historyCountH - 1, unsigned((unsigned long long)y * g.historyCountH / picH));
                mask[size_t(y) * picW + x] = g.historyCount[size_t(gy) * g.historyCountW + gx] >= spec.visibleAge ? 1u : 0u;
            }
        w.visibleFromGather = true;
        maskKnown[size_t(win)] = true;
    }

    void snapshotWindow(StillnessWindow &w, const Image &pic)
    {
        w.lastPicture = pic;
        const GiStatus &gi = scene.published().gi;
        for (size_t c = 0; c < gi.cascades.size(); ++c) {
            const GiVoxelStats st = scene.giVoxelStats(int(c));
            w.directDigest.push_back(st.available ? st.directDigest : std::string());
            w.lightDigest.push_back(st.available ? st.lightDigest : std::string());
            w.cascadeCentre.push_back(gi.cascades[c].centre);
        }
    }

    void completeWindow(StillnessWindow &w, size_t win)
    {
        const unsigned n = classes();
        {
            std::map<std::string, std::pair<unsigned, unsigned long long>> sum;   // name -> (frames, units)
            const std::vector<unsigned long long> &mf = moveFrames[win];
            w.lastMoveRowAgo = -1;
            for (size_t i = 0; i < mf.size(); ++i) {
                const auto it = rowsByFrame.find(mf[i]);
                if (it == rowsByFrame.end() || it->second.empty()) continue;
                std::map<std::string, unsigned> once;
                for (const auto &r : it->second) once[r.first] += r.second;
                for (const auto &o : once) { ++sum[o.first].first; sum[o.first].second += o.second; }
                w.lastMoveRowAgo = int(mf.size() - i);   // the stop is the frame after the move's last
            }
            w.moveRows.clear();
            for (const auto &s : sum)
                w.moveRows += (w.moveRows.empty() ? "" : ",") + s.first + ":" + std::to_string(s.second.first) + "/" +
                              std::to_string(s.second.second);
        }
        w.visiblePixels.assign(n + 1, 0u);
        w.settleFromStop.assign(n + 1, 0);
        w.settleFromLastGiRow.assign(n + 1, 0);
        w.lastGiRowFrame = -1;
        w.holdFrame = -1;
        for (size_t i = 0; i < w.frames.size(); ++i) {
            StillnessFrame &f = w.frames[i];
            const auto it = rowsByFrame.find(f.engineFrame);
            if (it != rowsByFrame.end()) {
                std::map<std::string, unsigned> sum;
                for (const auto &r : it->second) sum[r.first] += r.second;
                for (const auto &s : sum) {
                    f.giRows += (f.giRows.empty() ? "" : ",") + s.first + ":" + std::to_string(s.second);
                    ++f.giRowCount;
                }
            }
            if (i > 0 && f.giRowCount) w.lastGiRowFrame = int(i);
            if (w.holdFrame < 0 && f.held) w.holdFrame = int(i);
        }
        for (unsigned k = 0; k <= n; ++k) {
            if (!w.frames.empty()) w.visiblePixels[k] = w.frames.size() > 1 ? w.frames[1].classes[k].pixels : 0u;
            int last = 0;
            for (size_t i = 1; i < w.frames.size(); ++i)
                if (w.frames[i].classes[k].maxCodes >= 1u) last = int(i);
            w.settleFromStop[k] = last;
            w.settleFromLastGiRow[k] = w.lastGiRowFrame < 0 ? last : std::max(0, last - w.lastGiRowFrame);
        }
    }
};

StillnessRun::StillnessRun(Engine &engine, Scene &scene, View &view) : d(new Impl(engine, scene, view)) {}

StillnessRun::~StillnessRun()
{
    if (d) d->restore();
}

unsigned StillnessRun::plannedFrames() const { return unsigned(d->plan.size()); }

bool StillnessRun::begin(const StillnessSpec &spec, std::string &err)
{
    Impl &m = *d;
    if (m.begun) { err = "stillness: begin twice"; return false; }
    if (spec.frames < 2) { err = "stillness: the window needs at least 2 frames"; return false; }
    if (spec.visibleAge < 1 || spec.visibleAge > 15) { err = "stillness: visibleAge is 1..15 (the count's four bits)"; return false; }
    if (!spec.classes.empty() && spec.classes.size() != size_t(spec.classW) * spec.classH) {
        err = "stillness: the class image is not classW x classH";
        return false;
    }
    for (unsigned char c : spec.classes)
        if (c > spec.classNames.size()) { err = "stillness: a class index past classNames"; return false; }
    std::vector<StillnessMove> moves;
    if (!parseStillnessScript(spec.script, moves, err)) return false;
    m.spec = spec;
    m.report = StillnessReport();
    m.report.classNames = spec.classes.empty() ? std::vector<std::string>{ "all" } : spec.classNames;

    // ---- THE PLAN --------------------------------------------------------------------------------------------
    Vec3 eye, target;
    for (const StillnessMove &mv : moves) {
        if (mv.kind == StillnessMove::Kind::Teleport) {
            eye = mv.eye;
            target = mv.target;
            m.plan.push_back({ eye, target, Impl::Part::Move, -1, 0 });
        } else if (mv.kind == StillnessMove::Kind::Walk) {
            const Vec3 e0 = eye, t0 = target;
            for (unsigned i = 1; i <= mv.frames; ++i) {
                const float t = float(i) / float(mv.frames);
                m.plan.push_back({ lerp(e0, mv.eye, t), lerp(t0, mv.target, t), Impl::Part::Move, -1, 0 });
            }
            eye = mv.eye;
            target = mv.target;
        } else {
            for (unsigned i = 0; i < mv.frames; ++i) m.plan.push_back({ eye, target, Impl::Part::Move, -1, 0 });
        }
    }
    // the script's last frame IS the stop (window frame 0)
    const auto addWindow = [&](const char *name) {
        StillnessWindow w;
        w.name = name;
        w.eye = eye;
        w.target = target;
        const int win = int(m.report.windows.size());
        m.plan.back().part = Impl::Part::Window;
        m.plan.back().window = win;
        m.plan.back().index = 0;
        for (unsigned i = 1; i <= m.spec.frames; ++i) m.plan.push_back({ eye, target, Impl::Part::Window, win, i });
        w.frames.resize(m.spec.frames + 1);
        for (unsigned i = 0; i <= m.spec.frames; ++i) {
            w.frames[i].index = i;
            w.frames[i].classes.resize(m.report.classNames.size() + 1);
        }
        m.report.windows.push_back(std::move(w));
    };
    addWindow("walk");
    if (spec.path) {
        const Vec3 far(eye.x + kFarTeleportMetres, eye.y, eye.z);
        const Vec3 farT(target.x + kFarTeleportMetres, target.y, target.z);
        for (unsigned i = 0; i < kFarFrames; ++i) m.plan.push_back({ far, farT, Impl::Part::Bridge, -1, 0 });
        m.plan.push_back({ eye, target, Impl::Part::Move, -1, 0 });
        addWindow("teleport");
    }
    {
        int nextWin = -1;
        for (size_t i = m.plan.size(); i-- > 0;) {
            if (m.plan[i].part == Impl::Part::Window) nextWin = m.plan[i].window;
            else m.plan[i].window = nextWin;
        }
    }
    m.moveFrames.assign(m.report.windows.size(), {});
    m.masks.assign(m.report.windows.size(), {});
    m.maskKnown.assign(m.report.windows.size(), false);
    m.stopGatherFrame.assign(m.report.windows.size(), 0u);
    m.stopGatherFrameKnown.assign(m.report.windows.size(), false);
    m.stopSeen.assign(m.report.windows.size(), false);

    // ---- THE INSTRUMENTS -----------------------------------------------------------------------------------------
    // A CAPTURE ALREADY RUNNING keeps its records: the door would pop them from under it.
    if (m.engine.published().monitor.level != MonitorLevel::Off) {
        err = "stillness: the frame monitor is already on (a capture runs) - stop it first";
        return false;
    }
    // ON-SCREEN OR OFFSCREEN: an offscreen view reads its pixels; a window is read through its frame capture
    // (the presented bytes, requested before each frame).
    {
        Image probe;
        m.onscreen = !m.view.readPixels(probe);
    }
    m.gatherArmWas = m.armNow("gather.historyReadback");
    m.reflectArmWas = m.armNow("reflect.historyReadback");
    m.setMonitor(MonitorLevel::Review);
    m.monitorOurs = true;
    m.begun = true;
    return true;
}

bool StillnessRun::nextPose(Vec3 &eye, Vec3 &target)
{
    Impl &m = *d;
    if (!m.begun || m.next >= m.plan.size()) return false;
    const Impl::Planned &p = m.plan[m.next];
    eye = p.eye;
    target = p.target;
    // THE READBACKS RUN ONLY WHERE THEY ARE READ (each is an image copy and a host decode a frame): the gather's
    // history count from a window's stop frame until its mask is known (frameDone turns it off), the reflection's
    // count from the first stop to the end. An arm latches at the top of the next frame — this one.
    if (p.part == Impl::Part::Window && p.index == 0) {
        if (!m.gatherArmOn) m.gatherArmOn = m.setArm("gather.historyReadback", 1.0);
        if (!m.reflectArmOn) m.reflectArmOn = m.setArm("reflect.historyReadback", 1.0);
    }
    if (m.onscreen && p.part == Impl::Part::Window) m.view.requestFrameCapture(true);
    return true;
}

bool StillnessRun::frameDone(std::string &err)
{
    Impl &m = *d;
    if (!m.begun || m.next >= m.plan.size()) { err = "stillness: no frame was planned"; return false; }
    const Impl::Planned p = m.plan[m.next++];
    const unsigned long long engineFrame = m.engine.published().frame;
    m.drainRecords();
    const PublishedFrame &pub = m.scene.published();
    const GatherStatus &g = pub.gi.gather;

    if (p.part != Impl::Part::Window && p.window >= 0) m.moveFrames[size_t(p.window)].push_back(engineFrame);
    if (p.part == Impl::Part::Window) {
        StillnessWindow &w = m.report.windows[size_t(p.window)];
        StillnessFrame &f = w.frames[p.index];
        f.engineFrame = engineFrame;
        f.restFrames = g.restFrames;
        f.sinceRestart = g.sinceRestart;
        f.settleFrames = g.settleFrames;
        f.held = g.running && g.settleFrames && g.restFrames > g.settleFrames;
        m.byEngineFrame[engineFrame] = { p.window, p.index };
        if (pub.rayQuery.reflectFrame > 0u) m.byTraced[pub.rayQuery.reflectFrame - 1u] = { p.window, p.index };
        Image pic;
        if (!m.readPicture(pic, err)) return false;
        if (p.index == 0) {
            if (m.picW && (m.picW != pic.width || m.picH != pic.height)) {
                err = "stillness: the view changed size between the windows";
                return false;
            }
            if (!m.picW) {
                m.picW = pic.width;
                m.picH = pic.height;
                m.buildClassAt(m.picW, m.picH);
            }
            // THE STOP FRAME: its gather frame is the one whose history count decides the mask (the published
            // counter is one past the frame just recorded).
            if (g.running && g.frame > 0u) {
                m.stopGatherFrame[size_t(p.window)] = g.frame - 1u;
                m.stopGatherFrameKnown[size_t(p.window)] = true;
            }
            m.stopSeen[size_t(p.window)] = true;
        } else {
            if (pic.width != m.picW || pic.height != m.picH) {
                err = "stillness: the picture changed size inside a window";
                return false;
            }
            Impl::Queued q;
            q.window = p.window;
            q.frame = p.index;
            q.change.resize(size_t(m.picW) * m.picH);
            const unsigned char *a = pic.rgba.data(), *b = m.prev.rgba.data();
            unsigned char *ch = q.change.data();
            for (size_t i = 0, n = q.change.size(); i < n; ++i, a += 4, b += 4) {
                const int dr = std::abs(int(a[0]) - int(b[0])), dg = std::abs(int(a[1]) - int(b[1])),
                          db = std::abs(int(a[2]) - int(b[2]));
                ch[i] = (unsigned char)std::max(dr, std::max(dg, db));
            }
            m.queued.push_back(std::move(q));
        }
        m.prev = std::move(pic);
        if (p.index == m.spec.frames) m.snapshotWindow(w, m.prev);
    }
    // THE LATE READINGS of any window: the stop's history count, the reflection counts.
    bool masksPending = false;
    for (int win = 0; win < int(m.report.windows.size()); ++win) {
        m.tryMask(win, g);
        if (m.stopSeen[size_t(win)] && !m.maskKnown[size_t(win)]) masksPending = true;
    }
    if (m.gatherArmOn && !masksPending) m.gatherArmOn = !m.setArm("gather.historyReadback", m.gatherArmWas);
    m.flushQueued();
    m.takeReflectHistory(pub.rayQuery);
    return true;
}

bool StillnessRun::finish(StillnessReport &out, std::string &err)
{
    Impl &m = *d;
    if (!m.begun) { err = "stillness: never begun"; return false; }
    m.restore();
    if (!m.queued.empty()) {
        err = "stillness: the stop frame's history count never came back (the gather's readback; " +
              std::to_string(m.queued.size()) + " frames unread)";
        return false;
    }
    for (size_t i = 0; i < m.report.windows.size(); ++i) m.completeWindow(m.report.windows[i], i);
    StillnessReport &r = m.report;
    if (r.windows.size() == 2) {
        const StillnessWindow &a = r.windows[0], &b = r.windows[1];
        const unsigned n = unsigned(r.classNames.size());
        r.pathChecked = true;
        r.pathMaxCodes.assign(n + 1, 0u);
        r.pathPixelsDiffering.assign(n + 1, 0u);
        if (a.lastPicture.width == b.lastPicture.width && a.lastPicture.height == b.lastPicture.height) {
            for (size_t i = 0; i < size_t(a.lastPicture.width) * a.lastPicture.height; ++i) {
                const unsigned char *x = &a.lastPicture.rgba[i * 4u], *y = &b.lastPicture.rgba[i * 4u];
                unsigned v = 0;
                for (int c = 0; c < 3; ++c) v = std::max(v, unsigned(std::abs(int(x[c]) - int(y[c]))));
                const unsigned cls = m.classAt.empty() ? 0u : m.classAt[i];
                const unsigned ks[2] = { 0u, cls };
                for (unsigned j = 0; j < (cls ? 2u : 1u); ++j) {
                    r.pathMaxCodes[ks[j]] = std::max(r.pathMaxCodes[ks[j]], v);
                    if (v >= 1u) ++r.pathPixelsDiffering[ks[j]];
                }
            }
        }
        const size_t cascades = std::max(a.directDigest.size(), b.directDigest.size());
        for (size_t c = 0; c < cascades; ++c) {
            const auto eq = [&](const std::vector<std::string> &x, const std::vector<std::string> &y) {
                if (c >= x.size() || c >= y.size() || x[c].empty() || y[c].empty()) return -1;
                return x[c] == y[c] ? 1 : 0;
            };
            r.pathDirectEqual.push_back(eq(a.directDigest, b.directDigest));
            r.pathLightEqual.push_back(eq(a.lightDigest, b.lightDigest));
            r.pathCentreEqual.push_back(c < a.cascadeCentre.size() && c < b.cascadeCentre.size()
                                            ? (a.cascadeCentre[c] == b.cascadeCentre[c] ? 1 : 0) : -1);
        }
    }
    out = r;
    return true;
}

// ---- THE REPORT'S TWO TEXTS -----------------------------------------------------------------------------------------

std::string StillnessReport::tsv() const
{
    std::ostringstream o;
    o << "window\tframe\tengine_frame";
    std::vector<std::string> names{ "all" };
    for (const std::string &n : classNames) names.push_back(n);
    for (const std::string &n : names)
        o << "\t" << n << "_px\t" << n << "_max\t" << n << "_mean\t" << n << "_changed\t" << n << "_refl_none\t" << n
          << "_refl_le9\t" << n << "_refl_le17\t" << n << "_refl_le33\t" << n << "_refl_gt33";
    o << "\trest_frames\tsince_restart\tsettle_frames\theld\tgi_rows\n";
    for (const StillnessWindow &w : windows)
        for (const StillnessFrame &f : w.frames) {
            o << w.name << "\t" << f.index << "\t" << f.engineFrame;
            for (const StillnessClassFrame &c : f.classes) {
                char mean[32];
                std::snprintf(mean, sizeof(mean), "%.4f", c.meanCodes);
                o << "\t" << c.pixels << "\t" << c.maxCodes << "\t" << mean << "\t" << c.changed;
                for (int b : c.reflectBins) o << "\t" << b;
            }
            o << "\t" << f.restFrames << "\t" << f.sinceRestart << "\t" << f.settleFrames << "\t" << (f.held ? 1 : 0)
              << "\t" << (f.giRows.empty() ? "-" : f.giRows) << "\n";
        }
    return o.str();
}

std::string StillnessReport::summary() const
{
    std::ostringstream o;
    std::vector<std::string> names{ "all" };
    for (const std::string &n : classNames) names.push_back(n);
    for (const StillnessWindow &w : windows) {
        o << "STILLNESS " << w.name << " stop=(" << w.eye.x << "," << w.eye.y << "," << w.eye.z << ") frames="
          << (w.frames.empty() ? 0 : w.frames.size() - 1) << " visible="
          << (w.visibleFromGather ? "gather" : "all") << " lastGiRow=" << w.lastGiRowFrame << " hold=" << w.holdFrame
          << " | frames to <1 code (from stop / from last GI row; = the window: never):";
        for (size_t k = 0; k < names.size() && k < w.settleFromStop.size(); ++k)
            o << " " << names[k] << "=" << w.settleFromStop[k] << "/" << w.settleFromLastGiRow[k] << "("
              << w.visiblePixels[k] << "px)";
        o << " | the move (" << w.lastMoveRowAgo << " frames before the stop): " << (w.moveRows.empty() ? "-" : w.moveRows)
          << "\n";
    }
    if (pathChecked) {
        o << "STILLNESS path walk-vs-teleport | picture max codes / pixels >= 1:";
        for (size_t k = 0; k < names.size() && k < pathMaxCodes.size(); ++k)
            o << " " << names[k] << "=" << pathMaxCodes[k] << "/" << pathPixelsDiffering[k];
        o << " | cascade direct/total/placement equal:";
        for (size_t c = 0; c < pathDirectEqual.size(); ++c)
            o << " c" << c << "=" << pathDirectEqual[c] << "/" << pathLightEqual[c] << "/" << pathCentreEqual[c];
        if (windows.size() == 2) {
            o << " | direct digests:";
            for (size_t c = 0; c < windows[0].directDigest.size() && c < windows[1].directDigest.size(); ++c)
                o << " c" << c << "=" << hex(windows[0].directDigest[c]) << "~" << hex(windows[1].directDigest[c]);
        }
        o << "\n";
    }
    return o.str();
}

}   // namespace engine
}   // namespace jahshaka
