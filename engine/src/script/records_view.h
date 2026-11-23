#pragma once
// THE VIEW FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_view.cpp. The camera (engine.view.setCamera) and the post chain
// (engine.view.setPostFx; 2d's ViewFrames carries one as it drew) - the post chain with its two nested records.
#include "script/records.h"
#include "jahshaka/engine/Camera.h"
#include "jahshaka/engine/PostFx.h"

namespace jahshaka { namespace engine { namespace script {

// (LookKind::Count is no look - the host's range bound - so it has no name: a script cannot ask for it)
JAH_ENUM_NAMES(LookKind, {{LookKind::Desaturate, "desaturate"}, {LookKind::GlassWarp, "glassWarp"},
                          {LookKind::RadialBlur, "radialBlur"}, {LookKind::OldMovie, "oldMovie"},
                          {LookKind::Posterize, "posterize"}, {LookKind::Sharpen, "sharpen"}});
JAH_ENUM_NAMES(ExposureMeterPattern, {{ExposureMeterPattern::Average, "average"},
                                      {ExposureMeterPattern::CentreWeighted, "centreWeighted"},
                                      {ExposureMeterPattern::Spot, "spot"}});
JAH_ENUM_NAMES(FrameCaptureState, {{FrameCaptureState::Idle, "idle"}, {FrameCaptureState::Armed, "armed"},
                                   {FrameCaptureState::InFlight, "inFlight"}, {FrameCaptureState::Ready, "ready"}});

JAH_RECORD_COUNT(CameraDesc, 12);
JAH_RECORD_COUNT(LookDesc, 2);
JAH_RECORD_COUNT(ImageGrade, 12);
JAH_RECORD_COUNT(PostFxDesc, 35);

}}}  // namespace jahshaka::engine::script
