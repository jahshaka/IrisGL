// THE HIT SET'S UNIFORMS (PHOTON-7 stage 10, 10-2; the lead's ruling: one text, the bindings' rule) - what every ray job
// that reads its hits through jahHitRadianceStore hands its shader beside the hit set's bindings (ScreenProbeGather.h
// HitSetParams, filled by the one function hitSetParams): the cascades, the environment, the surface cache's counts,
// the geometric normal's rows, the hit list and its sun ray, the alpha and hit tables, the hit's own surface. A job's
// uniform block holds one JahHitParams member and names it JAH_HIT_PARAMS before including this file, which defines
// every macro the hit read takes (jah_rq_hit.glsl, jah_rq_card.glsl, jah_rq_geom.glsl, jah_rq_hit_record.glsl,
// jah_rq_hit_direct.glsl, jah_rq_hit_radiance.glsl) over it. The reflection (jah_reflect_params.glsl), the gather
// (jah_probe_params.glsl) and the cards' gather (rq_card_gather.comp) bind it.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_HIT_PARAMS_GLSL
#define JAH_RQ_HIT_PARAMS_GLSL

#ifndef JAH_HIT_PARAMS
#error "name the job's JahHitParams member as JAH_HIT_PARAMS before including jah_rq_hit_params.glsl"
#endif

/// std140, vec4s only (HitSetParams is its C++ mirror). Four cascades (the hit set's kGatherMaxCascades).
struct JahHitParams
{
	vec4 voxelOrigin[4];    // per cascade: its box's corner (world, xyz; w 0) - past the chain's length the last's
	vec4 voxelInvSize[4];   // ...1 / its box's size (xyz) and its cell, the largest side (w)
	vec4 counts;            // x the cascades, y the sky cube on, z the per-slot geometry rows, w a ray-traced mover exists
	vec4 skyColour;         // the cube's gain, or the flat environment (xyz)
	vec4 cards;             // x the card slots, y the card records (0 with no cache)
	vec4 hitList;           // x the list's capacity, y its width, z the instance table's entries, w the list on
	vec4 hitSun;            // xyz toward the sun, w a sun
	vec4 hitSun2;           // x the hit's sun ray's mask, z its length, w a far copy's sun-leg lift
	vec4 tables;            // xy the alpha table's device address, zw the hit table's (float bits)
	vec4 surface;           // x the voxeliser's texture pool bound, y the cloud field bound
	vec4 cloudMap;          // the cloud layer's 1 / tile, strength, scroll x, z
	vec4 cloudSun;          // ...its sun throw x, z, altitude, 1 / mu_s
};

#define JAH_VOX_COUNT        int( JAH_HIT_PARAMS.counts.x )
#define JAH_VOX_ORIGIN( c )  JAH_HIT_PARAMS.voxelOrigin[c]
#define JAH_VOX_INVSIZE( c ) JAH_HIT_PARAMS.voxelInvSize[c]
#define JAH_SKY_ON           ( JAH_HIT_PARAMS.counts.y > 0.5 )
#define JAH_SKY_COLOUR       JAH_HIT_PARAMS.skyColour.xyz
#define JAH_CARD_SLOTS       uint( JAH_HIT_PARAMS.cards.x )
#define JAH_CARD_RECORDS     uint( JAH_HIT_PARAMS.cards.y )
#define JAH_GEOM_SLOTS       uint( JAH_HIT_PARAMS.counts.z )
#define JAH_HIT_ON           ( JAH_HIT_PARAMS.hitList.w > 0.5 )
#define JAH_HIT_CAPACITY     uint( JAH_HIT_PARAMS.hitList.x )
#define JAH_HIT_LIST_W       max( uint( JAH_HIT_PARAMS.hitList.y ), 1u )
#define JAH_HIT_INSTANCES    uint( JAH_HIT_PARAMS.hitList.z )
#define JAH_HIT_SUN          JAH_HIT_PARAMS.hitSun
#define JAH_HIT_SUN_MASK     uint( JAH_HIT_PARAMS.hitSun2.x )
#define JAH_HIT_SUN_RANGE    JAH_HIT_PARAMS.hitSun2.z
#define JAH_HIT_FAR_LIFT     JAH_HIT_PARAMS.hitSun2.w
#define JAH_ALPHA_TABLE      floatBitsToUint( JAH_HIT_PARAMS.tables.xy )
#define JAH_HIT_TABLE        floatBitsToUint( JAH_HIT_PARAMS.tables.zw )
#define JAH_HIT_POOL_ON      ( JAH_HIT_PARAMS.surface.x > 0.5 )
#define JAH_HIT_CLOUD_ON     ( JAH_HIT_PARAMS.surface.y > 0.5 )
#define JAH_HIT_CLOUD_MAP    JAH_HIT_PARAMS.cloudMap
#define JAH_HIT_CLOUD_SUN    JAH_HIT_PARAMS.cloudSun
#define JAH_HIT_MOVERS_ON    ( JAH_HIT_PARAMS.counts.w > 0.5 )

#endif   // JAH_RQ_HIT_PARAMS_GLSL
