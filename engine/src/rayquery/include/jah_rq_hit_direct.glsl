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
// D_l(hit) = rho_hit x jahDiffuseAlbedo( cos, roughness_hit ) x (1 / pi) x the light's diffuse x
// its attenuation x cos( n_hit, L ) x V_l(hit) - THE INJECTION'S OWN PER-LIGHT FORM
// (LightInjection_piece_cs.any's halfLight: a lamp's one falloff and range window, a spot's cone,
// an area light's polygon form factor, the sun's cloud-sheet transmittance), with the hit's own
// shadow ray per light instead of the voxel march. E(hit) the material's emissive. The material is
// the VOXELISER'S (PhotonVoxelMaterial's rows, the row GpuInstance::ids.y names: the albedo and
// emissive colours times their pool maps at the hit's UV, the scalar roughness) - what the store
// itself was voxelised from. The bounce part is what jah_rq_hit.glsl's jahVoxelRadiance hands back
// in jahVoxelBounceOut: the store's read times its share that is NOT the direct volume's.
//
// THE TABLE (RayQueryTier::updateHitTable), read through its device address JAH_HIT_TABLE (a uvec2;
// zero = no table: the hit reads the store whole): u[0] = the lights, u[1] = the material rows,
// u[2] = the first light's word, u[3] = the first row's word, u[4] = the first word of the metal's lobe
// integrals (METAL-BOUNCE-1: photon/voxel/PhotonSpecularAlbedo.h's table, its floats' bits). A light is
// 32 words (eight vec4):
//   0 position (directional: the direction TOWARDS the light), w = the type (0 directional,
//     1 point, 2 spot, 5 area); 1 the diffuse colour x power, w = the range (0: none);
//   2 a spot's direction (-z), w = a point/spot's source radius; 3 a spot's (1 / (cos inner -
//     cos outer), cos outer, falloff), w = 1 for a two-sided area light; 4-7 an area light's corners.
// A row is 16 words: bgDiffuse, the BASE colour and the transparency, the emissive and the
// metalness (vec4 each), the albedo and emissive pool slices (16 bits each, one word), the
// specular weight, the roughness's float bits, the flags (bit 0 the albedo samples its slice, bit 1
// the emissive does) - PhotonVoxelMaterial::writeRow's row.
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

#include "jah_fork_brdf.glsl"

#define JAH_CLOUD_TAU( uv ) textureLod( jahHitCloud, uv, 0.0 ).x
#include "jah_cloud_shadow.glsl"

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahHitTableRef
{
	uint u[];
};

const uint kJahHitLightWords = 32u;
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
/// `albedo` is the diffuse lobe's, base x (1 - metalness); `metal` the metal's specular lobe, its F0
/// (base x metalness x kS) and Fresnel-90 weight (metalness x kS) - the voxeliser's split
/// (Voxelizer_piece_cs.any, METAL-BOUNCE-1).
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
	const float metalW = em.w * uintBitsToFloat( T.u[b + 13u] );
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
	metal = vec4( base * metalW, metalW );
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

/// THE HIT'S DIRECT LIGHT: every light of the table at P about N (the geometric normal facing the
/// ray's origin), the material's albedo, metal and roughness - the injection's per-light form (the
/// diffuse lobe's directional albedo, and a metal's specular lobe's, F0 A( mu, r ) + w B( mu, r ):
/// METAL-BOUNCE-1), the hit's own shadow ray (the casters, the sun's mask and lift).
vec3 jahHitDirect( vec3 P, vec3 N, vec3 albedo, float roughness, vec4 metal )
{
	JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
	const uint count = T.u[0];
	const uint base = T.u[2];
	const float lift = max( jahHitSurf.far ? JAH_HIT_FAR_LIFT : JAH_HIT_LIFT, 1e-4 * jahHitSurf.t );
	const vec3 origin = P + N * lift;
	vec3 sum = vec3( 0.0 );
	vec3 sumA = vec3( 0.0 );
	vec3 sumB = vec3( 0.0 );
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
		const float vis = type == 0 ? jahSunRay( origin, L, JAH_HIT_SUN_MASK, JAH_HIT_SUN_RANGE, JAH_HIT_SUN_RANGE )
									: jahSunRay( origin, L, JAH_HIT_SUN_MASK, max( dist - lift, 0.0 ), max( dist - lift, 0.0 ) );
		if( !( vis > 0.0 ) )
			continue;
		sum += cosL * jahDiffuseAlbedo( clamp( dot( N, L ), 0.0, 1.0 ), roughness ) * ( 0.318309886 * v1.xyz ) * atten * vis;
		if( metal.w > 0.0 )
		{
			float sa, sb;
			jahSpecularAlbedo( clamp( dot( N, L ), 0.0, 1.0 ), roughness, sa, sb );
			const vec3 l = cosL * ( 0.318309886 * v1.xyz ) * atten * vis;
			sumA += sa * l;
			sumB += sb * l;
		}
	}
	// a non-metal adds exactly zero (the metal's terms are zero): its light is the diffuse lobe's, bit for bit
	return sum * albedo + ( sumA * metal.xyz + sumB * metal.w );
}

#endif   // JAH_RQ_HIT_DIRECT_GLSL
