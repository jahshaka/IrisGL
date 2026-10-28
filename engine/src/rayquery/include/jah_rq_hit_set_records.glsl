// THE HIT SET AT THE RECORDS JOBS' NUMBERING (RayQueryTier's kCardGatherHitSlots: the TLAS at 0, the volumes 5-16, the
// sky 17, the card read 18-21 and its view term 22-28, the geometric normal's rows 29-30, the hit record's four 31-34, the
// hit's own surface 35-36) - ONE TEXT for every job that traces records against the hit readers: the cards' gather
// (rq_card_gather.comp) and the blended passes' escapes (rq_glass_escape.comp, PHOTON-7 10-4 R4). The host's half is
// hitSetLayout / HitSetWrite / hitSetTransitions over the same slots. The caller declares `const int kMaxCascades`, its
// Params with `JahHitParams hit` and JAH_HIT_PARAMS (jah_rq_hit_params.glsl) first, and defines, before this file,
// JAH_HIT_LOBE_INTEGRAL (an irradiance estimator reads a hit's specular lobe by its integral) and JAH_CARD_DEMAND (the
// reader whose demand a card read stamps) where it is one.
layout( set = 0, binding = 5 ) uniform sampler3D voxelIso[kMaxCascades];
layout( set = 0, binding = 6 ) uniform sampler3D voxelX[kMaxCascades];
layout( set = 0, binding = 7 ) uniform sampler3D voxelY[kMaxCascades];
layout( set = 0, binding = 8 ) uniform sampler3D voxelZ[kMaxCascades];
layout( set = 0, binding = 9 ) uniform sampler3D voxelCovP[kMaxCascades];
layout( set = 0, binding = 10 ) uniform sampler3D voxelCovN[kMaxCascades];
layout( set = 0, binding = 11 ) uniform sampler3D voxelPosP[kMaxCascades];
layout( set = 0, binding = 12 ) uniform sampler3D voxelPosN[kMaxCascades];
layout( set = 0, binding = 13 ) uniform sampler3D voxelBack[kMaxCascades];
layout( set = 0, binding = 14 ) uniform sampler3D voxelNrm[kMaxCascades];
layout( set = 0, binding = 15 ) uniform sampler3D voxelDirect[kMaxCascades];
#define JAH_VOX_SAMPLE_DIRECT( c, u, l ) textureLod( voxelDirect[c], u, l )
layout( set = 0, binding = 16 ) uniform sampler3D voxelDirectBack[kMaxCascades];
#define JAH_VOX_SAMPLE_DIRECT_BACK( c, u, l ) textureLod( voxelDirectBack[c], u, l )
layout( set = 0, binding = 17 ) uniform samplerCube skyCube;

#include "jah_rq_hit.glsl"

#define JAH_CARD_BINDING_BASE 18
#define JAH_CARD_VIEW_BINDING_BASE 22
#include "jah_rq_card_bindings.glsl"
#include "jah_rq_card.glsl"

layout( std430, set = 0, binding = 29 ) readonly buffer JahGeomRowOfSlot { uint jahGeomRowOfSlot[]; };
layout( std430, set = 0, binding = 30 ) readonly buffer JahGeomRows { uvec4 jahGeomRows[]; };
#define JAH_GEOM_ROW_OF( slot ) jahGeomRowOfSlot[slot]
#define JAH_GEOM_LANE( row, k ) jahGeomRows[( row ) * 3u + ( k )]
#include "jah_rq_geom.glsl"

layout( std430, set = 0, binding = 31 ) readonly buffer JahHitInstances { uvec4 jahHitInstances[]; };
layout( rgba32ui, set = 0, binding = 32 ) uniform writeonly uimage2D jahHitIds;
layout( r32ui, set = 0, binding = 33 ) uniform writeonly uimage2D jahHitDest;
layout( std430, set = 0, binding = 34 ) buffer JahHitBuf { uint jahHitBuf[]; };
#define JAH_HIT_INSTANCE( s, k ) jahHitInstances[( s ) * 10u + ( k )]
#include "jah_rq_sun_ray.glsl"
#include "jah_rq_hit_record.glsl"
layout( set = 0, binding = 35 ) uniform sampler2DArray jahHitPool;
layout( set = 0, binding = 36 ) uniform sampler2D jahHitCloud;
#include "jah_rq_hit_radiance.glsl"
#include "jah_rq_panes.glsl"
// THE CARD READ'S LOBE INTEGRALS (jah_rq_card.glsl: the includer's): the store's table, through the hit table.
void jahCardLobeHemi( float r, out float a, out float b )
{
	a = 0.0;
	b = 0.0;
	const uvec2 t = JAH_HIT_TABLE;
	if( ( t.x | t.y ) != 0u )
		jahSpecularAlbedoHemi( r, a, b );
}
