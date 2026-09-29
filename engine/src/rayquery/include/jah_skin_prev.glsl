// A POSED ITEM'S POINT, LAST FRAME (REFLECT-MOVERS-2; shared since REFLECT-EDGE-2 by
// the reflection trace and the march's velocity job, so the two cannot disagree
// about where a character's surface was).
//
// The skin cache keeps a PREVIOUS-POSE SLICE (SkinCache.h: float3 per vertex, at the
// byte offset the row's first pad word holds — GeometryRow layout1.z; 0 = no slice).
// The triangle's three vertices as they were last frame, at the point's
// barycentrics, through the instance's previous pose (the node's motion and the
// pose's together). Only the DIFFERENCE is applied to the point, so a triangle that
// did not move gives the point back bit for bit. False when nothing moved, or the
// slot's cache is not ready (no row, or no slice).
//
// The includer defines, before this file:
//   JAH_GEOM_SLOTS, JAH_GEOM_ROW_OF( slot ), JAH_GEOM_LANE( row, k )  (jah_rq_geom.glsl's)
//   JAH_SKIN_INSTANCE( slot, k )  the GPU scene instance table's uvec4 row k of a slot
//     (rows 0-2 = world, 3-5 = prevWorld — GpuScene.h)
// and includes jah_rq_geom.glsl.
#ifndef JAH_SKIN_PREV_GLSL
#define JAH_SKIN_PREV_GLSL

bool jahSkinnedPrevPoint( uint slot, uint geomIdx, uint prim, vec2 bary, vec3 pointNow, out vec3 was )
{
	was = pointNow;
	if( slot >= JAH_GEOM_SLOTS )
		return false;
	uint row = JAH_GEOM_ROW_OF( slot );
	if( row == 0xFFFFFFFFu )
		return false;
	row += geomIdx;
	GeometryRow g;
	g.addresses = JAH_GEOM_LANE( row, 0u );
	g.layout0 = JAH_GEOM_LANE( row, 1u );
	g.layout1 = JAH_GEOM_LANE( row, 2u );
	if( g.layout1.z == 0u || ( g.addresses.x | g.addresses.y ) == 0u || ( g.addresses.z | g.addresses.w ) == 0u )
		return false;
	GeomFloatRef prevRef = GeomFloatRef( jahGeomAddressPlus( g.addresses.xy, g.layout1.z ) );
	const vec3 b = vec3( 1.0 - bary.x - bary.y, bary.x, bary.y );
	vec3 cur = vec3( 0.0 ), old = vec3( 0.0 );
	bool poseMoved = false;
	for( uint k = 0u; k < 3u; ++k )
	{
		const uint vtx = geomIndex( g, prim * 3u + k );
		const vec3 c = geomPosition( g, vtx );
		const vec3 o = vec3( prevRef.v[vtx * 3u], prevRef.v[vtx * 3u + 1u], prevRef.v[vtx * 3u + 2u] );
		poseMoved = poseMoved || any( notEqual( floatBitsToUint( c ), floatBitsToUint( o ) ) );
		cur += c * b[k];
		old += o * b[k];
	}
	const uvec4 w0 = JAH_SKIN_INSTANCE( slot, 0u ), w1 = JAH_SKIN_INSTANCE( slot, 1u ), w2 = JAH_SKIN_INSTANCE( slot, 2u );
	const uvec4 q0 = JAH_SKIN_INSTANCE( slot, 3u ), q1 = JAH_SKIN_INSTANCE( slot, 4u ), q2 = JAH_SKIN_INSTANCE( slot, 5u );
	const bool nodeMoved = any( notEqual( w0, q0 ) ) || any( notEqual( w1, q1 ) ) || any( notEqual( w2, q2 ) );
	if( !poseMoved && !nodeMoved )
		return false;
	const vec4 c4 = vec4( cur, 1.0 ), o4 = vec4( old, 1.0 );
	const vec3 curW = vec3( dot( uintBitsToFloat( w0 ), c4 ), dot( uintBitsToFloat( w1 ), c4 ),
							dot( uintBitsToFloat( w2 ), c4 ) );
	const vec3 oldW = vec3( dot( uintBitsToFloat( q0 ), o4 ), dot( uintBitsToFloat( q1 ), o4 ),
							dot( uintBitsToFloat( q2 ), o4 ) );
	was = pointNow + ( oldW - curW );
	return true;
}

#endif   // JAH_SKIN_PREV_GLSL
