// THE CLOUD LAYER'S GEOMETRY AND EXTINCTION (CLOUDS-2D-1) -- the one copy,
// included by the layer itself (JahCloudLayer_ps.glsl) and by the sun disc
// seen through it (JahSunDiscClouded_ps.glsl), so the disc is dimmed by
// exactly the cloud the layer draws in front of it.
//
// THE MODEL. One sheet of cloud at `layer.x` metres above the ground of a
// CURVED earth (radius 6360 km): a camera ray meets the sheet at the distance
// where it crosses the sphere of radius R + altitude, so the sheet converges
// to the horizon the way a real layer does -- a low layer fills the sky, a
// high one stays overhead ("the altitude look"). Where it meets it, the sheet
// has a VERTICAL optical depth tau, baked on a change into a tiling field
// (JahCloudBake_ps.glsl) of `1 / layer.y` metres per tile and scrolled by
// `layer.zw` metres (the wind times the scene's clock, pushed by the host).
// A ray crossing the sheet at cosine mu to its normal travels tau / mu of it
// (Beer-Lambert through a slab).
//
// The including shader defines JAH_CLOUD_TAU( uv ) to read the field.

struct JahCloudHit
{
	vec2  uv;     // field coordinates (one tile = [0,1))
	float mu;     // cosine between the ray and the sheet's normal where they meet
	float dist;   // metres from the camera to the sheet
	float above;  // 1 = the ray reaches the sheet (it leaves above the horizon), else 0
};

JahCloudHit jahCloudIntersect( vec3 dir, vec3 camPos, vec4 layer )
{
	JahCloudHit h;
	const float kEarthRadius = 6360000.0;
	// The camera stands on the ground of the sphere (its world height above
	// y = 0 is its altitude), held under the sheet: this is a layer seen from
	// below, and a camera above it is not a case the 2D layer can draw.
	const float camY = clamp( camPos.y, 0.0, max( layer.x - 1.0, 0.0 ) );
	const float r0 = kEarthRadius + camY;
	const float r1 = kEarthRadius + layer.x;
	const float b = r0 * dir.y;
	// r1^2 - r0^2 without the cancellation of two 4e13 squares.
	const float c = ( layer.x - camY ) * ( r1 + r0 );
	// The positive root of t^2 + 2bt - c = 0 in its STABLE form for b >= 0
	// (-b + sqrt(b^2 + c) cancels catastrophically at the zenith in float).
	const float s = sqrt( max( b * b + c, 0.0 ) );
	const float t = b >= 0.0 ? c / max( b + s, 1e-6 ) : s - b;
	h.dist = t;
	h.mu = clamp( ( b + t ) / r1, 0.0, 1.0 );
	h.uv = ( camPos.xz + dir.xz * t + layer.zw ) * layer.y;
	// A ray that leaves BELOW the horizon meets the ground first.
	h.above = dir.y > 0.0 ? 1.0 : 0.0;
	return h;
}

// The fraction of the light along the ray that crosses the sheet unscattered.
// 1.0 EXACTLY where the field is empty (exp(-0) is 1), which is what keeps a
// clear sky's pixels byte-identical to a sky with no layer at all.
float jahCloudViewTransmittance( float tau, JahCloudHit h )
{
	return h.above > 0.0 ? exp( -tau / max( h.mu, 0.05 ) ) : 1.0;
}
