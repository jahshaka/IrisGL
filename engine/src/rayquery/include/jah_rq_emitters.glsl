// THE EMITTER TABLE'S SAMPLING (PHOTON-7 stage 11, 11-1b; the lead's ruling: bounding-sphere CONE sampling, one closest-hit
// ray, the balance heuristic) - the reflection's next-event estimation toward emissive geometry.
//
// THE TABLE is the hit table's section at u[5] (its first word) / u[6] (its targets), RayQueryTier::updateEmitterTable:
// eight words a target - the world bounding sphere's centre and radius, the row's emissive radiance (rgb) and the slot.
//
// THE STRATEGY. From a shading point x with geometric normal g a target e is the CONE of its bounding sphere (inside the
// sphere: the hemisphere above g), solid angle Omega_e; a direction is drawn uniformly in it. The pick takes e with
// probability P( e ) = w_e / W, w_e = Lmax_e Omega_e G_e (Lmax the radiance's largest channel; G_e the GATE: 1 where the
// cone reaches above g, else 0), W = sum w_e. So the light density of ANY direction is
//     p_l( L ) = sum_e P( e ) [ L in S_e ] / Omega_e = ( 1 / W ) sum_e Lmax_e G_e [ L in S_e ]
// from the table alone - a BRDF draw's balance weight needs no triangle. The ray takes E of WHATEVER surface it meets
// first (the row's emissive at the hit, jahHitMaterial: one function on both strategies; the sky is never counted), so the
// two strategies estimate the same quantity, the first hit's emission, for any emitter shape: unbiased; a cone ray that
// misses the emitter is a wasted ray, never a bias (the lead's condition: the hit rate printed; under ~25 % the cone is
// tightened to the instance's oriented box, a later step). The gate is any deterministic function of the texel (x, g, the
// lobe): both strategies read the same one.
//
// HOW A CALLER BINDS IT: JahHitTableRef and JAH_HIT_TABLE (jah_rq_card_lights.glsl / the hit params) declared first.
// No Hlms directive mark anywhere in this file.
#ifndef JAH_RQ_EMITTERS_GLSL
#define JAH_RQ_EMITTERS_GLSL

/// The words a target (OgreRayQuery.cpp kEmitterWords) and the most a shader walks (the tier's cap is at most this:
/// GiQualityFacts::reflectEmitters; a corrupt count is clamped here, the bounded-loop law).
const uint kJahEmitterWords = 8u;
const uint kJahEmitterMax = 96u;   // (the tier cap 64 emitters + kMaxPortals 16 portals + 16 lamp images, 11-2 (8))
/// A PORTAL (PHOTON-7 11-2 (6), RayQueryTier::updatePortals): an opening to the sky in the table's emitter form, slot
/// this word - the reflection's emitter NEE skips it, C2's sky NEE (jah_rq_hit_radiance.glsl) takes portals alone; each
/// strategy's density p_l is its own kind's (`portals` below)
const uint kJahPortalSlot = 0xFFFFFFFFu;
/// A LAMP's IMAGE IN A PLANAR NEAR-MIRROR (PHOTON-7 11-2 (8); RayQueryTier::updateHitTable): the lamp reflected through
/// the mirror's plane, its sphere the lamp's widened by the mirror's lobe - C2's image draw takes these alone; its words
/// 5 / 6 hold the mirror's instance slot and the lamp's index in the light table (uint bits), word 4 its weight
const uint kJahImageSlot = 0xFFFFFFFEu;
/// THE KINDS a strategy draws (jahNeePick's `kind`): the emitters (the reflection's NEE), the sky's portals (C2's sky NEE)
const uint kJahNeeKindEmitters = 0u, kJahNeeKindPortals = 1u, kJahNeeKindImages = 2u;
uint jahEmitterKindOf( uint slot )
{
	return slot == kJahPortalSlot ? kJahNeeKindPortals : ( slot == kJahImageSlot ? kJahNeeKindImages : kJahNeeKindEmitters );
}

uint jahEmitterCount()
{
	const uvec2 a = JAH_HIT_TABLE;
	if( ( a.x | a.y ) == 0u )
		return 0u;
	JahHitTableRef T = JahHitTableRef( a );
	return min( T.u[6], kJahEmitterMax );
}

/// ONE TARGET: its sphere, its radiance's largest channel, its slot.
struct JahEmitter
{
	vec3 c;
	float r;
	float lmax;
	uint slot;
};
JahEmitter jahEmitterAt( uint i )
{
	JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
	const uint b = T.u[5] + i * kJahEmitterWords;
	JahEmitter e;
	e.c = uintBitsToFloat( uvec3( T.u[b], T.u[b + 1u], T.u[b + 2u] ) );
	e.r = uintBitsToFloat( T.u[b + 3u] );
	const vec3 L = uintBitsToFloat( uvec3( T.u[b + 4u], T.u[b + 5u], T.u[b + 6u] ) );
	e.lmax = max( max( L.x, L.y ), L.z );
	e.slot = T.u[b + 7u];
	return e;
}

/// THE TARGET's SET S_e FROM x: the cone of its sphere (axis, cos of its half-angle), or - x inside the sphere - the
/// hemisphere above g (inside = true). Omega its solid angle; 0 for a target that cannot be drawn (no finite size).
struct JahEmitterCone
{
	vec3 axis;
	float cosMax;
	float omega;
	bool inside;
};
JahEmitterCone jahEmitterConeFrom( JahEmitter e, vec3 x )
{
	JahEmitterCone k;
	k.axis = vec3( 0.0, 0.0, 1.0 );
	k.cosMax = 1.0;
	k.omega = 0.0;
	k.inside = false;
	const vec3 d = e.c - x;
	const float d2 = dot( d, d );
	const float r2 = e.r * e.r;
	if( !( r2 > 0.0 ) || !( d2 >= 0.0 ) )
		return k;
	if( d2 <= r2 )
	{
		k.inside = true;
		k.cosMax = 0.0;
		k.omega = 6.2831853;
		return k;
	}
	const float dist = sqrt( d2 );
	k.axis = d / dist;
	const float sin2 = r2 / d2;
	k.cosMax = sqrt( max( 1.0 - sin2, 0.0 ) );
	// 2 pi ( 1 - cos ) without the cancellation: 2 pi sin^2 / ( 1 + cos )
	k.omega = 6.2831853 * sin2 / ( 1.0 + k.cosMax );
	return k;
}
/// L in S_e (L unit).
bool jahEmitterConeHolds( JahEmitterCone k, vec3 g, vec3 L )
{
	return k.inside ? dot( L, g ) > 0.0 : dot( L, k.axis ) >= k.cosMax;
}
/// THE GATE G_e: the set reaches above the geometric plane (a target wholly below it can send nothing the surface
/// reflects). NO LOBE GATE, measured: a GGX lobe's tail is heavy - the mass of its normals past a half-angle theta is
/// alpha^2 / ( alpha^2 + tan^2 theta ), 3.8 % past 2 atan( 5 alpha ) - and a gate there drew a SEAM on photon-firefly's
/// plate while the rest converged (inside it the NEE's mean, outside the BRDF's alone, whose typical texel sits far
/// under its mean until a firefly lands): unbiased both sides, a visible circle for hundreds of frames. `R` and `alpha`
/// are kept for a later gate that is a measured bar, never a guess.
bool jahEmitterGate( JahEmitterCone k, vec3 g, vec3 R, float alpha )
{
	if( !( k.omega > 0.0 ) )
		return false;
	if( k.inside )
		return true;
	const float sinMax = sqrt( max( 1.0 - k.cosMax * k.cosMax, 0.0 ) );
	return dot( k.axis, g ) >= -sinMax;   // (wholly below the surface: nothing)
}

/// THE PICK at x: W, the light density of the BRDF draw's direction Lb (p_l( Lb ) - its balance weight's), the chosen
/// target and its draw's direction L (uniform in its set: u.x the pick, u.yz the direction), and p_l( L ) - over every
/// target whose set holds L, not the chosen one's alone. ok false: no target reaches (W 0) - the texel takes the BRDF's
/// draw alone (its weight 1). `kind`: this strategy's kind - the emitters, or the sky's portals (never the images: theirs
/// is jahImagePick's component-wise draw).
struct JahNeePick
{
	bool ok;
	float W;
	float pdfB;   // p_l( Lb )
	vec3 L;
	float pdfL;   // p_l( L )
	uint slot;    // the chosen target's (the hit-rate counter's)
};
JahNeePick jahNeePick( vec3 x, vec3 g, vec3 R, float alpha, vec3 Lb, bool haveLb, vec3 u, uint kind )
{
	JahNeePick o;
	o.ok = false;
	o.W = 0.0;
	o.pdfB = 0.0;
	o.L = g;
	o.pdfL = 0.0;
	o.slot = 0xFFFFFFFFu;
	const uint n = jahEmitterCount();
	if( n == 0u )
		return o;
	// (1) W and the BRDF direction's sum
	float sumB = 0.0;
	for( uint i = 0u; i < n; ++i )
	{
		const JahEmitter e = jahEmitterAt( i );
		const JahEmitterCone k = jahEmitterConeFrom( e, x );
		if( jahEmitterKindOf( e.slot ) != kind || !( e.lmax > 0.0 ) || !jahEmitterGate( k, g, R, alpha ) )
			continue;
		o.W += e.lmax * k.omega;
		if( haveLb && jahEmitterConeHolds( k, g, Lb ) )
			sumB += e.lmax;
	}
	if( !( o.W > 0.0 ) || !( o.W < 3.0e38 ) )
		return o;
	o.pdfB = sumB / o.W;
	// (2) THE PICK, and its draw
	// (the last gated target takes the remainder: a sum's rounding never leaves the pick empty)
	const float target = u.x * o.W;
	float acc = 0.0;
	bool picked = false, any = false;
	JahEmitterCone chosen;
	for( uint i = 0u; i < n && !picked; ++i )
	{
		const JahEmitter e = jahEmitterAt( i );
		const JahEmitterCone k = jahEmitterConeFrom( e, x );
		if( jahEmitterKindOf( e.slot ) != kind || !( e.lmax > 0.0 ) || !jahEmitterGate( k, g, R, alpha ) )
			continue;
		acc += e.lmax * k.omega;
		chosen = k;
		o.slot = e.slot;
		any = true;
		picked = target < acc;
	}
	if( !any )
		return o;
	{
		// uniform in the cone around its axis (cos theta uniform on [cosMax, 1]), or in the hemisphere above g
		const vec3 axis = chosen.inside ? g : chosen.axis;
		const float cosMax = chosen.inside ? 0.0 : chosen.cosMax;
		const float ct = 1.0 - u.y * ( 1.0 - cosMax );
		const float st = sqrt( max( 1.0 - ct * ct, 0.0 ) );
		const float phi = 6.2831853 * u.z;
		vec3 T, B;
		jahReflectFrame( axis, T, B );
		o.L = normalize( ( cos( phi ) * st ) * T + ( sin( phi ) * st ) * B + ct * axis );
	}
	// (3) p_l( L ) over every set holding it
	float sumL = 0.0;
	for( uint i = 0u; i < n; ++i )
	{
		const JahEmitter e = jahEmitterAt( i );
		const JahEmitterCone k = jahEmitterConeFrom( e, x );
		if( jahEmitterKindOf( e.slot ) != kind || !( e.lmax > 0.0 ) || !jahEmitterGate( k, g, R, alpha ) )
			continue;
		if( jahEmitterConeHolds( k, g, o.L ) )
			sumL += e.lmax;
	}
	o.pdfL = sumL / o.W;
	o.ok = o.pdfL > 0.0;
	return o;
}

/// THE IMAGE DRAW (PHOTON-7 11-2 (8), the lead's ruling): C2's second strategy toward a lamp's image in a planar
/// near-mirror. Its component is ONE pair's - that lamp's lobe at that mirror (jahCardLampLobe) - so its density for a
/// direction of image k is P( k ) / Omega_k = lmax_k / W, never a sum over the other images (each estimates its own
/// component). An image counts at x where its cone reaches above g and x lies on the mirror's front (the plane through
/// the lamp and its image's midpoint, its normal toward the lamp). `u` the pick and the cone's two.
struct JahImage
{
	vec3 c;
	float r;
	float lmax;
	uint mirror;
	uint lamp;
};
JahImage jahImageAt( uint i )
{
	JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
	const uint b = T.u[5] + i * kJahEmitterWords;
	JahImage m;
	m.c = uintBitsToFloat( uvec3( T.u[b], T.u[b + 1u], T.u[b + 2u] ) );
	m.r = uintBitsToFloat( T.u[b + 3u] );
	m.lmax = uintBitsToFloat( T.u[b + 4u] );
	m.mirror = T.u[b + 5u];
	m.lamp = T.u[b + 6u];
	return m;
}
/// ...the image's lamp's position (the light table's first vec4)
vec3 jahImageLampPos( JahImage m )
{
	JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
	const uint o = T.u[2] + m.lamp * kJahHitLightWords;
	return uintBitsToFloat( uvec3( T.u[o], T.u[o + 1u], T.u[o + 2u] ) );
}
bool jahImageCounts( JahImage m, JahEmitterCone k, vec3 x, vec3 g )
{
	if( !( m.lmax > 0.0 ) || !( k.omega > 0.0 ) || k.inside )
		return false;
	const vec3 lp = jahImageLampPos( m );
	const vec3 n = lp - m.c;
	if( !( dot( x - 0.5 * ( lp + m.c ), n ) > 0.0 ) )
		return false;
	const float sinMax = sqrt( max( 1.0 - k.cosMax * k.cosMax, 0.0 ) );
	return dot( k.axis, g ) >= -sinMax;
}
struct JahImagePick
{
	bool ok;
	float W;      // sum over the counting images of lmax Omega
	uint index;   // the chosen image's table index
	vec3 L;
	float pdf;    // its component's density at L: lmax / W
};
JahImagePick jahImagePick( vec3 x, vec3 g, vec3 u )
{
	JahImagePick o;
	o.ok = false;
	o.W = 0.0;
	o.index = 0u;
	o.L = g;
	o.pdf = 0.0;
	const uint n = jahEmitterCount();
	for( uint i = 0u; i < n; ++i )
	{
		JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
		if( T.u[T.u[5] + i * kJahEmitterWords + 7u] != kJahImageSlot )
			continue;
		const JahImage m = jahImageAt( i );
		JahEmitter e;
		e.c = m.c;
		e.r = m.r;
		const JahEmitterCone k = jahEmitterConeFrom( e, x );
		if( jahImageCounts( m, k, x, g ) )
			o.W += m.lmax * k.omega;
	}
	if( !( o.W > 0.0 ) || !( o.W < 3.0e38 ) )
		return o;
	const float target = u.x * o.W;
	float acc = 0.0;
	bool picked = false;
	JahEmitterCone chosen;
	float lmax = 0.0;
	for( uint i = 0u; i < n && !picked; ++i )
	{
		JahHitTableRef T = JahHitTableRef( JAH_HIT_TABLE );
		if( T.u[T.u[5] + i * kJahEmitterWords + 7u] != kJahImageSlot )
			continue;
		const JahImage m = jahImageAt( i );
		JahEmitter e;
		e.c = m.c;
		e.r = m.r;
		const JahEmitterCone k = jahEmitterConeFrom( e, x );
		if( !jahImageCounts( m, k, x, g ) )
			continue;
		acc += m.lmax * k.omega;
		chosen = k;
		lmax = m.lmax;
		o.index = i;
		o.ok = true;
		picked = target < acc;
	}
	if( !o.ok )
		return o;
	const float ct = 1.0 - u.y * ( 1.0 - chosen.cosMax );
	const float st = sqrt( max( 1.0 - ct * ct, 0.0 ) );
	const float phi = 6.2831853 * u.z;
	vec3 T, B;
	jahReflectFrame( chosen.axis, T, B );
	o.L = normalize( ( cos( phi ) * st ) * T + ( sin( phi ) * st ) * B + ct * chosen.axis );
	o.pdf = lmax / o.W;
	return o;
}
/// ...image i's component density at a BRDF draw's direction Lb from x (its cone's: lmax_i / W where Lb lies in it, else
/// 0); `W` jahImagePick's at the same x.
float jahImagePdfAt( uint i, vec3 x, vec3 g, vec3 Lb, float W )
{
	if( !( W > 0.0 ) )
		return 0.0;
	const JahImage m = jahImageAt( i );
	JahEmitter e;
	e.c = m.c;
	e.r = m.r;
	const JahEmitterCone k = jahEmitterConeFrom( e, x );
	return jahImageCounts( m, k, x, g ) && jahEmitterConeHolds( k, g, Lb ) ? m.lmax / W : 0.0;
}

/// THE NEE's THREE DRAWS (the pick, the direction's two), keyed on the sample's seed pixel and index like the lobe's draw
/// (jah_reflect_draw.glsl): the rest stays deterministic (its sample index is the sequence's only input).
vec3 jahNeeSample3( ivec2 pix, uint frame )
{
	const uint h0 = jahReflectPcg( uint( pix.x ) * 2246822519u + uint( pix.y ) * 3266489917u + frame * 668265263u +
								   0x3C6EF372u );
	const uint h1 = jahReflectPcg( h0 ^ 0xA54FF53Au );
	return vec3( float( h0 & 0xFFFFFFu ) * ( 1.0 / 16777216.0 ), float( h1 & 0xFFFFu ) / 65536.0,
				 float( h1 >> 16u ) / 65536.0 );
}

#endif   // JAH_RQ_EMITTERS_GLSL
