// THE CARD READ'S FOUR BINDINGS (PHOTON-CARDS-2), declared once for the two
// jobs that read cards through jah_rq_card.glsl — the reflection trace
// (rq_reflect.comp) and gi.card_read_parity's harness (rq_card_parity.comp) —
// at consecutive bindings of set 0 from JAH_CARD_BINDING_BASE, which the caller
// defines first. The host binds SurfaceCache's two tables and its Depth and
// Radiance layers there (OgreRayQuery.cpp), or stand-ins with zero slots when
// the scene holds no cache.
//
// Include this file, then define JAH_CARD_SLOTS / JAH_CARD_RECORDS, then
// include jah_rq_card.glsl.

#ifndef JAH_RQ_CARD_BINDINGS_GLSL
#define JAH_RQ_CARD_BINDINGS_GLSL

#ifndef JAH_CARD_BINDING_BASE
#error "define JAH_CARD_BINDING_BASE before including jah_rq_card_bindings.glsl"
#endif

struct JahCardRecordGpu
{
	vec4 rowU;
	vec4 rowV;
	vec4 rowD;
	vec4 axis;
	uvec4 atlas;
};
layout( std430, set = 0, binding = JAH_CARD_BINDING_BASE ) readonly buffer JahCardTable
{
	JahCardRecordGpu jahCards[];
};
layout( std430, set = 0, binding = JAH_CARD_BINDING_BASE + 1 ) readonly buffer JahCardInstances
{
	uvec4 jahCardInstances[];
};
layout( set = 0, binding = JAH_CARD_BINDING_BASE + 2 ) uniform sampler2D jahCardDepthLayer;
layout( set = 0, binding = JAH_CARD_BINDING_BASE + 3 ) uniform sampler2D jahCardRadianceLayer;

#define JAH_CARD_INSTANCE( slot ) jahCardInstances[slot]
#define JAH_CARD_DEPTH( t ) texelFetch( jahCardDepthLayer, t, 0 ).x
#define JAH_CARD_RADIANCE( t ) texelFetch( jahCardRadianceLayer, t, 0 ).xyz

#endif   // JAH_RQ_CARD_BINDINGS_GLSL
