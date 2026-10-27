// THE PANES A RAY CROSSED, FOLDED (PHOTON-7 10-4 R7; L9: a transparent surface transmits a ray, attenuated by its
// material). The rays that read light (the caches' gathers, the screen gather, the reflection) list the panes they
// crossed (jah_rq_alpha.glsl's JAH_RQ_PROCEED_PANES); after the traversal the panes nearer than the committed opaque
// hit - or than the ray's end, on a miss - are taken in crossing order:
//   THE THROUGHPUT past them all is the product of their T (Ogre's blend passes 1 - alpha behind Fade, Transparent and
//   Refractive alike: OgreScene::gpuPaneWordFor);
//   EACH PANE's OWN LIGHT is its share (alpha) of the light the store's row shades it with at the crossing - the
//   row carries kD x alpha for Transparent / Refractive, so a glass sends alpha^2 kD as its pixel does, a Fade alpha kD
//   - weighted by the throughput of the panes in front of it. A pane whose light the store cannot answer (no sample)
//   adds none and still transmits.
// The caller adds own + throughput x (the committed hit's light, or the sky over a miss). Needs the hit-radiance
// includes (jahHitRadianceStore, jahHitWoundNormal, JAH_HIT_INSTANCE) and jah_rq_alpha.glsl.
// The includer enables GL_EXT_control_flow_attributes (the fold's loop is never unrolled).
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_PANES_GLSL
#define JAH_RQ_PANES_GLSL

/// The pane i's object-to-world transform: the GPU scene's instance rows (the TLAS copies them, rq_tlas_write.comp).
mat4x3 jahPaneToWorld( uint slot )
{
	const vec4 r0 = uintBitsToFloat( JAH_HIT_INSTANCE( slot, 0u ) );
	const vec4 r1 = uintBitsToFloat( JAH_HIT_INSTANCE( slot, 1u ) );
	const vec4 r2 = uintBitsToFloat( JAH_HIT_INSTANCE( slot, 2u ) );
	return transpose( mat3x4( r0, r1, r2 ) );
}

/// THE FOLD: `p` already reduced to the panes in front (jahPanesInFront); `origin` and unit `dir` the ray's,
/// `spread` its footprint per metre. Returns the panes' own light; `throughput` the product of their T.
vec3 jahPanesOwnLight( JahPanes p, vec3 origin, vec3 dir, float spread, out float throughput )
{
	vec3 own = vec3( 0.0 );
	float T = 1.0;
	// NEVER UNROLLED: the body inlines the whole store's shading (jahHitRadianceStore), and the list's bound is a
	// constant (8): unrolled, every caller's pipeline compiled that shading eight times over (rq_reflect 15 s cold
	// against its 2.9 s, rq_voxel_sides 4.0 s against 1.0 s - PHOTON-7 10-4 R7's fold, found by the app rows' cold boots)
	[[dont_unroll]] for( int i = 0; i < p.n; ++i )
	{
		const uint w = p.word[i];
		const float share = jahPaneOwn( w );
		if( share > 0.0 && T > 0.0 )
		{
			const uint slot = p.slotFar[i] & 0xFFFFFFu;
			const uint geom = ( p.slotFar[i] >> 24u ) & 127u;
			const bool far = ( p.slotFar[i] >> 31u ) != 0u;
			const mat4x3 toWorld = jahPaneToWorld( slot );
			vec3 wound = -dir;
			// (a far copy's triangle is the coarsest level's: no near row names it - the pane faces the ray)
			const bool known = !far && jahHitWoundNormal( slot, geom, p.prim[i], toWorld, wound );
			const vec3 hitN = known ? ( dot( wound, dir ) > 0.0 ? -wound : wound ) : -dir;
			jahHitSurf = JahHitSurface( slot, geom, p.prim[i], unpackHalf2x16( p.bary[i] ), toWorld, p.t[i], far, known );
			bool ok = false;
			uint source = 0u;
			const vec3 L = jahHitRadianceStore( far ? 0xFFFFFFFFu : slot, origin + dir * p.t[i], hitN, dir,
												max( p.t[i] * spread, 1e-4 ), false, ok, source );
			if( ok && finite3( L ) )
				own += T * share * L;
		}
		T *= jahPaneT( w );
	}
	throughput = T;
	return own;
}

#endif   // JAH_RQ_PANES_GLSL
