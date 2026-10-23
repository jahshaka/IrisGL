// THE CARD READ'S FOUR BINDINGS (PHOTON-CARDS-2), declared once for the two
// jobs that read cards through jah_rq_card.glsl — the reflection trace
// (rq_reflect.comp) and gi.card_read_parity's harness (rq_card_parity.comp) —
// at consecutive bindings of set 0 from JAH_CARD_BINDING_BASE, which the caller
// defines first. The host binds SurfaceCache's two tables and its Depth and
// Radiance layers there (OgreRayQuery.cpp), or stand-ins with zero slots when
// the scene holds no cache.
//
// THE VIEW TERM'S SEVEN (PHOTON-CARDS-5, jah_card_view.glsl) at consecutive
// bindings from JAH_CARD_VIEW_BINDING_BASE, which the caller also defines (each
// job appends them after its own last binding): the Indirect (the cached
// diffuse half, or the lobe's share where kD = 0), Flat (the emissive and the specular lobe's re-emission, at the
// emissive's binding - ENGINE-POLISH-PHOTON-1 stage 3), ShadowRough, Albedo and
// Normal layers, the Specular layer (F0 and kS: the movers' gate cuts the
// lobe's indirect share with it, the lights' lobe reads them) and the SunVis layer (the
// sun's visibility the read's specular lobe of the sun takes, PHOTON-3), in
// SurfaceCache::viewLayers' order.
//
// THE INSTANCE TABLE CARRIES THE READERS' DEMAND (PHOTON-7 stage 5, 5-2: residency by what the readers read), after
// its JAH_CARD_SLOTS entries, in uvec4 entries (SurfaceCache::syncBuffers writes the head, the readers the rest):
//   [S]       ( the frame's stamp, the count flag, 0, 0 ) - the host's, uploaded every frame;
//   [S + 1]   the hit counts while the flag is set: ( reflection carded, uncarded, gather carded, uncarded );
//   [S + 2 .. S + 2 + 8192)   per ATLAS CELL (16 texels a side, y x 128 + x) and PER READER (word 2 x cell + reader):
//                             the stamp of the last frame that reader read the card whose corner is that cell;
//   [S + 8194 .. S + 8194 + 4S)   per ITEM SLOT, per reader, per face bucket (the hit's facing axis and its sign: word
//                             16 x slot + 8 x reader + bucket): the stamp of the last frame a hit there found NO card -
//                             the faces the readers want. Per reader because a reader at REST (the reflection held,
//                             the gather held) reads nothing: what it read stays wanted on its own clock.
// A reader defines JAH_CARD_DEMAND (its index: 0 the reflection, 1 the gather) before including jah_rq_card.glsl;
// the parity harness does not, and writes nothing.
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
layout( std430, set = 0, binding = JAH_CARD_BINDING_BASE + 1 ) buffer JahCardInstances
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
// THE SPECULAR LAYER (ENGINE-POLISH-PHOTON-1): F0 and kS in the store's encoding, the sixth.
layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE + 5 ) uniform sampler2D jahCardSpecularLayer;
#define JAH_CARD_SPECULAR( t ) texelFetch( jahCardSpecularLayer, t, 0 )
// THE SUN'S VISIBILITY AT THE TEXEL (PHOTON-3 stage 2), the seventh: the still world's traced term times the
// movers' and the cloud sheet's, as the relight shaded the texel (R8) - the weight of the read's sun lobe.
layout( set = 0, binding = JAH_CARD_VIEW_BINDING_BASE + 6 ) uniform sampler2D jahCardSunVisLayer;
#define JAH_CARD_SUN_VIS( t ) texelFetch( jahCardSunVisLayer, t, 0 ).x

#endif   // JAH_RQ_CARD_BINDINGS_GLSL
