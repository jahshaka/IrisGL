// THE STILLNESS DOOR (PHOTON-2 stage 0; SPECS/PHOTON_2_SPEC.md) — the instrument of "the still world stays still".
//
// THE LAW IT MEASURES (physics; the spec's reader B §6): under still lights and geometry every cache value is a function
// of the world point alone; the eye's motion changes WHICH points are visible, never their values. So for a pixel whose
// surface was visible before the camera stopped, the only change allowed after the stop is noise decreasing below one
// 8-bit code; a disoccluded pixel may converge within the gather's settle. A picture that converges after the camera
// stops is a DEFECT, and this door says how long, where and why.
//
// ONE IMPLEMENTATION, TWO HOSTS: jahlab's `stillness` verb and Studio's `world.stillness` drive the same run. The host
// owns its camera (a document camera the mirror pushes) and its frames; the door owns everything else — the move
// script, the per-frame readings and the report:
//
//     StillnessRun run(engine, scene, view);
//     run.begin(spec, err);                       // the script parsed, the monitor and the two readback arms on
//     Vec3 eye, target;
//     while (run.nextPose(eye, target)) {         // the pose of the NEXT frame
//         <put the camera at eye -> target>; <render ONE frame, lockstep>;
//         run.frameDone(err);                     // that frame's picture, GI rows, gather rest, history counts
//     }
//     run.finish(report, err);                    // the monitor flushed, the arms restored, the report whole
//
// WHAT A FRAME OF THE WINDOW READS (the spec's (i)-(iv)): (i) the per-pixel |change| from the previous frame — the max
// over R, G, B in 8-bit codes, on the pixels VISIBLE AT THE STOP, per material class (max and mean); (ii) which GI cache
// rows of the frame monitor fired that frame (`vct.*`, `ifd.*`, `cards.*`, `rq.blas*`: every unit of cache work that
// changes what a pixel reads) and their units; (iii) from those, the frames until the max change stays under one code —
// measured from the stop AND from the last GI row (the two origins); (iv) the gather's rest counters and the reflection
// history's count histogram per class.
//
// "VISIBLE AT THE STOP" IS THE GATHER'S OWN TEST (never a second implementation of it): the pixel-history count the
// integrate wrote on the stop frame (GatherStatus::historyCount, the arm "gather.historyReadback") — a pixel whose
// history the gather carried through the last moves for at least `visibleAge - 1` frames. With no gather running every
// pixel counts (said in the report).
//
// PATH-INDEPENDENCE (`spec.path`): after the first window the door teleports the camera far away (beyond every cascade's
// box) and back to the same final pose, then measures a second window — the pose reached by a WALK (the script) and by
// a TELEPORT. AT EQUAL PLACEMENTS (the spec's rev-4 correction 3: a cascade's placement may depend on the path — the
// lattice and its hysteresis are an extent rule — its values may not): the return re-places every cascade and the
// irradiance field's window where the walk left them (Scene::setGiPlacementPin with the walk's recorded
// CascadeStatus::builtCam and GiStatus::ifdMin). The two windows' last pictures (per class: max, mean and the 99th
// percentile of |diff| in codes) and each cascade's light bytes (the DIRECT digest and the total, GiVoxelStats), its
// placement and the field's origin are compared.
//
// THE MOVE SCRIPT (one word or one line per segment; segments separated by '/', ';' or a newline):
//     teleport:x,y,z,tx,ty,tz      one frame at that pose (eye x,y,z looking at tx,ty,tz, metres)
//     walk:x,y,z,tx,ty,tz:N        N frames from the current pose to that one, eye and target linearly; the last frame
//                                  is exactly the pose
//     hold:N                       N frames at the current pose
// The first segment must place the camera (a teleport). The STOP is the script's last frame: the camera's arrival at
// its final pose. The window is `frames` more frames at that pose; window frame 0 is the stop frame.
//
// Frames, never wall time (the engine's fixed 1/60 s grid). Nothing here changes a picture: the monitor and the two
// readback arms are instruments.
#pragma once

#include "jahshaka/engine/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jahshaka {
namespace engine {

class Engine;
class Scene;
class View;

/// One segment of a move script.
struct StillnessMove {
    enum class Kind { Teleport, Walk, Hold };
    Kind kind = Kind::Hold;
    Vec3 eye, target;
    unsigned frames = 1;
};

/// Parses a move script (the grammar above). False with the reason on a malformed segment or a script that never
/// places the camera.
bool parseStillnessScript(const std::string &text, std::vector<StillnessMove> &out, std::string &err);
/// The camera's final pose under a parsed script (the stop's pose).
bool stillnessFinalPose(const std::vector<StillnessMove> &moves, Vec3 &eye, Vec3 &target);

struct StillnessSpec {
    std::string script;            ///< the move (the grammar above)
    unsigned    frames = 120;      ///< the window: frames measured after the stop
    /// ...and the TELEPORT window's own (PHOTON-7 stage 10-3: the path's window takes as many frames as its law needs to
    /// be observed; 0 = `frames`)
    unsigned    teleportFrames = 0;
    /// THE CARDS' DEMAND SETTLE S of the scene's tier (GiQualityFacts::cardDemandSettleFrames), written into every
    /// frame's record for a law after a stop (stage 5's rule (a): a reader's stamp within S of the change is
    /// legitimate demand). The host that set the tier passes it; 0 = not known (the record says 0).
    unsigned    cardsSettleFrames = 0;
    bool        path = false;      ///< + the teleport window and the walk-vs-teleport comparison
    /// THE MOVE'S TRAIL (PHOTON-2 1d): the last `moveTrail` frames before each stop measured too — every pixel (the
    /// eye moves, so no surface is "the same"), the change from the frame before, the GI rows and the ray levels the
    /// frame re-fitted — so a step DURING the move (a ray-level band crossing, a commit) is seen at its frame. 0 = none.
    unsigned    moveTrail = 0;
    /// A pixel is VISIBLE AT THE STOP when the gather's history count on the stop frame is at least this (the count
    /// is 1 on a frame the history was refused, n when it carried n - 1 frames; 10 = a full history at the shipped
    /// kHistoryFrames). At most 15 (the packed count's bits).
    unsigned    visibleAge = 10;
    /// THE GATHER'S CLASS RESTARTS (PHOTON-2 K5): the gather's history readback on from the first trail frame (or
    /// the stop) to the end, and every frame's StillnessFrame::classPixels / classRestarts from it. Off = the
    /// readback only until the masks are known.
    bool        classRestarts = false;
    /// THE MOVING WINDOW'S RAW MATERIAL (PHOTON-2 round 2, M1): a directory - every trail frame's picture
    /// (<out>-t<NNN>.rgb, RGB8), the gather's own per-pixel view distance and surface test of that frame (.dist
    /// float32 linear depth along the view axis; .flg u8: bit 0 = the history was accepted - the pixel was visible
    /// the frame before by the gather's own test - bit 1 = the foliage class), its material classes (.cls u8, when
    /// classesAt is set) and every trail frame's camera (<out>-cams.txt) - what the motion's residual (this frame
    /// against the frame before reprojected) is computed from, offline. Implies the history readback over the trail.
    std::string dumpDir;
    std::string dumpName;
    /// ...THE PICTURES ALONE (PHOTON-7 stage 5, the walk's time): only the trail frames' .rgb and the cams file are
    /// written - no .dist / .flg / .gsize / .cls (a walk compared byte for byte - gi.walk's L1 - reads nothing else) - and
    /// the gather's history is NOT read back: so L1 also proves the readback leaves every picture as it was.
    bool        dumpRgbOnly = false;
    float       fovYDegrees = 45.0f;   ///< the view's vertical field of view (the cams file's)
    /// THE FROZEN WALK (M1's baseline, PHOTON-2 round 4 - the read's P2-1): the run switches the arm "gi.freeze" on
    /// at the script's FIRST WALK FRAME, and only from a CONVERGED state - on the frame before, the gather HELD (its
    /// rest past the settle: the answer is the true rest mean) and the reflection HELD where one traces (every texel
    /// stopped on its rest mean) - else the run fails with the reason (settle longer before it); the arm is restored
    /// at the end. The walk then measures the motion's own resampling of the
    /// converged picture, which the live walk's residual less is the GI's stepping.
    bool        freezeWalk = false;
    /// ...the arm's value for it: 1 = THE GI FROZEN (no cache work, the gather's and the reflections' histories kept: the
    /// motion's own resampling); 2 = THE WORLD HELD (no cache work, the gather held, the REFLECTIONS LIVE: the reflection's
    /// own estimator and denoiser, no hit radiance changing - PHOTON-7 stage 6, rtreflect.walk_world).
    double      freezeValue = 1.0;
    /// The material classes at a pose (the same encoding as `classes`), for the dump's per-frame .cls.
    std::function<std::vector<unsigned char>(const Vec3 &eye, const Vec3 &target, unsigned w, unsigned h)> classesAt;
    /// THE MATERIAL CLASSES at the stop pose, per pixel of the view (row-major, classW x classH; resampled nearest
    /// when the picture's size differs): 0 = unclassified, k = classNames[k - 1]. Empty = one class "all".
    std::vector<std::string>   classNames;
    std::vector<unsigned char> classes;
    unsigned classW = 0, classH = 0;
};

/// A class's reading on one frame.
struct StillnessClassFrame {
    unsigned pixels = 0;           ///< the class's pixels the mask counts (visible at the stop; every one after a teleport)
    unsigned maxCodes = 0;         ///< the largest per-pixel change from the previous frame, 8-bit codes (max of R, G, B)
    double   meanCodes = 0.0;      ///< ...its mean over those pixels
    unsigned changed = 0;          ///< ...how many changed by one code or more
    /// THE REFLECTION HISTORY'S COUNT over the class's counted pixels (the arm "reflect.historyReadback"; -1 = no
    /// copy of this frame came back): no answer yet (0), <= 9 (the warm seed: just restarted), <= 17, <= 33, > 33.
    int      reflectBins[5] = { -1, -1, -1, -1, -1 };
};

/// One frame of a window.
struct StillnessFrame {
    unsigned           index = 0;          ///< frames after the stop (0 = the stop frame itself)
    unsigned long long engineFrame = 0;    ///< Engine::published().frame after it
    /// [0] = every counted pixel; [k] = class k (StillnessReport::classNames[k - 1]).
    std::vector<StillnessClassFrame> classes;
    /// The GI cache rows the frame monitor filed for this frame: "name:units" joined by ',' (empty: none fired).
    std::string        giRows;
    unsigned           giRowCount = 0;
    /// The gather's rest (GatherStatus): consecutive rest frames, frames since its last restart, the settle N,
    /// and whether the view HELD this frame (restFrames > settleFrames: nothing dispatched).
    unsigned           restFrames = 0, sinceRestart = 0, settleFrames = 0;
    /// THE HOLD WAITED ON THIS FRAME (PHOTON-2 round 2, L7): the gather's rest key moved, or its inputs owed work, or
    /// (PHOTON-7 stage 10, R3's contract) the surface cache owed the readers their hold (CardCacheStatus::readersHoldOwed).
    bool               holdKey = false;
    /// THE FIELD'S GRANT AND ITS SPEND this frame (PHOTON-7 stage 5, 5-4; GiStatus::ifdGrantLastFrame /
    /// ifdDispatchedLastFrame, integrations), the restarts and refinements it owes after it, and the voxel store's own
    /// owed work (GiStatus::chainOwed: the store not at rest).
    unsigned           ifdGrant = 0, ifdSpent = 0, ifdOwedRestarts = 0, ifdOwedRefines = 0;
    unsigned long long ifdOwedIntegrations = 0;
    unsigned long long ifdMarkedByMoves = 0;   ///< the field's probes marked by the accumulation so far (cumulative)
    unsigned ifdReadUnheld = 0;                ///< the field's read region's empty probes this frame (GiStatus::ifdReadUnheld)
    /// PHOTON-7 stage 5, 5-5 (what a law after a stop attributes a row to): the field's window moves so far (cumulative,
    /// GiStatus::ifdFollows), its restart depth and marks' lag; the cards admitted so far, the newest face mark of the
    /// last update's arrivals, the cache's frame, and the demand read-back's period and lag (CardCacheStatus).
    unsigned long long ifdFollows = 0, cardsAdmitted = 0, cardsArrivalMark = 0, cardsFrame = 0;
    unsigned ifdRestartDepth = 0, ifdMarkLag = 0, cardsDemandEvery = 0, cardsDemandLag = 0;
    /// ...and the cards' budgets a frame from the tier table (GiQualityFacts::cardBudgetTexels, cardIndirectTexels, as
    /// CardCacheStatus publishes them) and the page's side: what a law follows an arrival's cards through - its captures
    /// at the capture budget, their indirect at the indirect budget (the photon-7r merge's judge, owed_rows.py).
    unsigned cardsBudgetTexels = 0, cardsIndirectTexels = 0, cardsPageSize = 0;
    /// ...and the demand's settle S (GiQualityFacts::cardDemandSettleFrames, the spec's: the host that set the tier
    /// passes it) - a reader's stamp within S of the last change admits legitimately (stage 5's rule (a)).
    unsigned cardsSettleFrames = 0;
    /// THE ROOTS OF THE FRAME'S CARD WORK (PHOTON-7 stage 10, R3 (c): each round names its parent move;
    /// CardCacheStatus::workRoots): "<kind><cache frame>" joined by ';' ("-" none), the deepest round, the cache's rest
    /// decisions so far (R3 (b): one a demand change, at most) and its window drains (one a change, at most).
    std::string cardsRoots;
    unsigned    cardsRound = 0;
    unsigned long long cardsDecisions = 0, cardsWindowDrains = 0;
    /// ...and the frame's indirect relight in texels (CardCacheStatus::indirectTexelsLastFrame: the pace the L7 count
    /// bounds - lab.stillness_control's pace law reads a round's pages from it)
    unsigned cardsIndirectRelit = 0;
    /// THE READERS' THRESHOLD's counts (PHOTON-7 stage 10, CardCacheStatus: R3 (a) applied to every reader): the cards
    /// relit and captured so far, the relight frames read back VISIBLE (a texel moved a code since its last report; the
    /// unmeasured ones inside), those read back SUB-QUANTUM, and the newest reading's largest move in codes (-1: not
    /// finite) - lab.stillness_control's every-restart-has-a-cause law reads them
    unsigned long long cardsRelights = 0, cardsCaptures = 0, cardsVisible = 0, cardsSubQuantum = 0, cardsUnmeasured = 0;
    /// ...and the cache's reports never read for want of a ring slot (CardCacheStatus::reportsDropped): a defect if
    /// seen at all (lab.stillness_control's law)
    unsigned long long cardsReportsDropped = 0;
    /// ...and THE DEMAND WINDOW's INPUTS as the frame's plan read them (PHOTON-7 stage 10-3, the exact judge;
    /// CardCacheStatus::demandChangeFrame, demandFirst, demandNewest, demandOpenLag): owed_rows.py applies
    /// SurfaceCache::demandWindowOpen's one formula to them, never a stand-in
    unsigned long long cardsChange = 0, cardsFirst[2] = { 0, 0 }, cardsNewest[2] = { 0, 0 };
    unsigned cardsOpenLag = 0;
    float cardsMoveCodes = 0.0f;
    bool               storeOwed = false;
    /// THE FOLIAGE RULE'S OWN ACCEPTS that reached the host this frame (GatherStatus::foliageRuleAccepts' step; PHOTON-2
    /// L8: a few frames late, so a window's SUM is the reading).
    unsigned long long foliageRule = 0;
    /// THE VIEW HELD (PHOTON-7 stage 10): every reader held - the gather (GatherStatus::held) and, where the reflection
    /// traces, the reflection (reflHeld below). The door's hold frame (StillnessWindow::holdFrame) is the first such.
    bool               held = false;
    /// ...and THE GATHER's OWN HOLD (GatherStatus::held, the running gather's): its settle law reads it apart from the
    /// view's (StillnessWindow::gatherHoldFrame).
    bool               gatherHeld = false;
    /// THE REFLECTION'S REST this frame (RayQueryStatus, PHOTON-2 round 2 R1/G6): its rest frames (0 = moving or
    /// broken), held, the newest count of its texels still converging (-1 = none read for this rest) and the rest
    /// frames so far on which a moved instance crossed it.
    unsigned           reflRestFrames = 0, reflRestMovers = 0;
    /// THE CAMERA CUTS so far (PHOTON-2 round 2, N3; GatherStatus::cameraCuts, RayQueryStatus::reflectCameraCuts): the
    /// frames the gather's / the reflection's view detected a cut on (a view axis turned > 75 deg or the eye moved
    /// > 100 m) and restarted its histories.
    unsigned           cameraCuts = 0, reflCuts = 0;
    /// THE FRAME'S GPU ROWS (M1 + N1): the monitor rows over 0.05 ms, "row:ms" joined by ',' ("" = no record) - the
    /// walking cost per cache (vct.scrollN, vct.cascadeN, vct.light.settle ...).
    std::string        gpuRows;
    bool               reflHeld = false;
    int                reflRestActive = -1;
    /// THE REST'S CAPPED TEXELS (commit 8, RayQueryStatus::reflectRestCapped): stopped at their count's ceiling, not by
    /// their standard error - 0 on a healthy rest, -1 none read.
    int                reflRestCapped = -1;
    /// THE SOURCES THE REFLECTION RAN WITH (RayQueryStatus::reflectSources: the mode label) - -1 none traced.
    int                reflSources = -1;
    /// THE FRAME'S COST (PHOTON-2 stage 2's round, H4 (e); THE FRAME CLOCK, PHOTON-7 stage 0): the monitor record's BUSY
    /// frame GPU milliseconds (its span less the gaps the GPU waited on a submission - THE one per-frame GPU time the
    /// hitch law reads) and the whole renderOneFrame's wall milliseconds - the CPU's frame, which in a lockstep host
    /// never contains the GPU's: the GPU's frame is waited for where the host reads it (readMs), outside the call
    /// (-1: unmeasured).
    float              frameGpuMs = -1.0f, frameMs = -1.0f;
    /// THE FRAME CLOCK'S PARTS (PHOTON-7 stage 0, AB §2's question): the host's period (wall milliseconds from the
    /// previous frame's frameDone to this one's: the render, the door's read and its dump of the frame before), the
    /// door's own read of this frame's picture (readPixels waits for the GPU to finish the frame), and from the record:
    /// the GPU span (first command to last, frameGpuMs before the idle is taken out), the GPU's idle gaps inside it, the
    /// gaps no row names, the top-level passes' sum (MonitorRecord::gpuMs) and the cache rows' sum (the door's gpuRows,
    /// summed: they nest inside passes, so never add them to the passes). -1 = unmeasured.
    float              periodMs = -1.0f, readMs = -1.0f;
    float              gpuSpanMs = -1.0f, gpuIdleMs = -1.0f, gpuUnattributedMs = -1.0f, gpuPassesMs = -1.0f,
                       gpuRowsMs = -1.0f;
    /// THE FRAME'S SLOWEST CPU STAGE (PHOTON-2 round 2, the hitch law): the monitor record's longest stage, "name:ms"
    /// ("-": no record) - where a wall-clock hitch spent its time.
    std::string        slowStage;
    /// THE FOLIAGE CLASS'S RESTARTS (StillnessSpec::classRestarts; PHOTON-2 K5): over the pixels the gather wrote as
    /// foliage this frame (its history word's class bit - the cut-out class, written under the class-off arm too),
    /// how many, and how many of them restarted their history (count 1: refused or absent). -1 = no copy of this
    /// frame came back (or the gather held: a held frame writes no history).
    int                classPixels = -1, classRestarts = -1;
    /// THE STEPS (PHOTON-2 J1; a trail frame's): pixels whose change from the frame before exceeds by more than
    /// kStillnessStepCodes the largest change its 3x3 neighbourhood showed the frame before - a jump the motion around
    /// it does not explain (a field's entering planes landing in one frame). -1 = not measured (the trail's first
    /// two frames, a window frame).
    int                stepPixels = -1;
};
/// The margin of StillnessFrame::stepPixels, in 8-bit codes.
constexpr unsigned kStillnessStepCodes = 4u;

/// What one window measured.
struct StillnessWindow {
    std::string name;                      ///< "walk" (the script) or "teleport" (the path check's)
    Vec3        eye, target;               ///< the stop's pose
    bool        visibleFromGather = false; ///< the mask is the gather's history count (false: every pixel)
    std::vector<StillnessFrame> frames;
    /// THE MOVE'S TRAIL (StillnessSpec::moveTrail): the frames before the stop, oldest first, `index` counting down
    /// to 1 (the frame just before the stop); every pixel counted.
    std::vector<StillnessFrame> trail;
    /// THE TWO ORIGINS: the stop is frame 0; the last frame with a GI row (-1: none fired after the stop).
    int lastGiRowFrame = -1;
    /// Per class ([0] = all): the last window frame whose max change was >= 1 code (0: still from frame 1 on; the
    /// window's length: still changing at its end), and the same counted after the last GI row.
    std::vector<int> settleFromStop;
    std::vector<int> settleFromLastGiRow;
    std::vector<unsigned> visiblePixels;   ///< per class: the counted pixels
    int holdFrame = -1;                           ///< the first window frame the VIEW held - every reader (StillnessFrame::held; -1: never)
    /// ...and each reader's own first held frame (-1: never): the gather's (StillnessFrame::gatherHeld) and the
    /// reflection's (reflHeld) - the view's hold is at or after both.
    int gatherHoldFrame = -1, reflHoldFrame = -1;
    /// ...and the last window frame the hold waited on (StillnessFrame::holdKey; -1: none after the stop) - the bars'
    /// "last input write", rows or none.
    int lastHoldKeyFrame = -1;
    /// THE HOLD's DERIVED BOUND (PHOTON-7 stage 10, R3's design; ONE formula, stillnessDeriveLiveness - lab.stillness_control
    /// and scripting.e2e.world_stillness both read it): the readers' FIRST REST frame (rest frames >= 1), the stop's one
    /// rest DECISION's frame (the cache's decisions moving) and its WORK's end (its last GI row), and the first hold's
    /// bound from the first rest: (N + due) + due + W + N + 1 - the readers' demand span (N + the due), the read-back's
    /// due after it, W = the decision's work (workEnd - decision), the re-rest after it, the hold's own frame; W's PACE:
    /// ceil(the work's pages / the tier's pages a frame) + due. -1 where the window has no such frame.
    int firstRestFrame = -1, decisionFrame = -1, decisionWorkEnd = -1, holdBound = -1, workPace = -1;
    double workPages = 0.0;
    /// THE MOVE BEFORE THE STOP: the GI rows that fired on its frames ("name:frames/units", ',' joined) and how
    /// many frames before the stop the last of them landed (-1: none) — a commit that lands inside the move is the
    /// origin of every debt the window then pays.
    std::string moveRows;
    int         lastMoveRowAgo = -1;
    /// THE HEAT: per pixel of the picture (row-major), the largest change in codes over the window's frames 1..N
    /// (`heat`) and over its last quarter (`heatLate`: what still changes at the end), on the counted pixels only.
    std::vector<unsigned char> heat, heatLate;
    unsigned heatW = 0, heatH = 0;
    /// THE WINDOW'S LAST PICTURE and each cascade's light at its end (GiVoxelStats; empty when unreadable).
    Image                    lastPicture;
    std::vector<std::string> directDigest, lightDigest;
    std::vector<Vec3>        cascadeCentre;
    std::vector<Vec3>        cascadeBuiltCam;   ///< the placements' cameras (the pin the teleport window takes)
    Vec3                     fieldOrigin;       ///< the irradiance field's window (GiStatus::ifdMin)
};

/// THE HOLD's DERIVED BOUND, ONE FORMULA (Stillness.cpp says the terms): fills the window's firstRestFrame, decisionFrame,
/// decisionWorkEnd, holdBound, workPace and workPages from its frames. The door calls it on every window.
void stillnessDeriveLiveness(StillnessWindow &w);

/// The run's report.
struct StillnessReport {
    std::vector<std::string>   classNames;   ///< the classes, [k - 1] for column k
    std::vector<StillnessWindow> windows;    ///< "walk", then "teleport" when the spec asked for the path
    /// THE PATH CHECK (both windows): the last pictures' largest per-pixel difference in codes, how many pixels
    /// differ by one code or more, per class ([0] = all, over the class's pixels whatever their visibility); each
    /// cascade's DIRECT digest, total digest and placement equal under the two paths.
    bool                  pathChecked = false;
    std::vector<unsigned> pathMaxCodes;
    std::vector<unsigned> pathPixelsDiffering;
    std::vector<double>   pathMeanCodes;        ///< per class ([0] = all): the mean |diff| over the class's pixels
    std::vector<unsigned> pathP99Codes;         ///< ...and its 99th percentile
    int                   pathFieldEqual = -1;  ///< the field's origin equal under the two paths (1), not (0), none (-1)
    std::vector<int>      pathDirectEqual, pathLightEqual, pathCentreEqual;   ///< per cascade: 1 equal, 0 not, -1 unread
    /// THE FROZEN WALK (StillnessSpec::freezeWalk): the engine frame the freeze took effect on (0: not asked) and the
    /// gather's rest frames / settle and the reflection's rest frames on the frame before it (the converged state it
    /// froze).
    unsigned long long    freezeEngineFrame = 0;
    unsigned              freezeRestFrames = 0, freezeSettleFrames = 0, freezeReflRestFrames = 0;
    /// THE COST, from the same records: per monitor row whose GPU time was measured (the ray tier's `rq.*`, the
    /// gather's `gather.*`, the cache rows), the mean GPU milliseconds over the frames it ran and how many — the
    /// whole run's (the move, the windows), a frame being one row's sum.
    std::vector<std::pair<std::string, std::pair<double, unsigned>>> gpuMs;
    /// One TSV: a header line and a row per window frame.
    std::string tsv() const;
    /// The one-line-per-window summary (plus the path line), each starting "STILLNESS".
    std::string summary() const;
};

class StillnessRun {
public:
    StillnessRun(Engine &engine, Scene &scene, View &view);
    ~StillnessRun();
    StillnessRun(const StillnessRun &) = delete;
    StillnessRun &operator=(const StillnessRun &) = delete;

    /// Parses the script, plans the frames, opens its monitor tee and switches the frame monitor on when it is off (a
    /// host's running capture is left as it is: the tee reads a copy of every record) and the readback arms. False
    /// with the reason.
    bool begin(const StillnessSpec &spec, std::string &err);
    /// The pose of the NEXT frame to render; false when every planned frame has been rendered.
    bool nextPose(Vec3 &eye, Vec3 &target);
    /// After the host rendered the frame nextPose() gave: reads it.
    bool frameDone(std::string &err);
    /// Flushes the monitor, restores it and the arms, completes the report. Also run by the destructor (the report
    /// then discarded) so an abandoned run leaves the engine as it found it.
    bool finish(StillnessReport &out, std::string &err);

    /// Frames the plan holds in all (the move, the windows, the path's bridge).
    unsigned plannedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}   // namespace engine
}   // namespace jahshaka
