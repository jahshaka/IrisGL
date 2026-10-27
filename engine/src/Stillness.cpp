// THE STILLNESS DOOR (PHOTON-2 stage 0) — Stillness.h says what it measures and why. Built on the engine's PUBLIC
// door only (Engine / Scene / View): the frame monitor's popped records, the published snapshots, the readbacks, the
// arm registry. No Ogre here: the door reads what any host could read, in one place both hosts share.
#include "jahshaka/engine/Stillness.h"

#include "jahshaka/engine/Engine.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <map>
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

/// THE FRAME'S CARD WORK BY ITS ROOTS (R3 (c)): "<kind><cache frame>" joined by ';', "-" none.
std::string cardRootsText(const CardCacheStatus &c)
{
    std::string out;
    for (const CardCacheStatus::WorkRoot &r : c.workRoots) {
        if (!out.empty()) out += ';';
        out += r.kind ? r.kind : '?';
        out += std::to_string(r.frame);
    }
    return out.empty() ? std::string("-") : out;
}

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
        unsigned index = 0;     ///< its frame in the window (0 = the stop); for a trail frame, frames before the stop
        bool trail = false;     ///< a move frame inside the window's trail (StillnessSpec::moveTrail)
        bool walkStart = false; ///< the script's first walk frame (StillnessSpec::freezeWalk engages there)
    };
    std::vector<std::vector<unsigned long long>> moveFrames;   ///< per window: the engine frames of its move
    std::vector<Planned> plan;
    size_t next = 0;            ///< the frame nextPose() hands out next
    bool begun = false, finished = false;

    // ---- what the run switched, restored at the end -----------------------------------------------------------
    bool monitorOurs = false;
    /// THE MONITOR AS THE RUN FOUND IT (G7): restore() returns it to this level - only while it still stands at the
    /// level the run set (a host that changed it during the run owns it then; one that started a capture at the very
    /// level the run set cannot be told apart, and is stopped with the run).
    MonitorLevel monitorWas = MonitorLevel::Off, monitorSet = MonitorLevel::Off;
    /// THE RECORDS COME THROUGH A TEE (PHOTON-2 F7; Engine::openMonitorTee): never the host's pop, which Studio's
    /// frame monitor owns while it records (framemonitor.cpp) — the door reads its own copy of every record.
    unsigned tee = 0u;
    /// THE TAIL: the run renders frames at the final pose, unmeasured, while something it reads is still owed - a
    /// window frame's record when a HOST'S CAPTURE stays on (the monitor was on before the run: not ours to stop, so
    /// nothing flushes the last frames' records), or a window's voxel digests (asked for without a wait, filled at a
    /// frame's drain: H5). At most kTailFrames (the monitor's holding queue publishes within its kMaxHeldFrames).
    static constexpr unsigned kTailFrames = 24u;
    unsigned tailFrames = 0u;
    /// The tail's engine frames: their records are read (a window frame's may ride them) but their GPU work is not the
    /// run's - the cost line (gpuSum) leaves them out (G7).
    std::set<unsigned long long> tailEngineFrames;
    double gatherArmWas = 0.0, reflectArmWas = 0.0, freezeArmWas = 0.0;
    bool gatherArmOn = false, reflectArmOn = false;   ///< the run has switched it on (and not off since)
    bool freezeArmOn = false;                         ///< the frozen walk's arm (StillnessSpec::freezeWalk) is on
    bool pinned = false;                              ///< the path's placement pin is on (released at the end)
    bool onscreen = false;

    // ---- the readings ---------------------------------------------------------------------------------------------
    StillnessReport report;
    std::vector<std::vector<unsigned char>> masks;   ///< per window: the counted pixels (1), at the picture's size
    std::vector<bool> maskKnown;
    std::vector<unsigned> stopGatherFrame;            ///< per window: the gather frame of the stop
    std::vector<bool> stopGatherFrameKnown;
    std::vector<bool> stopSeen;                       ///< per window: its stop frame has been rendered
    Image prev;                                       ///< the previous frame's picture (inside a window)
    Image trailPrev;                                  ///< ...and inside a move's trail
    std::vector<unsigned char> trailPrevChange;       ///< ...and its per-pixel change from the one before (J1's steps)
    unsigned long long lastKeyMoves = 0;              ///< GatherStatus::keyMoves after the last frame (L7)
    unsigned long long lastFoliageRule = 0;           ///< GatherStatus::foliageRuleAccepts after the last frame (L8)
    unsigned picW = 0, picH = 0;
    std::vector<unsigned char> classAt;               ///< per picture pixel: its class (0 = unclassified)
    /// A window frame's per-pixel change, held until the window's mask is known (the history count arrives a few
    /// frames after the stop).
    struct Queued { int window = -1; size_t frame = 0; std::vector<unsigned char> change; };
    std::vector<Queued> queued;
    /// The traced reflection frame -> (window, frame).
    std::map<unsigned, std::pair<int, size_t>> byTraced;
    /// ...and a DUMPED trail frame's (the walk's own frames: their per-texel history count, `.rhc`, for the lag law).
    std::map<unsigned, size_t> byTracedTrail;
    /// THE GATHER FRAME -> (window, trail?, frame) (StillnessSpec::classRestarts): each frame the gather recorded.
    struct FrameRef { int window = -1; bool trail = false; size_t index = 0; };
    std::map<unsigned, FrameRef> byGather;
    unsigned lastGatherMapped = 0u;                   ///< the gather's published frame counter last mapped
    /// The class restarts of the gather frame the readback holds now, into the frame it maps to (once).
    // (the pictures-only dump reads no history: gi.walk's L1 then compares a frozen walk WITH the gather's history
    // readback against one WITHOUT it - the readback must not change a picture, PHOTON-7 stage 5)
    bool readsHistory() const { return spec.classRestarts || (!spec.dumpDir.empty() && !spec.dumpRgbOnly); }
    /// THE MOVING WINDOW'S DUMP (M1): a file of the dump directory.
    std::string dumpPath(const std::string &suffix) const { return spec.dumpDir + "/" + spec.dumpName + suffix; }
    static std::string trailTag(size_t i)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "-t%03u", unsigned(i));
        return b;
    }
    void takeClassRestarts(const GatherStatus &g)
    {
        if (!readsHistory() || g.historyCount.empty() || g.historyClass.size() != g.historyCount.size()) return;
        const auto it = byGather.find(g.historyCountFrame);
        if (it == byGather.end()) return;
        StillnessWindow &w = report.windows[size_t(it->second.window)];
        std::vector<StillnessFrame> &fs = it->second.trail ? w.trail : w.frames;
        if (it->second.index >= fs.size()) return;
        StillnessFrame &f = fs[it->second.index];
        if (f.classPixels >= 0) return;
        // THE DUMP (M1): the gather's own view distance and surface test of this trail frame.
        if (!spec.dumpDir.empty() && !spec.dumpRgbOnly && it->second.trail && g.historyDist.size() == g.historyCount.size()) {
            const std::string tag = trailTag(it->second.index);
            std::ofstream d(dumpPath(tag + ".dist"), std::ios::binary);
            d.write(reinterpret_cast<const char *>(g.historyDist.data()), std::streamsize(g.historyDist.size() * 4u));
            std::vector<unsigned char> flags(g.historyCount.size());
            for (size_t i = 0; i < flags.size(); ++i)
                flags[i] = (unsigned char)((g.historyCount[i] >= 2u ? 1u : 0u) | (g.historyClass[i] ? 2u : 0u));
            std::ofstream fl(dumpPath(tag + ".flg"), std::ios::binary);
            fl.write(reinterpret_cast<const char *>(flags.data()), std::streamsize(flags.size()));
            std::ofstream sz(dumpPath(tag + ".gsize"));
            sz << g.historyCountW << " " << g.historyCountH << "\n";
            // ...and THE GATHER'S HISTORY COUNT N a texel (one byte, clamped), the lag law's N for what the gather's
            // history carries - the diffuse and the rough lobe riding it (PHOTON-7 10-3 R12: gi.walk_rough), on .gsize's grid
            std::vector<unsigned char> n(g.historyCount.size());
            for (size_t i = 0; i < n.size(); ++i) n[i] = (unsigned char)std::min<unsigned>(g.historyCount[i], 255u);
            std::ofstream hc(dumpPath(tag + ".ghc"), std::ios::binary);
            hc.write(reinterpret_cast<const char *>(n.data()), std::streamsize(n.size()));
        }
        int px = 0, rs = 0;
        for (size_t i = 0; i < g.historyCount.size(); ++i)
            if (g.historyClass[i]) {
                ++px;
                if (g.historyCount[i] <= 1u) ++rs;
            }
        f.classPixels = px;
        f.classRestarts = rs;
    }
    /// This frame's gather frame, when the gather recorded one (its published counter moved: one past it).
    void mapGatherFrame(const GatherStatus &g, int window, bool trail, size_t index)
    {
        if (!readsHistory() || !g.running || g.frame == 0u || g.frame <= lastGatherMapped) return;
        byGather[g.frame - 1u] = { window, trail, index };
        lastGatherMapped = g.frame;
    }
    unsigned lastTraced = 0u;                         ///< the reflection's traced-frame counter last seen
    std::map<unsigned long long, std::vector<std::pair<std::string, unsigned>>> rowsByFrame;
    /// THE FRAME'S GPU ROWS (PHOTON-2 round 2, M1 + N1: the walking cost per cache - vct.scrollN, vct.cascadeN,
    /// vct.light.settle, ifd.*, cards.*, the ray tier's rq.* and the gather's): "row:ms" over 0.05 ms, joined by ','.
    std::map<unsigned long long, std::string> gpuRowsByFrame;
    std::vector<MonitorRecord> records;
    std::map<std::string, std::pair<double, unsigned>> gpuSum;   ///< row -> (GPU ms summed, frames)
    /// record frame -> the frame clock's readings (StillnessFrame::frameGpuMs .. gpuRowsMs)
    struct FrameClock { float busy = -1.0f, wall = -1.0f, span = -1.0f, idle = -1.0f, unattributed = -1.0f,
                        passes = -1.0f, rows = -1.0f; };
    std::map<unsigned long long, FrameClock> frameTimes;
    /// THE HOST'S PERIOD (PHOTON-7 stage 0): the previous frameDone's entry, and this frame's read of its picture.
    std::chrono::steady_clock::time_point lastDone;
    bool haveLastDone = false;
    float readMsNow = -1.0f;
    void applyClock(StillnessFrame &f) const
    {
        const auto t = frameTimes.find(f.engineFrame);
        if (t == frameTimes.end()) return;
        f.frameGpuMs = t->second.busy;
        f.frameMs = t->second.wall;
        f.gpuSpanMs = t->second.span;
        f.gpuIdleMs = t->second.idle;
        f.gpuUnattributedMs = t->second.unattributed;
        f.gpuPassesMs = t->second.passes;
        f.gpuRowsMs = t->second.rows;
    }
    std::map<unsigned long long, std::string> slowStages;               ///< record frame -> its slowest stage

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
        if (freezeArmOn) setArm("gi.freeze", freezeArmWas);
        if (pinned) scene.setGiPlacementPin(GiPlacementPin());   // released (a message: the host's next frame)
        // flushes everything waiting into the queue (and the tee); a level the host set since is the host's
        if (monitorOurs && engine.published().monitor.level == monitorSet) setMonitor(monitorWas);
        drainRecords();
        if (tee) {
            engine.closeMonitorTee(tee);
            tee = 0u;
        }
    }

    /// A window frame whose monitor record has not come back yet.
    bool recordsMissing() const
    {
        for (const StillnessWindow &w : report.windows)
            for (const StillnessFrame &f : w.frames)
                if (f.engineFrame && !rowsByFrame.count(f.engineFrame)) return true;
        return false;
    }

    unsigned classes() const { return unsigned(report.classNames.size()); }

    bool readPicture(Image &img, std::string &err)
    {
        // (timed: the read waits for the GPU to finish the frame - StillnessFrame::readMs)
        const auto t0 = std::chrono::steady_clock::now();
        struct Timed {
            float &out; std::chrono::steady_clock::time_point t0;
            ~Timed() { out = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
        } timed{ readMsNow, t0 };
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
        if (tee) engine.popMonitorTee(tee, got);
        for (MonitorRecord &r : got) {
            std::map<std::string, double> frameMs;
            for (const CacheWork &w : r.cacheWork)
                if (w.gpuMs >= 0.0f) frameMs[w.detail] += w.gpuMs;
            if (!tailEngineFrames.count(r.frame))   // a tail frame's work is not the run's (G7)
                for (const auto &f : frameMs) {
                    gpuSum[f.first].first += f.second;
                    ++gpuSum[f.first].second;
                }
            // the frame's BUSY GPU (its span less the gaps the GPU waited on a submission), its wall time and the
            // clock's parts (StillnessFrame::periodMs ..): the span, the idle, the gaps no row names, the passes, the rows
            {
                FrameClock c;
                c.busy = r.frameGpuMs >= 0.0f && r.gpuIdleMs >= 0.0f ? r.frameGpuMs - r.gpuIdleMs : -1.0f;
                c.wall = r.totalMs;
                c.span = r.frameGpuMs;
                c.idle = r.gpuIdleMs;
                c.unattributed = r.unattributedGpuMs;
                c.passes = r.gpuMs;
                double rowsMs = 0.0;
                bool anyRow = false;
                for (const auto &f : frameMs) { rowsMs += f.second; anyRow = true; }
                c.rows = anyRow ? float(rowsMs) : -1.0f;
                frameTimes[r.frame] = c;
            }
            const FrameStage *slow = nullptr;
            for (const FrameStage &st : r.stages)
                if (!slow || st.ms > slow->ms) slow = &st;
            if (slow) {
                char b[160];
                std::snprintf(b, sizeof(b), "%s:%.2f", slow->name.c_str(), slow->ms);
                slowStages[r.frame] = b;
            }
            {
                std::string &g = gpuRowsByFrame[r.frame];
                for (const auto &fm : frameMs) {
                    // (a row under 0.05 ms is left out - but the hit write-back on every frame it runs: the rest's
                    // record runs move the trace's work into it, so its ms are read beside the trace's)
                    if (fm.second < 0.05 && fm.first != "hit.composite") continue;
                    char b[96];
                    std::snprintf(b, sizeof(b), "%s%s:%.2f", g.empty() ? "" : ",", fm.first.c_str(), fm.second);
                    g += b;
                }
            }
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
        // THE DUMP's history count of a walk frame (PHOTON-7 stage 6, the lag law): the trace texel's moving count N,
        // one byte a texel (N <= 12, the window), its grid in `.rsize` - beside the frame's `.rgb`.
        if (const auto tt = byTracedTrail.find(rq.reflectHistoryFrame); tt != byTracedTrail.end()) {
            if (!spec.dumpDir.empty()) {
                const std::string tag = trailTag(tt->second);
                std::vector<unsigned char> n(rq.reflectHistoryCount.size());
                for (size_t i = 0; i < n.size(); ++i)
                    n[i] = (unsigned char)std::min<unsigned>(rq.reflectHistoryCount[i], 255u);
                std::ofstream h(dumpPath(tag + ".rhc"), std::ios::binary);
                h.write(reinterpret_cast<const char *>(n.data()), std::streamsize(n.size()));
                std::ofstream sz(dumpPath(tag + ".rsize"));
                sz << rq.reflectHistoryW << " " << rq.reflectHistoryH << "\n";
                // ...and its moving SE in display codes (the lag law's SE_walk: float32 a texel, the same grid)
                if (rq.reflectHistorySe.size() == rq.reflectHistoryCount.size()) {
                    std::ofstream se(dumpPath(tag + ".rse"), std::ios::binary);
                    se.write(reinterpret_cast<const char *>(rq.reflectHistorySe.data()),
                             std::streamsize(rq.reflectHistorySe.size() * sizeof(float)));
                }
            }
            byTracedTrail.erase(tt);
            return;
        }
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
    /// The gather frames whose history copies the door has seen (the diagnosis of a copy that never came back).
    unsigned histSeenLo = ~0u, histSeenHi = 0u;
    void tryMask(int win, const GatherStatus &g)
    {
        if (!g.historyCount.empty()) {
            histSeenLo = std::min(histSeenLo, g.historyCountFrame);
            histSeenHi = std::max(histSeenHi, g.historyCountFrame);
        }
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

    void snapshotWindow(StillnessWindow &w, size_t win, const Image &pic)
    {
        w.lastPicture = pic;
        const GiStatus &gi = scene.published().gi;
        for (size_t c = 0; c < gi.cascades.size(); ++c) {
            w.cascadeCentre.push_back(gi.cascades[c].centre);
            w.cascadeBuiltCam.push_back(gi.cascades[c].builtCam);
        }
        w.fieldOrigin = gi.ifdMin;
        // THE DIGESTS WITHOUT A WAIT (PHOTON-2 stage 2's round, H5): asked for here, filled on the render thread at
        // the next frame's drain, taken in finish() (the run renders tail frames at the final pose while one is owed).
        // ONLY FOR THE PATH CHECK, which alone reads them (the round-2 read's W4: every window downloaded and hashed
        // every light volume on the render thread for nothing).
        if (!spec.path) return;
        if (digests.size() <= win) digests.resize(win + 1);
        digests[win] = std::make_shared<GiVoxelDigests>();
        scene.requestGiVoxelDigests(digests[win]);
    }
    /// Per window: its digests, asked for at its last frame (snapshotWindow).
    std::vector<std::shared_ptr<GiVoxelDigests>> digests;
    bool digestsPending() const
    {
        for (const auto &d : digests)
            if (d && !d->ready) return true;
        return false;
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
        w.lastHoldKeyFrame = -1;
        w.holdFrame = -1;
        w.gatherHoldFrame = -1;
        w.reflHoldFrame = -1;
        for (StillnessFrame &f : w.trail) {
            if (const auto ss = slowStages.find(f.engineFrame); ss != slowStages.end()) f.slowStage = ss->second;
            if (const auto gr = gpuRowsByFrame.find(f.engineFrame); gr != gpuRowsByFrame.end()) f.gpuRows = gr->second;
            applyClock(f);
            const auto it = rowsByFrame.find(f.engineFrame);
            if (it == rowsByFrame.end()) continue;
            std::map<std::string, unsigned> sum;
            for (const auto &r : it->second) sum[r.first] += r.second;
            for (const auto &s2 : sum) {
                f.giRows += (f.giRows.empty() ? "" : ",") + s2.first + ":" + std::to_string(s2.second);
                ++f.giRowCount;
            }
        }
        for (size_t i = 0; i < w.frames.size(); ++i) {
            StillnessFrame &f = w.frames[i];
            if (const auto ss = slowStages.find(f.engineFrame); ss != slowStages.end()) f.slowStage = ss->second;
            if (const auto gr = gpuRowsByFrame.find(f.engineFrame); gr != gpuRowsByFrame.end()) f.gpuRows = gr->second;
            applyClock(f);
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
            if (i > 0 && f.holdKey) w.lastHoldKeyFrame = int(i);
            if (w.holdFrame < 0 && f.held) w.holdFrame = int(i);
            if (w.gatherHoldFrame < 0 && f.gatherHeld) w.gatherHoldFrame = int(i);
            if (w.reflHoldFrame < 0 && f.reflHeld) w.reflHoldFrame = int(i);
        }
        stillnessDeriveLiveness(w);
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

// THE HOLD's DERIVED BOUND, ONE FORMULA (PHOTON-7 stage 10, R3's design - lab.stillness_control (3) wrote it; stage 10-3
// moved it here so scripting.e2e.world_stillness reads the same: StillnessWindow::holdBound). From the readers' FIRST REST
// frame the first hold comes within
//     (N + due)  the readers' DEMAND SPAN L = N + the due: each reader's rest indices [1, L] (the per-reader snapshot)
//   + due        the read-back's due after it (the period + the ring + the issue frame's one + 1)
//   + W          the decision's own work: its captures and card-to-card rounds, from the decision to its last GI row
//   + N + 1      the re-rest after that work, and the hold's own frame
// with N the gather's settle (StillnessFrame::settleFrames) and the due the cards' demand due (the record's period + lag +
// 1); and W'S PACE: W <= ceil(the work's pages / the tier's indirect pages a frame) + due (the rounds pipelined: one due
// covers the chain), the pages the frames' own indirect texels relit over a page. A window without a decision derives none.
void stillnessDeriveLiveness(StillnessWindow &w) {
    w.firstRestFrame = w.decisionFrame = w.decisionWorkEnd = w.holdBound = w.workPace = -1;
    w.workPages = 0.0;
    const size_t n = w.frames.size();
    for (size_t i = 0; i < n && w.firstRestFrame < 0; ++i)
        if (w.frames[i].restFrames >= 1u) w.firstRestFrame = int(i);
    for (size_t i = 1; i < n && w.decisionFrame < 0; ++i)
        if (w.frames[i].cardsDecisions > w.frames[i - 1].cardsDecisions) w.decisionFrame = int(i);
    if (w.decisionFrame < 0) return;
    w.decisionWorkEnd = w.decisionFrame;
    for (size_t i = size_t(w.decisionFrame); i < n; ++i)
        if (w.frames[i].giRowCount) w.decisionWorkEnd = int(i);
    const StillnessFrame &d = w.frames[size_t(w.decisionFrame)];
    const unsigned N = d.settleFrames;
    const unsigned due = d.cardsDemandEvery + d.cardsDemandLag + 1u;
    const int W = w.decisionWorkEnd - w.decisionFrame;
    w.holdBound = int(N + due) + int(due) + W + int(N) + 1;
    const double page = double(d.cardsPageSize) * double(d.cardsPageSize);
    const double P = page > 0.0 ? double(d.cardsIndirectTexels) / page : 0.0;
    for (int i = w.decisionFrame; i <= w.decisionWorkEnd; ++i) {
        const StillnessFrame &f = w.frames[size_t(i)];
        if (f.giRowCount && page > 0.0) w.workPages += double(f.cardsIndirectRelit) / page;   // (every round, 0 included)
    }
    if (P > 0.0) w.workPace = int(std::ceil(w.workPages / P)) + int(due);
}

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
    if (spec.frames < 2 || spec.teleportFrames == 1u) { err = "stillness: a window needs at least 2 frames"; return false; }
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
            bool first = true;
            for (const Impl::Planned &pl : m.plan) first = first && !pl.walkStart;
            for (unsigned i = 1; i <= mv.frames; ++i) {
                const float t = float(i) / float(mv.frames);
                m.plan.push_back({ lerp(e0, mv.eye, t), lerp(t0, mv.target, t), Impl::Part::Move, -1, 0 });
                if (i == 1 && first) m.plan.back().walkStart = true;
            }
            eye = mv.eye;
            target = mv.target;
        } else {
            for (unsigned i = 0; i < mv.frames; ++i) m.plan.push_back({ eye, target, Impl::Part::Move, -1, 0 });
        }
    }
    // the script's last frame IS the stop (window frame 0)
    const auto addWindow = [&](const char *name, unsigned count) {
        StillnessWindow w;
        w.name = name;
        w.eye = eye;
        w.target = target;
        const int win = int(m.report.windows.size());
        m.plan.back().part = Impl::Part::Window;
        m.plan.back().window = win;
        m.plan.back().index = 0;
        for (unsigned i = 1; i <= count; ++i) m.plan.push_back({ eye, target, Impl::Part::Window, win, i });
        w.frames.resize(count + 1);
        for (unsigned i = 0; i <= count; ++i) {
            w.frames[i].index = i;
            w.frames[i].classes.resize(m.report.classNames.size() + 1);
        }
        m.report.windows.push_back(std::move(w));
    };
    addWindow("walk", m.spec.frames);
    if (spec.path) {
        const Vec3 far(eye.x + kFarTeleportMetres, eye.y, eye.z);
        const Vec3 farT(target.x + kFarTeleportMetres, target.y, target.z);
        for (unsigned i = 0; i < kFarFrames; ++i) m.plan.push_back({ far, farT, Impl::Part::Bridge, -1, 0 });
        m.plan.push_back({ eye, target, Impl::Part::Move, -1, 0 });
        addWindow("teleport", m.spec.teleportFrames ? m.spec.teleportFrames : m.spec.frames);
    }
    {
        int nextWin = -1;
        unsigned before = 0;
        for (size_t i = m.plan.size(); i-- > 0;) {
            if (m.plan[i].part == Impl::Part::Window) {
                nextWin = m.plan[i].window;
                before = 0;
            } else {
                m.plan[i].window = nextWin;
                ++before;
                if (nextWin >= 0 && before <= m.spec.moveTrail) {
                    m.plan[i].trail = true;
                    m.plan[i].index = before;
                }
            }
        }
        for (StillnessWindow &w : m.report.windows) {
            unsigned n = 0;
            for (const Impl::Planned &pl : m.plan)
                if (pl.trail && pl.window == int(&w - m.report.windows.data())) ++n;
            w.trail.resize(n);
            for (unsigned i = 0; i < n; ++i) {
                w.trail[i].index = n - i;
                w.trail[i].classes.resize(m.report.classNames.size() + 1);
            }
        }
    }
    m.moveFrames.assign(m.report.windows.size(), {});
    m.masks.assign(m.report.windows.size(), {});
    m.maskKnown.assign(m.report.windows.size(), false);
    m.stopGatherFrame.assign(m.report.windows.size(), 0u);
    m.stopGatherFrameKnown.assign(m.report.windows.size(), false);
    m.stopSeen.assign(m.report.windows.size(), false);

    // ---- THE INSTRUMENTS -----------------------------------------------------------------------------------------
    // A CAPTURE ALREADY RUNNING keeps its records: the door reads its own copy through the tee below and never pops
    // the host's queue, so the run goes ahead beside it (G7: the stage-0 refusal left the tee's path unreachable).
    // ON-SCREEN OR OFFSCREEN: an offscreen view reads its pixels; a window is read through its frame capture
    // (the presented bytes, requested before each frame).
    {
        Image probe;
        m.onscreen = !m.view.readPixels(probe);
    }
    m.gatherArmWas = m.armNow("gather.historyReadback");
    m.reflectArmWas = m.armNow("reflect.historyReadback");
    if (spec.freezeWalk) {
        bool walks = false;
        for (const Impl::Planned &pl : m.plan) walks = walks || pl.walkStart;
        // the walk's first frame needs a frame before it: the converged state is read there
        if (!walks || m.plan.front().walkStart) {
            err = "stillness: freeze needs a walk after the script's first frame";
            return false;
        }
        m.freezeArmWas = m.armNow("gi.freeze");
        if (m.freezeArmWas > 0.5) {
            err = "stillness: freeze asked with the arm gi.freeze already on (the walk would start frozen, not converged)";
            return false;
        }
    }
    // THE TEE BEFORE THE MONITOR (no record of the run is missed); the monitor switched on only when it is off — a
    // host's running capture is left exactly as it was (and its records in the host's own queue).
    m.tee = m.engine.openMonitorTee();
    if (!m.tee) { err = "stillness: the engine opened no monitor tee"; return false; }
    m.monitorWas = m.engine.published().monitor.level;
    if (m.monitorWas == MonitorLevel::Off) {
        m.monitorSet = MonitorLevel::Review;
        m.setMonitor(m.monitorSet);
        m.monitorOurs = true;
    }
    m.begun = true;
    return true;
}

bool StillnessRun::nextPose(Vec3 &eye, Vec3 &target)
{
    Impl &m = *d;
    if (!m.begun || m.finished) return false;
    if (m.next >= m.plan.size()) {
        // THE TAIL (a host's capture on: see kTailFrames): the final pose again, unmeasured, while a record is owed.
        const bool owed = (!m.monitorOurs && m.recordsMissing()) || m.digestsPending();
        if (m.plan.empty() || m.tailFrames >= Impl::kTailFrames || !owed) return false;
        eye = m.plan.back().eye;
        target = m.plan.back().target;
        return true;
    }
    const Impl::Planned &p = m.plan[m.next];
    eye = p.eye;
    target = p.target;
    // THE READBACKS RUN ONLY WHERE THEY ARE READ (each is an image copy and a host decode a frame): the gather's
    // history count from a window's stop frame until its mask is known (frameDone turns it off), the reflection's
    // count from the first stop to the end. An arm latches at the top of the next frame — this one.
    if (m.readsHistory() && (p.trail || p.part == Impl::Part::Window) && !m.gatherArmOn)
        m.gatherArmOn = m.setArm("gather.historyReadback", 1.0);
    // (a DUMPED walk reads the reflection's count over its trail too: the dump's `.rhc`, the lag law's N)
    if (p.trail && !m.spec.dumpDir.empty() && !m.reflectArmOn)
        m.reflectArmOn = m.setArm("reflect.historyReadback", 1.0);
    if (p.part == Impl::Part::Window && p.index == 0) {
        if (!m.gatherArmOn) m.gatherArmOn = m.setArm("gather.historyReadback", 1.0);
        if (!m.reflectArmOn) m.reflectArmOn = m.setArm("reflect.historyReadback", 1.0);
    }
    if (m.onscreen && (p.part == Impl::Part::Window || p.trail)) m.view.requestFrameCapture(true);
    // THE TELEPORT'S RETURN IS PLACED WHERE THE WALK LEFT THE CHAIN (the path check at equal placements): the pin is a
    // message, applied at this frame's drain, before the frame re-places anything.
    if (p.part == Impl::Part::Window && p.window == 1 && p.index == 0 && !m.report.windows.empty()) {
        const StillnessWindow &walk = m.report.windows[0];
        GiPlacementPin pin;
        pin.on = true;
        pin.cascadeBuiltCam = walk.cascadeBuiltCam;
        pin.field = true;
        pin.fieldOrigin = walk.fieldOrigin;
        m.scene.setGiPlacementPin(pin);
        m.pinned = true;
    }
    return true;
}

bool StillnessRun::frameDone(std::string &err)
{
    Impl &m = *d;
    if (!m.begun) { err = "stillness: no frame was planned"; return false; }
    if (m.next >= m.plan.size()) {
        if (m.tailFrames >= Impl::kTailFrames) { err = "stillness: no frame was planned"; return false; }
        ++m.tailFrames;   // a tail frame: its records only
        m.tailEngineFrames.insert(m.engine.published().frame);
        m.drainRecords();
        return true;
    }
    const Impl::Planned p = m.plan[m.next++];
    // THE HOST'S PERIOD (StillnessFrame::periodMs): from the previous frameDone's entry to this one's.
    const auto doneAt = std::chrono::steady_clock::now();
    const float periodMs =
        m.haveLastDone ? float(std::chrono::duration<double, std::milli>(doneAt - m.lastDone).count()) : -1.0f;
    m.lastDone = doneAt;
    m.haveLastDone = true;
    const unsigned long long engineFrame = m.engine.published().frame;
    m.drainRecords();
    const PublishedFrame &pub = m.scene.published();
    const GatherStatus &g = pub.gi.gather;

    if (p.part != Impl::Part::Window && p.window >= 0) m.moveFrames[size_t(p.window)].push_back(engineFrame);
    // THE HOLD'S REASONS, this frame (L7): the rest key moved since the last frame, or the inputs owed work. Read
    // before the trail and window parts write the frame; the key's count is taken once, below them.
    // THE MOVE'S TRAIL (1d): every pixel, the change from the frame before (none on its first frame).
    if (p.trail) {
        StillnessWindow &w = m.report.windows[size_t(p.window)];
        StillnessFrame &f = w.trail[w.trail.size() - p.index];
        m.mapGatherFrame(g, p.window, true, w.trail.size() - p.index);
        if (!m.spec.dumpDir.empty() && pub.rayQuery.reflectFrame > m.lastTraced) {
            m.byTracedTrail[pub.rayQuery.reflectFrame - 1u] = w.trail.size() - p.index;
            m.lastTraced = pub.rayQuery.reflectFrame;
        }
        f.engineFrame = engineFrame;
        f.restFrames = g.restFrames;
        f.sinceRestart = g.sinceRestart;
        f.settleFrames = g.settleFrames;
        // (...or the cache owed the readers their hold - the stop's decision, its rounds, a report in flight:
        // readersHoldOwed, PHOTON-7 stage 10 - the hold waits on it as on its inputs)
        f.holdKey = g.keyMoves != m.lastKeyMoves || g.inputsBusy || pub.gi.cards.readersHoldOwed;
        f.ifdGrant = pub.gi.ifdGrantLastFrame;
        f.ifdSpent = pub.gi.ifdDispatchedLastFrame;
        f.ifdOwedRestarts = pub.gi.ifdOwedRestarts;
        f.ifdOwedRefines = pub.gi.ifdOwedRefines;
        f.ifdOwedIntegrations = pub.gi.ifdOwedIntegrations;
        f.ifdMarkedByMoves = pub.gi.ifdMarkedByMoves;
        f.ifdReadUnheld = pub.gi.ifdReadUnheld;
        f.ifdFollows = pub.gi.ifdFollows;
        f.ifdRestartDepth = pub.gi.ifdRestartDepth;
        f.ifdMarkLag = pub.gi.ifdMarkLag;
        f.cardsAdmitted = pub.gi.cards.admitted;
        f.cardsArrivalMark = pub.gi.cards.arrivalMarkNewest;
        f.cardsFrame = pub.gi.cards.cacheFrame;
        f.cardsDemandEvery = pub.gi.cards.demandReadEvery;
        f.cardsDemandLag = pub.gi.cards.demandReadLag;
        f.cardsBudgetTexels = pub.gi.cards.budgetTexels;
        f.cardsIndirectTexels = pub.gi.cards.indirectBudgetTexels;
        f.cardsPageSize = pub.gi.cards.pageSize;
        f.cardsSettleFrames = m.spec.cardsSettleFrames;
        f.cardsRoots = cardRootsText(pub.gi.cards);
        f.cardsRound = pub.gi.cards.workRound;
        f.cardsDecisions = pub.gi.cards.restDecisions;
        f.cardsWindowDrains = pub.gi.cards.windowDrains;
        f.cardsIndirectRelit = pub.gi.cards.indirectTexelsLastFrame;
        f.cardsRelights = pub.gi.cards.relights;
        f.cardsCaptures = pub.gi.cards.captures;
        f.cardsVisible = pub.gi.cards.visibleRelights;
        f.cardsSubQuantum = pub.gi.cards.subQuantumRelights;
        f.cardsUnmeasured = pub.gi.cards.unmeasuredRelights;
        f.cardsMoveCodes = pub.gi.cards.moveCodesLast;
        f.cardsReportsDropped = pub.gi.cards.reportsDropped;
        f.cardsChange = pub.gi.cards.demandChangeFrame;
        f.cardsFirst[0] = pub.gi.cards.demandFirst[0];
        f.cardsFirst[1] = pub.gi.cards.demandFirst[1];
        f.cardsNewest[0] = pub.gi.cards.demandNewest[0];
        f.cardsNewest[1] = pub.gi.cards.demandNewest[1];
        f.cardsOpenLag = pub.gi.cards.demandOpenLag;
        f.storeOwed = pub.gi.chainOwed;
        f.foliageRule = g.foliageRuleAccepts >= m.lastFoliageRule ? g.foliageRuleAccepts - m.lastFoliageRule : 0ull;
        f.reflRestFrames = pub.rayQuery.reflectRestFrames;
        f.reflRestMovers = pub.rayQuery.reflectRestMoverFrames;
        f.reflHeld = pub.rayQuery.reflectHeld;
        f.reflRestActive = pub.rayQuery.reflectRestActive;
        f.reflRestCapped = pub.rayQuery.reflectRestCapped;
        f.reflSources = pub.rayQuery.reflectSources;
        f.cameraCuts = g.cameraCuts;
        f.reflCuts = pub.rayQuery.reflectCameraCuts;
        Image pic;
        if (!m.readPicture(pic, err)) return false;
        f.periodMs = periodMs;
        f.readMs = m.readMsNow;
        if (!m.picW) {
            m.picW = pic.width;
            m.picH = pic.height;
            m.buildClassAt(m.picW, m.picH);
        }
        // THE DUMP (M1): this trail frame's picture, its classes and its camera.
        if (!m.spec.dumpDir.empty()) {
            const size_t ti = w.trail.size() - p.index;
            const std::string tag = Impl::trailTag(ti);
            std::vector<unsigned char> rgb(size_t(pic.width) * pic.height * 3u);
            for (size_t i = 0, n = size_t(pic.width) * pic.height; i < n; ++i) {
                rgb[i * 3u] = pic.rgba[i * 4u];
                rgb[i * 3u + 1u] = pic.rgba[i * 4u + 1u];
                rgb[i * 3u + 2u] = pic.rgba[i * 4u + 2u];
            }
            std::ofstream o(m.dumpPath(tag + ".rgb"), std::ios::binary);
            o.write(reinterpret_cast<const char *>(rgb.data()), std::streamsize(rgb.size()));
            if (m.spec.classesAt && !m.spec.dumpRgbOnly) {
                const std::vector<unsigned char> cls = m.spec.classesAt(p.eye, p.target, pic.width, pic.height);
                std::ofstream c(m.dumpPath(tag + ".cls"), std::ios::binary);
                c.write(reinterpret_cast<const char *>(cls.data()), std::streamsize(cls.size()));
            }
            std::ofstream cams(m.dumpPath("-cams.txt"), std::ios::app);
            if (ti == 0)
                cams << "# trail eye.x eye.y eye.z target.x target.y target.z | " << pic.width << " " << pic.height
                     << " fovY " << m.spec.fovYDegrees << "\n";
            cams << ti << " " << p.eye.x << " " << p.eye.y << " " << p.eye.z << " " << p.target.x << " " << p.target.y
                 << " " << p.target.z << "\n";
        }
        // NOT THE WINDOW'S FIRST TRAIL FRAME (p.index == the trail's length: the read's finding 7 — a later window's
        // first trail frame diffed against the PREVIOUS window's last picture, the bogus row at its trail's start).
        std::vector<unsigned char> change;
        if (m.trailPrev.width == pic.width && m.trailPrev.height == pic.height && p.index < w.trail.size()) {
            const unsigned n = m.classes();
            std::vector<double> sum(n + 1, 0.0);
            const unsigned char *a = pic.rgba.data(), *b = m.trailPrev.rgba.data(), *ca = m.classAt.data();
            change.assign(size_t(m.picW) * m.picH, 0u);
            for (size_t i = 0, np = size_t(m.picW) * m.picH; i < np; ++i, a += 4, b += 4) {
                const unsigned v = unsigned(std::max(std::abs(int(a[0]) - int(b[0])),
                                                     std::max(std::abs(int(a[1]) - int(b[1])), std::abs(int(a[2]) - int(b[2])))));
                change[i] = (unsigned char)std::min(v, 255u);
                const unsigned c = ca[i];
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
            // THE STEPS (J1): a pixel's change beyond the largest its 3x3 neighbourhood showed the frame before.
            if (m.trailPrevChange.size() == change.size()) {
                int steps = 0;
                const int W = int(m.picW), H = int(m.picH);
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        unsigned around = 0u;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int xx = std::clamp(x + dx, 0, W - 1), yy = std::clamp(y + dy, 0, H - 1);
                                around = std::max(around, unsigned(m.trailPrevChange[size_t(yy) * W + xx]));
                            }
                        if (unsigned(change[size_t(y) * W + x]) > around + kStillnessStepCodes) ++steps;
                    }
                f.stepPixels = steps;
            }
        }
        m.trailPrevChange = std::move(change);
        m.trailPrev = std::move(pic);
    }
    if (p.part == Impl::Part::Window) {
        StillnessWindow &w = m.report.windows[size_t(p.window)];
        StillnessFrame &f = w.frames[p.index];
        m.mapGatherFrame(g, p.window, false, p.index);
        f.engineFrame = engineFrame;
        f.restFrames = g.restFrames;
        f.sinceRestart = g.sinceRestart;
        f.settleFrames = g.settleFrames;
        // (...or the cache owed the readers their hold - the stop's decision, its rounds, a report in flight:
        // readersHoldOwed, PHOTON-7 stage 10 - the hold waits on it as on its inputs)
        f.holdKey = g.keyMoves != m.lastKeyMoves || g.inputsBusy || pub.gi.cards.readersHoldOwed;
        f.ifdGrant = pub.gi.ifdGrantLastFrame;
        f.ifdSpent = pub.gi.ifdDispatchedLastFrame;
        f.ifdOwedRestarts = pub.gi.ifdOwedRestarts;
        f.ifdOwedRefines = pub.gi.ifdOwedRefines;
        f.ifdOwedIntegrations = pub.gi.ifdOwedIntegrations;
        f.ifdMarkedByMoves = pub.gi.ifdMarkedByMoves;
        f.ifdReadUnheld = pub.gi.ifdReadUnheld;
        f.ifdFollows = pub.gi.ifdFollows;
        f.ifdRestartDepth = pub.gi.ifdRestartDepth;
        f.ifdMarkLag = pub.gi.ifdMarkLag;
        f.cardsAdmitted = pub.gi.cards.admitted;
        f.cardsArrivalMark = pub.gi.cards.arrivalMarkNewest;
        f.cardsFrame = pub.gi.cards.cacheFrame;
        f.cardsDemandEvery = pub.gi.cards.demandReadEvery;
        f.cardsDemandLag = pub.gi.cards.demandReadLag;
        f.cardsBudgetTexels = pub.gi.cards.budgetTexels;
        f.cardsIndirectTexels = pub.gi.cards.indirectBudgetTexels;
        f.cardsPageSize = pub.gi.cards.pageSize;
        f.cardsSettleFrames = m.spec.cardsSettleFrames;
        f.cardsRoots = cardRootsText(pub.gi.cards);
        f.cardsRound = pub.gi.cards.workRound;
        f.cardsDecisions = pub.gi.cards.restDecisions;
        f.cardsWindowDrains = pub.gi.cards.windowDrains;
        f.cardsIndirectRelit = pub.gi.cards.indirectTexelsLastFrame;
        f.cardsRelights = pub.gi.cards.relights;
        f.cardsCaptures = pub.gi.cards.captures;
        f.cardsVisible = pub.gi.cards.visibleRelights;
        f.cardsSubQuantum = pub.gi.cards.subQuantumRelights;
        f.cardsUnmeasured = pub.gi.cards.unmeasuredRelights;
        f.cardsMoveCodes = pub.gi.cards.moveCodesLast;
        f.cardsReportsDropped = pub.gi.cards.reportsDropped;
        f.cardsChange = pub.gi.cards.demandChangeFrame;
        f.cardsFirst[0] = pub.gi.cards.demandFirst[0];
        f.cardsFirst[1] = pub.gi.cards.demandFirst[1];
        f.cardsNewest[0] = pub.gi.cards.demandNewest[0];
        f.cardsNewest[1] = pub.gi.cards.demandNewest[1];
        f.cardsOpenLag = pub.gi.cards.demandOpenLag;
        f.storeOwed = pub.gi.chainOwed;
        f.foliageRule = g.foliageRuleAccepts >= m.lastFoliageRule ? g.foliageRuleAccepts - m.lastFoliageRule : 0ull;
        f.reflRestFrames = pub.rayQuery.reflectRestFrames;
        f.reflRestMovers = pub.rayQuery.reflectRestMoverFrames;
        f.reflHeld = pub.rayQuery.reflectHeld;
        f.reflRestActive = pub.rayQuery.reflectRestActive;
        f.reflRestCapped = pub.rayQuery.reflectRestCapped;
        f.reflSources = pub.rayQuery.reflectSources;
        f.cameraCuts = g.cameraCuts;
        f.reflCuts = pub.rayQuery.reflectCameraCuts;
        // THE VIEW HOLDS ONLY WHEN EVERY READER HOLDS (PHOTON-7 stage 10, the lead's ruling on (1)): the gather held AND,
        // where the reflection traces, the reflection held - a still view whose reflection still converges moves its
        // pixels, and the door's hold was blind to it (Showroom 2: 29-56 codes a frame for 100+ frames after the
        // gather's hold, past the canary)
        f.held = g.running && g.held && (!pub.rayQuery.reflect || pub.rayQuery.reflectHeld);
        f.gatherHeld = g.running && g.held;
        // THE TRACE THIS FRAME DID (when it did: a frame the reflection skipped leaves the counter where it was, and
        // its copy is the earlier frame's — never re-labelled as this one).
        if (pub.rayQuery.reflectFrame > m.lastTraced) {
            m.byTraced[pub.rayQuery.reflectFrame - 1u] = { p.window, p.index };
            m.lastTraced = pub.rayQuery.reflectFrame;
        }
        Image pic;
        if (!m.readPicture(pic, err)) return false;
        f.periodMs = periodMs;
        f.readMs = m.readMsNow;
        // THE WINDOW'S DUMPED FRAMES (StillnessSpec::dumpWindowFrames): the picture, and its classes
        if (!m.spec.dumpDir.empty() && std::find(m.spec.dumpWindowFrames.begin(), m.spec.dumpWindowFrames.end(),
                                                 unsigned(p.index)) != m.spec.dumpWindowFrames.end()) {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "-w%d-f%03u", p.window, unsigned(p.index));
            std::vector<unsigned char> rgb(size_t(pic.width) * pic.height * 3u);
            for (size_t i = 0, n = size_t(pic.width) * pic.height; i < n; ++i)
                for (size_t c = 0; c < 3u; ++c) rgb[i * 3u + c] = pic.rgba[i * 4u + c];
            std::ofstream o(m.dumpPath(std::string(tag) + ".rgb"), std::ios::binary);
            o.write(reinterpret_cast<const char *>(rgb.data()), std::streamsize(rgb.size()));
            if (m.spec.classesAt && !m.spec.dumpRgbOnly) {
                const std::vector<unsigned char> cls = m.spec.classesAt(p.eye, p.target, pic.width, pic.height);
                std::ofstream c(m.dumpPath(std::string(tag) + ".cls"), std::ios::binary);
                c.write(reinterpret_cast<const char *>(cls.data()), std::streamsize(cls.size()));
            }
        }
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
        if (p.index + 1u == w.frames.size()) m.snapshotWindow(w, size_t(p.window), m.prev);   // (the window's last frame)
    }
    m.lastKeyMoves = g.keyMoves;
    m.lastFoliageRule = g.foliageRuleAccepts;
    // THE FROZEN WALK (StillnessSpec::freezeWalk): the next frame is the walk's first - the freeze is switched on for
    // it (an arm latches at the top of the next frame) only when THIS frame is converged: the gather HELD (its true rest
    // mean) and the reflection HELD (every texel stopped on its rest mean) where one traces.
    if (m.spec.freezeWalk && !m.freezeArmOn && m.next < m.plan.size() && m.plan[m.next].walkStart) {
        const bool held = g.running && g.held;
        const bool reflHeld = !pub.rayQuery.reflect || pub.rayQuery.reflectHeld;
        if (!held || !reflHeld) {
            err = "stillness: freeze - the view had not converged at the walk's start (the gather " +
                  std::string(held ? "held" : "not held") + ": rest " + std::to_string(g.restFrames) + " of settle " +
                  std::to_string(g.settleFrames) + (g.running ? "" : ", no gather") + "; the reflection " +
                  (reflHeld ? "held" : "not held") + ": rest " + std::to_string(pub.rayQuery.reflectRestFrames) +
                  "): settle longer before the run";
            return false;
        }
        m.freezeArmOn = m.setArm("gi.freeze", m.spec.freezeValue);
        if (!m.freezeArmOn) {
            err = "stillness: freeze - the engine refused the arm gi.freeze";
            return false;
        }
        m.report.freezeEngineFrame = engineFrame + 1u;
        m.report.freezeRestFrames = g.restFrames;
        m.report.freezeSettleFrames = g.settleFrames;
        m.report.freezeReflRestFrames = pub.rayQuery.reflectRestFrames;
    }
    // THE LATE READINGS of any window: the stop's history count, the reflection counts.
    bool masksPending = false;
    for (int win = 0; win < int(m.report.windows.size()); ++win) {
        m.tryMask(win, g);
        if (m.stopSeen[size_t(win)] && !m.maskKnown[size_t(win)]) masksPending = true;
    }
    m.takeClassRestarts(g);
    if (m.gatherArmOn && !masksPending && !m.readsHistory())
        m.gatherArmOn = !m.setArm("gather.historyReadback", m.gatherArmWas);
    m.flushQueued();
    m.takeReflectHistory(pub.rayQuery);
    return true;
}

bool StillnessRun::finish(StillnessReport &out, std::string &err)
{
    Impl &m = *d;
    if (!m.begun) { err = "stillness: never begun"; return false; }
    m.restore();
    for (size_t i = 0; i < m.digests.size() && i < m.report.windows.size(); ++i) {
        const auto &dg = m.digests[i];
        if (!dg) continue;
        if (!dg->ready) {
            err = "stillness: the " + m.report.windows[i].name + " window's voxel digests never came back";
            return false;
        }
        m.report.windows[i].lightDigest = dg->light;
        m.report.windows[i].directDigest = dg->direct;
    }
    if (!m.queued.empty()) {
        std::string stops;
        for (size_t i = 0; i < m.report.windows.size(); ++i)
            if (!m.maskKnown[i] && m.stopSeen[i])
                stops += " " + m.report.windows[i].name + "'s stop is gather frame " +
                         (m.stopGatherFrameKnown[i] ? std::to_string(m.stopGatherFrame[i]) : std::string("(none)")) + ";";
        err = "stillness: the stop frame's history count never came back (the gather's readback; " +
              std::to_string(m.queued.size()) + " frames unread;" + stops + " the copies seen: gather frames " +
              (m.histSeenLo == ~0u ? std::string("none") : std::to_string(m.histSeenLo) + ".." + std::to_string(m.histSeenHi)) + ")";
        return false;
    }
    // ONE COUNTER (the read's finding 2): the monitor files a record for EVERY frame while it is on, keyed by its
    // frame; the door keys its frames by Engine::published().frame. The two are one count today — if they ever part
    // (an engine whose record stamp stands still), every frame's rows would silently read "none fired": refuse.
    for (const StillnessWindow &w : m.report.windows)
        for (const StillnessFrame &f : w.frames)
            if (!m.rowsByFrame.count(f.engineFrame)) {
                err = "stillness: no frame-monitor record carries engine frame " + std::to_string(f.engineFrame) +
                      " (the " + w.name + " window's frame " + std::to_string(f.index) +
                      "): the records' frame stamp and Engine::published().frame disagree";
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
        r.pathMeanCodes.assign(n + 1, 0.0);
        r.pathP99Codes.assign(n + 1, 0u);
        if (a.lastPicture.width == b.lastPicture.width && a.lastPicture.height == b.lastPicture.height) {
            std::vector<std::vector<unsigned>> hist(n + 1, std::vector<unsigned>(256, 0u));
            for (size_t i = 0; i < size_t(a.lastPicture.width) * a.lastPicture.height; ++i) {
                const unsigned char *x = &a.lastPicture.rgba[i * 4u], *y = &b.lastPicture.rgba[i * 4u];
                unsigned v = 0;
                for (int c = 0; c < 3; ++c) v = std::max(v, unsigned(std::abs(int(x[c]) - int(y[c]))));
                const unsigned cls = m.classAt.empty() ? 0u : m.classAt[i];
                const unsigned ks[2] = { 0u, cls };
                for (unsigned j = 0; j < (cls ? 2u : 1u); ++j) {
                    r.pathMaxCodes[ks[j]] = std::max(r.pathMaxCodes[ks[j]], v);
                    if (v >= 1u) ++r.pathPixelsDiffering[ks[j]];
                    ++hist[ks[j]][v];
                }
            }
            for (unsigned k = 0; k <= n; ++k) {
                unsigned long long total = 0, sum = 0;
                for (unsigned v = 0; v < 256; ++v) { total += hist[k][v]; sum += (unsigned long long)v * hist[k][v]; }
                if (!total) continue;
                r.pathMeanCodes[k] = double(sum) / double(total);
                unsigned long long acc = 0;
                for (unsigned v = 0; v < 256; ++v) {
                    acc += hist[k][v];
                    if (acc * 100ull >= total * 99ull) { r.pathP99Codes[k] = v; break; }
                }
            }
        }
        r.pathFieldEqual = a.fieldOrigin == b.fieldOrigin ? 1 : 0;
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
    for (const auto &g : m.gpuSum)
        if (g.second.second) r.gpuMs.push_back({ g.first, { g.second.first / g.second.second, g.second.second } });
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
    o << "\trest_frames\tsince_restart\tsettle_frames\theld\tgi_rows\tframe_gpu_ms\tframe_ms"
      << "\tclass_px\tclass_restart\tstep_px\tslow_stage\thold_key\tfoliage_rule"
      << "\trefl_rest_frames\trefl_held\trefl_rest_active\trefl_rest_movers\tcuts\trefl_cuts\tgpu_rows"
      << "\tperiod_ms\tread_ms\tgpu_span_ms\tgpu_idle_ms\tgpu_unattr_ms\tgpu_passes_ms\tgpu_rows_ms\trefl_rest_capped"
      << "\trefl_sources"
      << "\tifd_grant\tifd_spent\tifd_owed_restarts\tifd_owed_refines\tifd_owed_integrations\tstore_owed\tifd_marked\tifd_read_unheld\tifd_follows\tifd_restart_depth\tifd_mark_lag\tcards_admitted\tcards_arrival_mark\tcards_frame\tcards_demand_every\tcards_demand_lag\tcards_budget_texels\tcards_indirect_texels\tcards_page_size\tcards_settle_frames\tcards_roots\tcards_round\tcards_decisions\tcards_window_drains\tcards_indirect_relit\tcards_relights\tcards_captures\tcards_visible\tcards_subquantum\tcards_unmeasured\tcards_move_codes\tgather_held\tcards_reports_dropped\tcards_change\tcards_first_reflect\tcards_first_gather\tcards_newest_reflect\tcards_newest_gather\tcards_open_lag\n";
    for (const StillnessWindow &w : windows)
        for (int part = 0; part < 2; ++part)
        for (const StillnessFrame &f : part == 0 ? w.trail : w.frames) {
            o << w.name << ( part == 0 ? "-move" : "" ) << "\t" << ( part == 0 ? -int( f.index ) : int( f.index ) )
              << "\t" << f.engineFrame;
            for (const StillnessClassFrame &c : f.classes) {
                char mean[32];
                std::snprintf(mean, sizeof(mean), "%.4f", c.meanCodes);
                o << "\t" << c.pixels << "\t" << c.maxCodes << "\t" << mean << "\t" << c.changed;
                for (int b : c.reflectBins) o << "\t" << b;
            }
            o << "\t" << f.restFrames << "\t" << f.sinceRestart << "\t" << f.settleFrames << "\t" << (f.held ? 1 : 0)
              << "\t" << (f.giRows.empty() ? "-" : f.giRows);
            char ms[64];
            std::snprintf(ms, sizeof(ms), "\t%.3f\t%.3f\t%d\t%d\t%d\t", f.frameGpuMs, f.frameMs, f.classPixels,
                          f.classRestarts, f.stepPixels);
            o << ms << (f.slowStage.empty() ? "-" : f.slowStage) << "\t" << (f.holdKey ? 1 : 0) << "\t"
              << f.foliageRule << "\t" << f.reflRestFrames << "\t" << (f.reflHeld ? 1 : 0) << "\t" << f.reflRestActive
              << "\t" << f.reflRestMovers << "\t" << f.cameraCuts << "\t" << f.reflCuts << "\t"
              << (f.gpuRows.empty() ? "-" : f.gpuRows);
            char clk[160];
            std::snprintf(clk, sizeof(clk), "\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t", f.periodMs, f.readMs,
                          f.gpuSpanMs, f.gpuIdleMs, f.gpuUnattributedMs, f.gpuPassesMs, f.gpuRowsMs, f.reflRestCapped);
            // (the mode label: the sources' name, "-" where nothing traced)
            o << clk << (f.reflSources < 0 ? "-" : reflectSourcesName(ReflectSources(f.reflSources)));
            o << "\t" << f.ifdGrant << "\t" << f.ifdSpent << "\t" << f.ifdOwedRestarts << "\t" << f.ifdOwedRefines
              << "\t" << f.ifdOwedIntegrations << "\t" << (f.storeOwed ? 1 : 0) << "\t" << f.ifdMarkedByMoves
              << "\t" << f.ifdReadUnheld << "\t" << f.ifdFollows << "\t" << f.ifdRestartDepth << "\t" << f.ifdMarkLag
              << "\t" << f.cardsAdmitted << "\t" << f.cardsArrivalMark << "\t" << f.cardsFrame << "\t" << f.cardsDemandEvery
              << "\t" << f.cardsDemandLag << "\t" << f.cardsBudgetTexels << "\t" << f.cardsIndirectTexels << "\t"
              << f.cardsPageSize << "\t" << f.cardsSettleFrames << "\t" << f.cardsRoots << "\t" << f.cardsRound << "\t"
              << f.cardsDecisions << "\t" << f.cardsWindowDrains << "\t" << f.cardsIndirectRelit << "\t" << f.cardsRelights
              << "\t" << f.cardsCaptures << "\t" << f.cardsVisible << "\t" << f.cardsSubQuantum << "\t" << f.cardsUnmeasured
              << "\t" << f.cardsMoveCodes << "\t" << (f.gatherHeld ? 1 : 0) << "\t" << f.cardsReportsDropped
              << "\t" << f.cardsChange << "\t" << f.cardsFirst[0] << "\t" << f.cardsFirst[1] << "\t" << f.cardsNewest[0]
              << "\t" << f.cardsNewest[1] << "\t" << f.cardsOpenLag << "\n";
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
          << (w.visibleFromGather ? "gather" : "all") << " lastGiRow=" << w.lastGiRowFrame << " lastHoldKey=" << w.lastHoldKeyFrame << " hold=" << w.holdFrame
          << " firstRest=" << w.firstRestFrame << " decision=" << w.decisionFrame << " workEnd=" << w.decisionWorkEnd
          << " holdBound=" << w.holdBound << " pace=" << w.workPace << " pages=" << w.workPages
          << " | frames to <1 code (from stop / from last GI row; = the window: never):";
        for (size_t k = 0; k < names.size() && k < w.settleFromStop.size(); ++k)
            o << " " << names[k] << "=" << w.settleFromStop[k] << "/" << w.settleFromLastGiRow[k] << "("
              << w.visiblePixels[k] << "px)";
        o << " | the move (" << w.lastMoveRowAgo << " frames before the stop): " << (w.moveRows.empty() ? "-" : w.moveRows)
          << "\n";
    }
    if (!gpuMs.empty()) {
        o << "STILLNESS gpu ms (mean over the frames each ran):";
        char buf[64];
        for (const auto &g : gpuMs) {
            if (g.first.rfind("rq.", 0) != 0 && g.first.rfind("gather.", 0) != 0 && g.first.rfind("vct.", 0) != 0 &&
                g.first.rfind("ifd.", 0) != 0 && g.first.rfind("cards.", 0) != 0)
                continue;
            std::snprintf(buf, sizeof(buf), " %s=%.3f(%u)", g.first.c_str(), g.second.first, g.second.second);
            o << buf;
        }
        o << "\n";
    }
    if (freezeEngineFrame)
        o << "STILLNESS freeze: gi.freeze on from engine frame " << freezeEngineFrame
          << " (the walk's first), converged there: the gather held (rest " << freezeRestFrames << " of settle "
          << freezeSettleFrames << "), the reflection held (rest " << freezeReflRestFrames << ")\n";
    if (pathChecked) {
        o << "STILLNESS path walk-vs-teleport (at equal placements) | picture mean/p99/max codes, pixels >= 1:";
        for (size_t k = 0; k < names.size() && k < pathMaxCodes.size(); ++k) {
            char b[96];
            std::snprintf(b, sizeof(b), " %s=%.3f/%u/%u,%u", names[k].c_str(), pathMeanCodes[k], pathP99Codes[k],
                          pathMaxCodes[k], pathPixelsDiffering[k]);
            o << b;
        }
        o << " | field origin equal " << pathFieldEqual;
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
