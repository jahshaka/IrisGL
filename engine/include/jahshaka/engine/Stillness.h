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
    /// The ray tier's per-instance levels this frame re-fitted (GpuSceneStatus::rayLevelRefits' step).
    unsigned           rayLevelRefits = 0;
    bool               held = false;
    /// THE FRAME'S COST (PHOTON-2 stage 2's round, H4 (e)): the monitor record's BUSY frame GPU milliseconds (its span
    /// less the gaps the GPU waited on a submission) and the whole renderOneFrame's wall milliseconds (-1: unmeasured).
    float              frameGpuMs = -1.0f, frameMs = -1.0f;
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
    int holdFrame = -1;                    ///< the first window frame the gather HELD (-1: never)
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

    /// Parses the script, plans the frames, switches the frame monitor on (refused while a capture already runs: its
    /// records would be taken from it) and the readback arms. False with the reason.
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
