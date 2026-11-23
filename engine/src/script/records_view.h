#pragma once
// THE VIEW FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_view.cpp. The camera (engine.view.setCamera) and the post chain
// (engine.view.setPostFx; 2d's ViewFrames carries one as it drew) - the post chain with its two nested records.
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Camera.h"
#include "jahshaka/engine/PostFx.h"

namespace jahshaka { namespace engine { namespace script {

// (LookKind::Count is no look - the host's range bound - so it has no name: a script cannot ask for it)
JAH_ENUM_NAMES(LookKind, {JAH_ENUMERATOR(LookKind, Desaturate), JAH_ENUMERATOR(LookKind, GlassWarp),
                          JAH_ENUMERATOR(LookKind, RadialBlur), JAH_ENUMERATOR(LookKind, OldMovie),
                          JAH_ENUMERATOR(LookKind, Posterize), JAH_ENUMERATOR(LookKind, Sharpen)});
JAH_ENUM_NAMES(ExposureMeterPattern, {JAH_ENUMERATOR(ExposureMeterPattern, Average),
                                      JAH_ENUMERATOR(ExposureMeterPattern, CentreWeighted),
                                      JAH_ENUMERATOR(ExposureMeterPattern, Spot)});
JAH_ENUM_NAMES(FrameCaptureState, {JAH_ENUMERATOR(FrameCaptureState, Idle), JAH_ENUMERATOR(FrameCaptureState, Armed),
                                   JAH_ENUMERATOR(FrameCaptureState, InFlight), JAH_ENUMERATOR(FrameCaptureState, Ready)});

JAH_RECORD_COUNT(CameraDesc, 12);
JAH_RECORD_COUNT(LookDesc, 2);
JAH_RECORD_COUNT(ImageGrade, 12);
JAH_RECORD_COUNT(PostFxDesc, 35);

}}}  // namespace jahshaka::engine::script
