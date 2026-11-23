// THE VIEW FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_view.h declares, every field
// named once in declaration order (records.h). Each declared domain is the one its header states (Camera.h, PostFx.h);
// a number the header bounds by no law is a finite float.
#include "script/records_view.h"

namespace jahshaka { namespace engine { namespace script {

const Table<CameraDesc> &Record<CameraDesc>::table()
{
    static const Table<CameraDesc> t({
        field("position", &CameraDesc::position).in("m"),
        field("orientation", &CameraDesc::orientation),
        field("fovDegrees", &CameraDesc::fovDegrees).range(0, 180).above(0).in("deg"),
        field("nearClip", &CameraDesc::nearClip).above(0).in("m"),
        field("farClip", &CameraDesc::farClip).above(0).in("m"),
        field("orthographic", &CameraDesc::orthographic),
        field("orthoSize", &CameraDesc::orthoSize).above(0).in("m"),
        field("constrainAspect", &CameraDesc::constrainAspect),
        field("aspect", &CameraDesc::aspect).above(0),
        field("framingAspect", &CameraDesc::framingAspect).atLeast(0),
        field("lensShiftX", &CameraDesc::lensShiftX),
        field("lensShiftY", &CameraDesc::lensShiftY),
    });
    return t;
}

const Table<LookDesc> &Record<LookDesc>::table()
{
    static const Table<LookDesc> t({
        field("kind", &LookDesc::kind),
        field("p", &LookDesc::p),
    });
    return t;
}

const Table<ImageGrade> &Record<ImageGrade>::table()
{
    static const Table<ImageGrade> t({
        field("contrast", &ImageGrade::contrast).above(0),
        field("saturation", &ImageGrade::saturation).atLeast(0),
        field("shadows", &ImageGrade::shadows).in("stops"),
        field("highlights", &ImageGrade::highlights).in("stops"),
        field("whiteTemperature", &ImageGrade::whiteTemperature).above(0).in("K"),
        field("whiteTint", &ImageGrade::whiteTint),
        field("vignette", &ImageGrade::vignette).range(0, 1),
        field("filmSlope", &ImageGrade::filmSlope).above(0),
        field("filmToe", &ImageGrade::filmToe).range(0, 1),
        field("filmShoulder", &ImageGrade::filmShoulder).range(0, 1),
        field("filmBlackClip", &ImageGrade::filmBlackClip).range(0, 1),
        field("filmWhiteClip", &ImageGrade::filmWhiteClip).range(0, 1),
    });
    return t;
}

const Table<PostFxDesc> &Record<PostFxDesc>::table()
{
    static const Table<PostFxDesc> t({
        field("hdr", &PostFxDesc::hdr),
        field("exposure", &PostFxDesc::exposure),
        field("exposureMin", &PostFxDesc::exposureMin),
        field("exposureMax", &PostFxDesc::exposureMax),
        field("meterPattern", &PostFxDesc::meterPattern),
        field("meterLowPercent", &PostFxDesc::meterLowPercent).range(0, 100).in("%"),
        field("meterHighPercent", &PostFxDesc::meterHighPercent).range(0, 100).in("%"),
        field("bloom", &PostFxDesc::bloom),
        field("bloomThreshold", &PostFxDesc::bloomThreshold).atLeast(0),
        field("bloomKnee", &PostFxDesc::bloomKnee).atLeast(0),
        field("bloomAmount", &PostFxDesc::bloomAmount).range(0, 2),
        field("ssao", &PostFxDesc::ssao),
        field("ssaoScale", &PostFxDesc::ssaoScale).range(0.5, 1),
        field("ssaoPower", &PostFxDesc::ssaoPower).above(0),
        field("ssaoRadius", &PostFxDesc::ssaoRadius).above(0).in("m"),
        field("ditherOff", &PostFxDesc::ditherOff),
        field("smaaPreset", &PostFxDesc::smaaPreset).range(-1, 3),
        field("ssr", &PostFxDesc::ssr).range(0, 2),
        field("ssrMaxDistance", &PostFxDesc::ssrMaxDistance).above(0).in("m"),
        field("ssrSteps", &PostFxDesc::ssrSteps).range(0, 128),
        field("ssrThickness", &PostFxDesc::ssrThickness).above(0).in("m"),
        field("ssrIntensity", &PostFxDesc::ssrIntensity).atLeast(0),
        field("ssrMarchPhase", &PostFxDesc::ssrMarchPhase).range(0, 2),
        field("reflectionRoughnessCutoff", &PostFxDesc::reflectionRoughnessCutoff).range(0, 1),
        field("refractions", &PostFxDesc::refractions),
        field("distortion", &PostFxDesc::distortion),
        field("distortionStrength", &PostFxDesc::distortionStrength).atLeast(0),
        field("tonemapFixed", &PostFxDesc::tonemapFixed),
        field("exposureScale", &PostFxDesc::exposureScale).atLeast(0),
        field("looks", &PostFxDesc::looks).upTo(unsigned(LookKind::Count)),   // (a kind at most once: PostFx.h LookDesc)
        field("hzb", &PostFxDesc::hzb),
        field("hzbFarthest", &PostFxDesc::hzbFarthest),
        field("allowOffscreen", &PostFxDesc::allowOffscreen),
        field("hdrReadback", &PostFxDesc::hdrReadback),
        field("image", &PostFxDesc::image),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
