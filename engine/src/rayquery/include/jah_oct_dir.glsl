// THE OCTAHEDRAL DIRECTION, ONE TEXT (a unit vector in two snorm components): the hit record's ray direction
// (jah_rq_hit_record.glsl, read back by rq_hit_composite.comp) and the blended passes' escape records (PhotonVct's
// fragment writes them through the piece JahLampRayLib, rq_glass_escape.comp reads them). No Hlms directive mark here.
#ifndef JAH_OCT_DIR_GLSL
#define JAH_OCT_DIR_GLSL
vec2 jahOctEncode( vec3 n )
{
	n /= max( abs( n.x ) + abs( n.y ) + abs( n.z ), 1e-20 );
	vec2 e = n.xy;
	if( n.z < 0.0 )
		e = ( 1.0 - abs( n.yx ) ) * vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
	return e;
}
vec3 jahOctDecodeDir( vec2 e )
{
	vec3 n = vec3( e, 1.0 - abs( e.x ) - abs( e.y ) );
	if( n.z < 0.0 )
		n.xy = ( 1.0 - abs( n.yx ) ) * vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
	return normalize( n );
}
#endif   // JAH_OCT_DIR_GLSL
