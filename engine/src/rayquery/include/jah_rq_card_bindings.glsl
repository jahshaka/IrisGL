// THE CARD READ'S FOUR BINDINGS (PHOTON-CARDS-2), declared once for the two
// jobs that read cards through jah_rq_card.glsl — the reflection trace
// (rq_reflect.comp) and gi.card_read_parity's harness (rq_card_parity.comp) —
// at consecutive bindings of set 0 from JAH_CARD_BINDING_BASE, which the caller
// defines first. The host binds SurfaceCache's two tables and its Depth and
// Radiance layers there (OgreRayQuery.cpp), or stand-ins with zero slots when
// the scene holds no cache.
//
// THE VIEW TERM'S FIVE (PHOTON-CARDS-5, jah_card_view.glsl) at consecutive
// bindings from JAH_CARD_VIEW_BINDING_BASE, which the caller also defines (each
// job appends them after its own last binding): the Indirect, Emissive,
// ShadowRough, Albedo and Normal layers, in SurfaceCache::viewLayers' order.
//
// Include this file, then define JAH_CARD_SLOTS / JAH_CARD_RECORDS, then
// include jah_rq_card.glsl.

#ifndef JAH_RQ_CARD_BINDINGS_GLSL
#define JAH_RQ_CARD_BINDINGS_GLSL

#ifndef JAH_CARD_BINDING_BASE
#error "define JAH_CARD_BINDING_BASE before including jah_rq_card_bindings.glsl"
#endif
#ifndef JAH_CARD_VIEW_BINDING_BASE
#error "define JAH_CARD_VIEW_BINDING_BASE before including jah_rq_card_bindings.glsl"
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

layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE ) uniform sampler2D jahCardIndirectLayer;
layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE + 1 ) uniform sampler2D jahCardEmissiveLayer;
layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE + 2 ) uniform sampler2D jahCardShadowRoughLayer;
layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE + 3 ) uniform sampler2D jahCardAlbedoLayer;
layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE + 4 ) uniform sampler2D jahCardNormalLayer;
#define JAH_CARD_INDIRECT( t ) texelFetch( jahCardIndirectLayer, t, 0 ).xyz
#define JAH_CARD_EMISSIVE( t ) texelFetch( jahCardEmissiveLayer, t, 0 ).xyz
#define JAH_CARD_SHADOW_ROUGH( t ) texelFetch( jahCardShadowRoughLayer, t, 0 ).xy
#define JAH_CARD_ALBEDO( t ) texelFetch( jahCardAlbedoLayer, t, 0 )
#define JAH_CARD_NORMAL( t ) texelFetch( jahCardNormalLayer, t, 0 )

#endif   // JAH_RQ_CARD_BINDINGS_GLSL
