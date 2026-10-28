// A VOXEL-ANSWERED HIT'S DIRECT LIGHT, COMPUTED AT THE HIT (FAR-SUN-2; SPECS/FAR-SUN-2.md stage 2,
// spikes/far-sun-2/NOTES.md).
//
// WHY: the voxel store answers a hit with its CELL's light. A face's direct light varies inside a
// cell - lit and shaded parts, faces of other orientations, a black wall's foot beside the sunlit
// floor - and the store keeps one light per cell (level 0: two sides of one merged normal) or one
// per half-axis (the directional level, two cells wide). Measured: a lit face read 0.79 of its own
// direct light at a 1.875 m cell in the canyon and 0.33 on Bistro's lit hits at cascade 3, a
// shaded chair back in a lit cell read 53 times its own; no read of the store closed it, only
// shading at the hit did. The direct light is a POINT quantity and the bounce varies slowly, so:
//
//     R(hit) = sum over the lights of D_l(hit) + E(hit) + the store's BOUNCE part
//
// D_l(hit) = rho_hit x Dd( L, V ) x (1 / pi) x the light's diffuse x its attenuation x cos( n_hit, L ) x
// V_l(hit) - HlmsPbs's OWN DIFFUSE LOBE TOWARD THE HIT'S VIEWER (PHOTON-4 stage 2: jahDisneyDiffuse at
// N.L, N.V, V.H, BRDF_Default's, the decode's and the card read's), with the injection's per-light
// falloff (LightInjection_piece_cs.any's halfLight: a lamp's one falloff and range window, a spot's cone,
// an area light's polygon form factor, the sun's cloud-sheet transmittance) and the hit's own shadow ray
// per light instead of the voxel march. E(hit) the material's emissive. The material is
// the VOXELISER'S (PhotonVoxelMaterial's rows, the row GpuInstance::ids.y names: the albedo and
// emissive colours times their pool maps at the hit's UV, the scalar roughness) - what the store
// itself was voxelised from. The bounce part is what jah_rq_hit.glsl's jahVoxelRadiance hands back
// in jahVoxelBounceOut: the store's read times its share that is NOT the direct volume's.
//
// THE TABLE (RayQueryTier::updateHitTable), read through its device address JAH_HIT_TABLE (a uvec2;
// zero = no table: the hit reads the store whole): u[0] = the lights, u[1] = the material rows,
// u[2] = the first light's word, u[3] = the first row's word, u[4] = the first word of the metal's lobe
// integrals (METAL-BOUNCE-1: photon/voxel/PhotonSpecularAlbedo.h's table, its floats' bits), u[5] = the first
// word of THE EMITTER TABLE and u[6] = its targets (PHOTON-7 stage 11, 11-1a: 8 words each - the world bounding
// sphere's centre and radius, the row's emissive radiance and the slot; RayQueryTier::updateEmitterTable). A light is
// 32 words (eight vec4):
//   0 position (directional: the direction TOWARDS the light), w = the type (0 directional,
//     1 point, 2 spot, 5 area); 1 the diffuse colour x power, w = the range (0: none);
//   2 a spot's or an area light's direction (-z: an area light's emitting side), w = a point/spot's source radius;
//     3 a spot's (1 / (cos inner - cos outer), cos outer, falloff), w = 1 for a two-sided area light; 4-7 an area
//     light's corners.
// A row is 16 words: bgDiffuse, the colour the albedo map multiplies (the metallic workflow's base, the
// specular workflows' diffuse) and the transparency, the emissive and the metalness (vec4 each), the
// albedo and emissive pool slices (16 bits each, one word), the specular weight, the roughness's float
// bits, and a word of the flags (bit 0 the albedo samples its slice, bit 1 the emissive does) beside the
// untinted F0 above the dielectric's, ( F0 - 0.04 ) / 0.96 at 10 bits a channel from bit 2 (the specular
// workflows' Fresnel, 0 in the metallic one) - PhotonVoxelMaterial::writeRow's row.
//
// HOW A CALLER BINDS IT (macros and names defined first): JAH_HIT_TABLE (uvec2), JAH_HIT_POOL_ON and
// JAH_HIT_CLOUD_ON (bool), JAH_HIT_CLOUD_MAP and JAH_HIT_CLOUD_SUN (vec4: jah_cloud_shadow.glsl's
// two), the samplers `jahHitPool` (sampler2DArray) and `jahHitCloud` (sampler2D); jah_rq_geom.glsl,
// jah_rq_sun_ray.glsl and jah_rq_hit_record.glsl included before (the instance table, the
// geometry rows, the shadow ray and its lift). Before calling, the caller describes the hit in
// `jahHitSurf`.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_HIT_DIRECT_GLSL
#define JAH_RQ_HIT_DIRECT_GLSL
#include "jah_ray_origin.glsl"
// THE AREA LIGHT'S VISIBILITY AT A HIT (PHOTON-7, 7-H at the photon-7r merge; L9): the strata law every area-light
// reader takes (jah_area_strata.glsl - the cards, the pixel): the rectangle cut into kHitAreaStrata x kHitAreaStrata
// strata jittered by the hit point's world key, their form factors f computed (no ray), and ONE stratum's ray a sample:
// the stratum drawn with probability f_k / sum f by the sample's sequence (jahHitAreaSeq, the includer's - the
// reflection's draw index, the gather's ray seed), its visibility V_k taken whole - so its expectation is the ratio
// sum( V f ) / sum( f ) the other readers converge to, and an UNOCCLUDED light reads exactly 1 (no noise where nothing
// shadows). The centre ray it replaces saw a light through any gap at its centre and lost it behind anything there.
// AT REST THE DRAW IS STRATIFIED IN TIME (PHOTON-7 11-2 (2), the lead's ruling (B)): a rest sequence index s
// (jahHitAreaCycle >= 0, the includer's) takes stratum ( s + its texel's offset ) mod S, S = kHitAreaStrata^2, at the
// weight f_k S / sum f - so every S consecutive rest samples visit each stratum exactly once, their mean is the ratio
// exactly, and a sparse stratum (the light through a small gap) is never left out of a rest mean (the importance draw
// picked a 1 % stratum about once in 100 samples, and the per-texel stop judged the stream before it came: -32 % on
// rtreflect.hit_area_shadow). The texel's offset (jahHitAreaCycleKey) keeps neighbours off lockstep. Moving (-1): the
// importance draw above, an unoccluded light exactly 1 a sample.
#include "jah_area_strata.glsl"
const uint kHitAreaStrata = 4u;
uint jahHitAreaSeq = 0u;
int jahHitAreaCycle = -1;
uint jahHitAreaCycleKey = 0u;

#include "jah_fork_brdf.glsl"

#define JAH_CLOUD_TAU( uv ) textureLod( jahHitCloud, uv, 0.0 ).x
#include "jah_cloud_shadow.glsl"

// the table's reference type and the lights' words (shared with the card read's light loop)
#define JAH_CARD_CLOUD( P ) ( JAH_HIT_CLOUD_ON ? jahCloudTransmittance( P, JAH_HIT_CLOUD_MAP, JAH_HIT_CLOUD_SUN ) : 1.0 )
#include "jah_rq_card_lights.glsl"
const uint kJahHitRowWords = 16u;
const uint kJahHitRowsPerPool = 1024u;
const uint kJahHitRowDiffuseTex = 1u;
const uint kJahHitRowEmissiveTex = 2u;

/// THE METAL'S LOBE INTEGRALS (METAL-BOUNCE-1), from the table's u[4]: the floats the Photon jobs read
/// from their constant buffer (JahSpecularAlbedo_piece_all.any, unwrapped by the build into
/// jah_spec_albedo.glsl), so a hit's metal and its voxel re-emit by one table.
float jahHitSpecularAlbedoWord( int k )
{
	JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
	return uintBitsToFloat( T.u[T.u[4] + uint( k )] );
}
#define JAH_SPEC_ALBEDO( k ) jahHitSpecularAlbedoWord( k )
#include "jah_spec_albedo.glsl"

/// THE HIT, as its caller saw it: the slot, the BLAS geometry (= submesh) and triangle, the
/// barycentrics, the object-to-world transform, the ray's length and whether the far copy
/// answered (its triangle is the mesh's coarsest level: no row of the near copy names it).
struct JahHitSurface
{
	uint slot;
	uint geom;
	uint prim;
	vec2 bary;
	mat4x3 objectToWorld;
	float t;
	bool far;
	bool valid;
};
JahHitSurface jahHitSurf = JahHitSurface( 0u, 0u, 0u, vec2( 0.0 ), mat4x3( 1.0 ), 0.0, false, false );

/// The material the store was voxelised from, at the hit. False where the table, the slot's
/// material word or its row is missing (an item the store has not converted yet: one rebuild).
/// `albedo` is the diffuse lobe's, the mapped colour x (1 - metalness); `metal` the specular lobe of EVERY
/// workflow (PHOTON-4), its F0 x kS (( 0.04 + metalness x max( b - 0.04, 0 ) + ( f - 0.04 ) ) x kS, f the
/// specular workflows' untinted Fresnel - a dielectric's too) and Fresnel-90 weight kS - the voxeliser's
/// split (Voxelizer_piece_cs.any, METAL-BOUNCE-1, ENGINE-POLISH-PHOTON-1 stage 2).
bool jahHitMaterial( float footprint, out vec3 albedo, out vec3 emissive, out float roughness, out vec4 metal )
{
	albedo = vec3( 0.0 );
	emissive = vec3( 0.0 );
	roughness = 1.0;
	metal = vec4( 0.0 );
	const uvec2 a = JAH_HIT_TABLE;
	if( ( a.x | a.y ) == 0u || !jahHitSurf.valid || jahHitSurf.slot >= JAH_HIT_INSTANCES )
		return false;
	const uint word = JAH_HIT_INSTANCE( jahHitSurf.slot, 8u ).y;
	if( word == 0xFFFFFFFFu )
		return false;
	JahHitTableRef T = JahHitTableRef( a );
	const uint row = ( word >> 16u ) * kJahHitRowsPerPool + ( word & 0xFFFFu );
	if( row >= T.u[1] )
		return false;
	const uint b = T.u[3] + row * kJahHitRowWords;
	const vec4 bgDiffuse = uintBitsToFloat( uvec4( T.u[b], T.u[b + 1u], T.u[b + 2u], T.u[b + 3u] ) );
	const vec4 diffuse = uintBitsToFloat( uvec4( T.u[b + 4u], T.u[b + 5u], T.u[b + 6u], T.u[b + 7u] ) );
	const vec4 em = uintBitsToFloat( uvec4( T.u[b + 8u], T.u[b + 9u], T.u[b + 10u], T.u[b + 11u] ) );
	const uint slices = T.u[b + 12u];
	const float specW = uintBitsToFloat( T.u[b + 13u] );
	const uint flags = T.u[b + 15u];
	roughness = uintBitsToFloat( T.u[b + 14u] );
	vec3 base = diffuse.xyz * bgDiffuse.xyz;
	emissive = em.xyz;
	if( ( flags & ( kJahHitRowDiffuseTex | kJahHitRowEmissiveTex ) ) != 0u && JAH_HIT_POOL_ON )
	{
		// THE HIT'S UV, and the map's level for the ray's footprint (the triangle's texel density:
		// its UV area over its world area). A far copy's triangle has no near row: the map's
		// coarsest level, its mean.
		vec2 uv = vec2( 0.0 );
		float lod = 64.0;
		if( !jahHitSurf.far && jahHitSurf.slot < JAH_GEOM_SLOTS && JAH_GEOM_ROW_OF( jahHitSurf.slot ) != 0xFFFFFFFFu )
		{
			const uint r = JAH_GEOM_ROW_OF( jahHitSurf.slot ) + jahHitSurf.geom;
			GeometryRow g;
			g.addresses = JAH_GEOM_LANE( r, 0u );
			g.layout0 = JAH_GEOM_LANE( r, 1u );
			g.layout1 = JAH_GEOM_LANE( r, 2u );
			if( ( g.addresses.x | g.addresses.y ) != 0u && ( g.addresses.z | g.addresses.w ) != 0u &&
				( g.layout1.x & GeomUvMask ) != GeomUvNone )
			{
				const vec3 bc = vec3( 1.0 - jahHitSurf.bary.x - jahHitSurf.bary.y, jahHitSurf.bary );
				vec2 t[3];
				vec3 w[3];
				for( uint k = 0u; k < 3u; ++k )
				{
					const uint vtx = geomIndex( g, jahHitSurf.prim * 3u + k );
					t[k] = geomUv( g, vtx );
					w[k] = jahHitSurf.objectToWorld * vec4( geomPosition( g, vtx ), 1.0 );
					uv += t[k] * bc[k];
				}
				const vec2 ts = vec2( textureSize( jahHitPool, 0 ).xy );
				const float uvArea = abs( ( t[1].x - t[0].x ) * ( t[2].y - t[0].y ) -
										  ( t[2].x - t[0].x ) * ( t[1].y - t[0].y ) ) * ts.x * ts.y;
				const float worldArea = length( cross( w[1] - w[0], w[2] - w[0] ) );
				const float texelsPerMetre = sqrt( uvArea / max( worldArea, 1e-12 ) );
				lod = log2( max( footprint * texelsPerMetre, 1.0 ) );
			}
		}
		lod = min( lod, float( textureQueryLevels( jahHitPool ) - 1 ) );
		if( ( flags & kJahHitRowDiffuseTex ) != 0u )
			base = diffuse.xyz * textureLod( jahHitPool, vec3( uv, float( slices & 0xFFFFu ) ), lod ).xyz;
		if( ( flags & kJahHitRowEmissiveTex ) != 0u )
			emissive = em.xyz * textureLod( jahHitPool, vec3( uv, float( slices >> 16u ) ), lod ).xyz;
	}
	albedo = base * ( 1.0 - em.w );
	// ONE EXPRESSION ON EVERY ROUTE: kS ( 0.04 + m max( b - 0.04, 0 ) + ( f - 0.04 ) ) - the store's encoding clamps
	// a channel below the dielectric's F0 at it (Voxelizer_piece_cs.any), so the hit does too.
	const vec3 f0Rel = vec3( float( ( flags >> 2u ) & 1023u ), float( ( flags >> 12u ) & 1023u ), float( flags >> 22u ) ) *
					   ( 1.0 / 1023.0 );
	metal = vec4( ( vec3( 0.04 ) + em.w * max( base - vec3( 0.04 ), vec3( 0.0 ) ) + 0.96 * f0Rel ) * specW, specW );
	return true;
}

/// THE POLYGON FORM FACTOR of an area light's rectangle seen from P about N - Ogre's LTC_Evaluate
/// with the identity matrix (AreaLights_LTC_piece_ps.any, its clipping variant: the quad clipped to
/// P's horizon, each edge's integral summed), the injection's cosL for an area light.
vec3 jahHitEdgeVec( vec3 v1, vec3 v2 )
{
	const float x = dot( v1, v2 );
	const float y = abs( x );
	const float a = 0.8543985 + ( 0.4965155 + 0.0145206 * y ) * y;
	const float b = 3.4175940 + ( 4.1616724 + y ) * y;
	const float v = a / b;
	const float thetaSinTheta = ( x > 0.0 ) ? v : 0.5 * inversesqrt( max( 1.0 - x * x, 1e-7 ) ) - v;
	return cross( v1, v2 ) * thetaSinTheta;
}
float jahHitQuadFormFactor( vec3 N, vec3 P, vec3 c0, vec3 c1, vec3 c2, vec3 c3, bool twoSided )
{
	const vec3 t1 = normalize( abs( N.y ) < 0.99 ? cross( N, vec3( 0.0, 1.0, 0.0 ) ) : cross( N, vec3( 1.0, 0.0, 0.0 ) ) );
	const vec3 t2 = cross( N, t1 );
	const mat3 toLocal = transpose( mat3( t1, t2, N ) );
	vec3 L[5];
	L[0] = toLocal * ( c0 - P );
	L[1] = toLocal * ( c1 - P );
	L[2] = toLocal * ( c2 - P );
	L[3] = toLocal * ( c3 - P );
	L[4] = vec3( 0.0, 1.0, 0.0 );
	int config = 0;
	if( L[0].z > 0.0 ) config += 1;
	if( L[1].z > 0.0 ) config += 2;
	if( L[2].z > 0.0 ) config += 4;
	if( L[3].z > 0.0 ) config += 8;
	int n = 0;
	if( config == 1 ) { n = 3; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[3].z * L[0] + L[0].z * L[3]; }
	else if( config == 2 ) { n = 3; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
	else if( config == 3 ) { n = 4; L[2] = -L[2].z * L[1] + L[1].z * L[2]; L[3] = -L[3].z * L[0] + L[0].z * L[3]; }
	else if( config == 4 ) { n = 3; L[0] = -L[3].z * L[2] + L[2].z * L[3]; L[1] = -L[1].z * L[2] + L[2].z * L[1]; }
	else if( config == 6 ) { n = 4; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
	else if( config == 7 ) { n = 5; L[4] = -L[3].z * L[0] + L[0].z * L[3]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
	else if( config == 8 ) { n = 3; L[0] = -L[0].z * L[3] + L[3].z * L[0]; L[1] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = L[3]; }
	else if( config == 9 ) { n = 4; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[2].z * L[3] + L[3].z * L[2]; }
	else if( config == 11 ) { n = 5; L[4] = L[3]; L[3] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
	else if( config == 12 ) { n = 4; L[1] = -L[1].z * L[2] + L[2].z * L[1]; L[0] = -L[0].z * L[3] + L[3].z * L[0]; }
	else if( config == 13 ) { n = 5; L[4] = L[3]; L[3] = L[2]; L[2] = -L[1].z * L[2] + L[2].z * L[1]; L[1] = -L[1].z * L[0] + L[0].z * L[1]; }
	else if( config == 14 ) { n = 5; L[4] = -L[0].z * L[3] + L[3].z * L[0]; L[0] = -L[0].z * L[1] + L[1].z * L[0]; }
	else if( config == 15 ) { n = 4; }
	if( n == 0 )
		return 0.0;
	if( n == 3 ) L[3] = L[0];
	if( n == 4 ) L[4] = L[0];
	for( int k = 0; k < 5; ++k )
		L[k] = normalize( L[k] );
	float sum = jahHitEdgeVec( L[0], L[1] ).z + jahHitEdgeVec( L[1], L[2] ).z + jahHitEdgeVec( L[2], L[3] ).z;
	if( n >= 4 ) sum += jahHitEdgeVec( L[3], L[4] ).z;
	if( n == 5 ) sum += jahHitEdgeVec( L[4], L[0] ).z;
	return twoSided ? abs( sum ) : max( 0.0, sum );
}

/// THE HIT'S DIRECT LIGHT TOWARD ITS VIEWER V (the reversed ray): every light of the table at P about N
/// (the geometric normal facing the ray's origin), the material's albedo, metal and roughness - HlmsPbs's
/// diffuse lobe toward V on EVERY ray kind (PHOTON-4 stage 2: the decode and the card read shade toward the
/// viewer of a gather ray and a reflection ray alike), the hit's own shadow ray (the casters, the sun's mask
/// and lift). THE SPECULAR LOBE, BY THE RAY'S KIND (PHOTON-3 stage 2; the lead's ruling): a REFLECTION ray is
/// one direction per pixel and takes E mu f_s( L, V ), f_s = BRDF_Default's D Vis F with the hit's F0 and w
/// (jahSpecularLobe) - what the surface sends along it; a GATHER ray (the job defines JAH_HIT_LOBE_INTEGRAL)
/// estimates an irradiance integral with a few stochastic rays, which cannot sample a polished hit's near-delta
/// lobe (a mirror floor's sun glint, gi.id_depth), so it takes the lobe's integral mu ( F0 A( mu, r ) + w B( mu, r ) )
/// - the store's own re-emission, energy-exact in expectation (Lumen's choice for the same reason).
vec3 jahHitDirect( vec3 P, vec3 N, vec3 V, vec3 albedo, float roughness, vec4 metal )
{
	JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
	const uint count = T.u[0];
	const uint base = T.u[2];
	// THE ONE ORIGIN LAW (jah_ray_origin.glsl): the hit's own float bound (its position, the distance it was carried)
	// plus its surface term - 0 on level 0, which the casters' near copies are; a far copy's JAH_HIT_FAR_LIFT.
	const vec3 origin = jahRayOrigin( P, N, jahHitSurf.t, jahHitSurf.far ? JAH_HIT_FAR_LIFT : 0.0 );
	const float lift = length( origin - P );
	vec3 sum = vec3( 0.0 );
	vec3 spec = vec3( 0.0 );
	const float NdotV = clamp( dot( N, V ), 0.0, 1.0 );
	for( uint i = 0u; i < count; ++i )
	{
		const uint o = base + i * kJahHitLightWords;
		const vec4 v0 = uintBitsToFloat( uvec4( T.u[o], T.u[o + 1u], T.u[o + 2u], T.u[o + 3u] ) );
		const vec4 v1 = uintBitsToFloat( uvec4( T.u[o + 4u], T.u[o + 5u], T.u[o + 6u], T.u[o + 7u] ) );
		const vec4 v2 = uintBitsToFloat( uvec4( T.u[o + 8u], T.u[o + 9u], T.u[o + 10u], T.u[o + 11u] ) );
		const vec4 v3 = uintBitsToFloat( uvec4( T.u[o + 12u], T.u[o + 13u], T.u[o + 14u], T.u[o + 15u] ) );
		const int type = int( v0.w + 0.5 );
		vec3 L;
		float dist = 0.0;
		float atten = 1.0;
		if( type == 0 )
		{
			L = normalize( v0.xyz );
			// THE CLOUD SHEET on the sun's way to the hit (the injection's lightAtten for a
			// directional light, CLOUDS-2D-2)
			if( JAH_HIT_CLOUD_ON )
				atten *= jahCloudTransmittance( P, JAH_HIT_CLOUD_MAP, JAH_HIT_CLOUD_SUN );
		}
		else
		{
			const vec3 d = v0.xyz - P;
			dist = length( d );
			L = d / max( dist, 1e-6 );
			if( type == 1 || type == 2 )
				atten = jahLightAttenuation( dist, v1.w, v2.w );
			else if( v1.w > 0.0 )
				atten = jahLightWindow( dist, v1.w );
			if( type == 2 )
				atten *= jahSpotAttenuation( dot( -L, v2.xyz ), v3.xyz );
		}
		if( !( atten > 0.0 ) )
			continue;
		float cosL = dot( N, L );
		if( type == 5 )
		{
			vec3 c[4];
			for( uint k = 0u; k < 4u; ++k )
				c[k] = uintBitsToFloat( uvec3( T.u[o + 16u + 4u * k], T.u[o + 17u + 4u * k], T.u[o + 18u + 4u * k] ) );
			cosL = jahHitQuadFormFactor( N, P, c[0], c[1], c[2], c[3], v3.w > 0.5 );
		}
		if( !( cosL > 0.0 ) )
			continue;
		float vis;
		if( type == 5 )
		{
			// THE STRATA (the head of this file): the corners' rectangle, its emitting side the light's -Z
			const vec3 c0 = uintBitsToFloat( uvec3( T.u[o + 16u], T.u[o + 17u], T.u[o + 18u] ) );
			const vec3 eu = uintBitsToFloat( uvec3( T.u[o + 20u], T.u[o + 21u], T.u[o + 22u] ) ) - c0;
			const vec3 ev = uintBitsToFloat( uvec3( T.u[o + 28u], T.u[o + 29u], T.u[o + 30u] ) ) - c0;
			const uint n = kHitAreaStrata, key = jahAreaStrataKey( P );
			const bool twoSided = v3.w > 0.5;
			float f[kHitAreaStrata * kHitAreaStrata];
			float sumF = 0.0;
			vec3 dq, oq, rq;
			float clr2, reach;
			for( uint k = 0u; k < n * n; ++k )
			{
				f[k] = jahAreaStratumAt( P, N, N, lift, c0, eu, ev, v2.xyz, twoSided, n, k, key, kAreaLampEnd, dq, clr2,
										 oq, rq, reach );
				sumF += f[k];
			}
			vis = 0.0;
			if( sumF > 0.0 && jahHitAreaCycle >= 0 )
			{
				// (the rest's time-stratified draw: the cycle's stratum at its weight f_k S / sum f)
				const uint S = n * n;
				const uint pick = ( uint( jahHitAreaCycle ) + jahAreaPcg( jahHitAreaCycleKey ^ 0x51ED270Bu ) ) % S;
				if( f[pick] > 0.0 )
				{
					jahAreaStratumAt( P, N, N, lift, c0, eu, ev, v2.xyz, twoSided, n, pick, key, kAreaLampEnd, dq, clr2,
									  oq, rq, reach );
					vis = ( reach > 0.0 ? jahSunRay( oq, rq, JAH_HIT_SUN_MASK, reach, reach ) : 1.0 ) * f[pick] *
						  float( S ) / sumF;
				}
			}
			else if( sumF > 0.0 )
			{
				float u = float( jahAreaPcg( jahHitAreaSeq ^ jahAreaPcg( key ) ) ) * 2.3283064e-10 * sumF;
				uint pick = 0u;
				for( ; pick + 1u < n * n; ++pick )
				{
					if( f[pick] > 0.0 && u < f[pick] )
						break;
					u -= f[pick];
				}
				while( !( f[pick] > 0.0 ) && pick > 0u )   // (a rounding walk past the last stratum that lights)
					--pick;
				jahAreaStratumAt( P, N, N, lift, c0, eu, ev, v2.xyz, twoSided, n, pick, key, kAreaLampEnd, dq, clr2, oq,
								  rq, reach );
				vis = reach > 0.0 ? jahSunRay( oq, rq, JAH_HIT_SUN_MASK, reach, reach ) : 1.0;
			}
		}
		else
			vis = type == 0 ? jahSunRay( origin, L, JAH_HIT_SUN_MASK, JAH_HIT_SUN_RANGE, JAH_HIT_SUN_RANGE )
							: jahSunRay( origin, L, JAH_HIT_SUN_MASK, max( dist - lift, 0.0 ), max( dist - lift, 0.0 ) );
		if( !( vis > 0.0 ) )
			continue;
		// THE DIFFUSE LOBE TOWARD V: BRDF_Default's normalised Disney lobe at ( L, V ). An AREA light (type 5) has no
		// single L: its lobe is taken at the direction to its CENTRE (L above), its form factor cosL kept whole.
		const float NdotL = clamp( dot( N, L ), 0.0, 1.0 );
		const vec3 halfV = L + V;
		const float VdotH = dot( halfV, halfV ) > 1e-12 ? clamp( dot( V, normalize( halfV ) ), 0.0, 1.0 ) : 0.0;
		sum += cosL * jahDisneyDiffuse( NdotL, NdotV, VdotH, roughness ) * ( 0.318309886 * v1.xyz ) * atten * vis;
		// the light's irradiance E = v1 (pi x its intensity) at cosine cosL: sent toward V by the lobe (a
		// reflection ray), or re-emitted by the lobe's integral, mu ( F0 A + w B ) E / pi (a GATHER ray:
		// JAH_HIT_LOBE_INTEGRAL - the lead's ruling, the header says why)
		if( metal.w > 0.0 )
		{
#ifdef JAH_HIT_LOBE_INTEGRAL
			float sa, sb;
			jahSpecularAlbedo( clamp( dot( N, L ), 0.0, 1.0 ), roughness, sa, sb );
			spec += ( cosL * atten * vis * 0.318309886 ) * v1.xyz * ( metal.xyz * sa + metal.w * sb );
#else
			// A LAMP HAS A SIZE (PHOTON-7 stage 11): a point or spot light's glint is the lobe widened by its sphere's
			// angular size, its energy the point lobe's (JahBrdf's jahSphereLightWiden - the pixel's BRDF_Default takes the
			// same function); a directional or area light's as before
			float rS = roughness;
			float sNorm = 1.0;
			if( type == 1 || type == 2 )
			{
				float aW;
				sNorm = jahSphereLightWiden( max( roughness * roughness, 0.001 ), v2.w, dist, clamp( dot( N, L ), 0.0, 1.0 ),
											 aW );
				rS = sqrt( aW );
			}
			spec += ( cosL * atten * vis * sNorm ) * v1.xyz * jahSpecularLobe( N, L, V, rS, metal.xyz, metal.w );
#endif
		}
	}
	// a surface with no specular lobe (kS = 0) adds exactly zero: its light is the diffuse lobe's, bit for bit
	return sum * albedo + spec;
}

/// THE STORE'S BOUNCE PART TOWARD THE HIT'S VIEWER (PHOTON-4 stage 2). The store re-emits what a surface
/// gathered by the lobes' HEMISPHERICAL albedos (LightVctBounceInject_piece_cs.any): rho Dd-bar( r ) for the
/// diffuse lobe and F0 A-bar( r ) + w B-bar( r ) for the specular one, per channel. The diffuse share is sent
/// toward V by the lobe's directional albedo at N.V over its hemispherical mean (the card's environment half,
/// jah_card_view.glsl; its lobe split, jah_rq_card.glsl), the specular share kept as its integral (a gathered
/// field's lobe, the PHOTON-3 ruling): bounce x ( rho Dd( N.V ) + S-bar ) / ( rho Dd-bar + S-bar ).
#ifndef JAH_HIT_GATE_DECLARED
#define JAH_HIT_GATE_DECLARED
bool jahHitTracedLobe = false;   // (jah_rq_card.glsl states it)
#endif
vec3 jahHitBounceToward( vec3 bounce, vec3 albedo, float roughness, vec4 metal, float NdotV )
{
	float sa = 0.0, sb = 0.0;
	if( metal.w > 0.0 )
		jahSpecularAlbedoHemi( roughness, sa, sb );
	const vec3 specLobe = metal.xyz * sa + metal.w * sb;
	const vec3 whole = albedo * jahDiffuseAlbedoHemi( roughness ) + specLobe;
	// (C2: where the reader traces the lobe itself the bounce's specular share is left out - the traced lobe replaces it)
	const vec3 toward = albedo * jahDiffuseAlbedo( NdotV, roughness ) + ( jahHitTracedLobe ? vec3( 0.0 ) : specLobe );
	return mix( bounce, bounce * toward / max( whole, vec3( 1e-12 ) ), greaterThan( whole, vec3( 0.0 ) ) );
}

#endif   // JAH_RQ_HIT_DIRECT_GLSL
