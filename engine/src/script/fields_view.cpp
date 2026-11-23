// THE VIEW FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_view.h declares, every field
// named once in declaration order by its own member (JAH_FIELD). EACH DECLARED DOMAIN IS THE RANGE THE ENGINE TAKES AS
// IT IS (the R lane's audit, 7): a value the engine would clamp or round is refused at the call, never changed silently -
// the clamp's file:line beside it. A number the engine takes whole is a finite float. The laws between members (far past
// near, low under high, a look kind once) are the commands' (commands_view.cpp).
#include "script/records_view.h"

namespace jahshaka { namespace engine { namespace script {

const Table<CameraDesc> &Record<CameraDesc>::table()
{
    static const Table<CameraDesc> t({
        JAH_FIELD(CameraDesc, position).in("m"),
        JAH_FIELD(CameraDesc, orientation),   // (a zero quaternion: engine.view.setCamera refuses it)
        JAH_FIELD(CameraDesc, fovDegrees).range(1, 179).in("deg"),   // OgreView.cpp:1429, :1476: clamped to [1, 179]
        JAH_FIELD(CameraDesc, nearClip).atLeast(0.001).in("m"),      // OgreView.cpp:1401: max(near, 0.001)
        JAH_FIELD(CameraDesc, farClip).above(0).in("m"),             // (>= near + 0.01, OgreView.cpp:1402: the command's)
        JAH_FIELD(CameraDesc, orthographic),
        JAH_FIELD(CameraDesc, orthoSize).above(0).in("m"),
        JAH_FIELD(CameraDesc, constrainAspect),
        JAH_FIELD(CameraDesc, aspect).above(0),
        JAH_FIELD(CameraDesc, framingAspect).atLeast(0),
        JAH_FIELD(CameraDesc, lensShiftX),
        JAH_FIELD(CameraDesc, lensShiftY),
    });
    return t;
}

const Table<LookDesc> &Record<LookDesc>::table()
{
    static const Table<LookDesc> t({
        JAH_FIELD(LookDesc, kind),
        JAH_FIELD(LookDesc, p),
    });
    return t;
}

const Table<ImageGrade> &Record<ImageGrade>::table()
{
    static const Table<ImageGrade> t({
        JAH_FIELD(ImageGrade, contrast).above(0),
        JAH_FIELD(ImageGrade, saturation).atLeast(0),
        JAH_FIELD(ImageGrade, shadows).in("stops"),
        JAH_FIELD(ImageGrade, highlights).in("stops"),
        JAH_FIELD(ImageGrade, whiteTemperature).range(1000, 40000).in("K"),   // OgreChain.cpp:4210: clamped to it
        JAH_FIELD(ImageGrade, whiteTint),
        JAH_FIELD(ImageGrade, vignette).range(0, 1),
        JAH_FIELD(ImageGrade, filmSlope).atLeast(0.01),   // FinalToneMapping_ps.glsl:274: max(slope, 0.01)
        JAH_FIELD(ImageGrade, filmToe).range(0, 1),
        JAH_FIELD(ImageGrade, filmShoulder).range(0, 1),
        JAH_FIELD(ImageGrade, filmBlackClip).range(0, 1),
        JAH_FIELD(ImageGrade, filmWhiteClip).range(0, 1),
    });
    return t;
}

const Table<PostFxDesc> &Record<PostFxDesc>::table()
{
    static const Table<PostFxDesc> t({
        JAH_FIELD(PostFxDesc, hdr),
        JAH_FIELD(PostFxDesc, exposure),
        JAH_FIELD(PostFxDesc, exposureMin),   // (<= exposureMax: the command's)
        JAH_FIELD(PostFxDesc, exposureMax),
        JAH_FIELD(PostFxDesc, meterPattern),
        JAH_FIELD(PostFxDesc, meterLowPercent).range(0, 100).in("%"),   // (<= meterHighPercent: the command's)
        JAH_FIELD(PostFxDesc, meterHighPercent).range(0, 100).in("%"),
        JAH_FIELD(PostFxDesc, bloom),
        JAH_FIELD(PostFxDesc, bloomThreshold).atLeast(0),
        JAH_FIELD(PostFxDesc, bloomKnee).atLeast(0),
        JAH_FIELD(PostFxDesc, bloomAmount).range(0, 2),
        JAH_FIELD(PostFxDesc, ssao),
        JAH_FIELD(PostFxDesc, ssaoScale).range(0.5, 1),
        JAH_FIELD(PostFxDesc, ssaoPower).above(0),
        JAH_FIELD(PostFxDesc, ssaoRadius).above(0).in("m"),
        JAH_FIELD(PostFxDesc, ditherOff),   // (a measuring arm: setPostFx refuses it on, engine.view.setPostFxArms sets it)
        JAH_FIELD(PostFxDesc, smaaPreset).range(-1, 3),
        JAH_FIELD(PostFxDesc, ssr).range(0, 2),
        JAH_FIELD(PostFxDesc, ssrMaxDistance).above(0).in("m"),
        JAH_FIELD(PostFxDesc, ssrSteps).range(0, 128),   // (a measuring knob: as ditherOff; 1..7 the arm's command refuses)
        JAH_FIELD(PostFxDesc, ssrThickness).above(0).in("m"),
        JAH_FIELD(PostFxDesc, ssrIntensity).atLeast(0),
        JAH_FIELD(PostFxDesc, ssrMarchPhase).range(0, 2),
        JAH_FIELD(PostFxDesc, reflectionRoughnessCutoff).range(0, 1),
        JAH_FIELD(PostFxDesc, refractions),
        JAH_FIELD(PostFxDesc, distortion),
        JAH_FIELD(PostFxDesc, distortionStrength).atLeast(0),
        JAH_FIELD(PostFxDesc, tonemapFixed),
        JAH_FIELD(PostFxDesc, exposureScale).atLeast(0),
        JAH_FIELD(PostFxDesc, looks).upTo(unsigned(LookKind::Count)),   // (a kind at most once: the command's)
        JAH_FIELD(PostFxDesc, hzb),
        JAH_FIELD(PostFxDesc, hzbFarthest),
        JAH_FIELD(PostFxDesc, allowOffscreen),
        JAH_FIELD(PostFxDesc, hdrReadback),
        JAH_FIELD(PostFxDesc, image),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
