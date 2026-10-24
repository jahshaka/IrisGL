// THE IRRADIANCE FIELD'S READER - the eight-probe cage, its visibility and its confidence, ONE TEXT (PHOTON-7 stage 7,
// 7-2): read by the PBS pixel's piece (PhotonIfd_piece_ps.any's applyIrradianceField - the refractives and every pass
// that still shades from the field) and by the screen-probe gather's integrate (rq_probe_integrate.comp: the field is
// the fallback of the pixels no probe answers, composed after the temporal mean - L4/L5). Ogre-Next's
// IrradianceField_piece_ps.any as it stood on fork 24b2436c9 (an owned copy since OWN-GI-1) with Jahshaka's five
// hooks folded in, in their order: the sample point's bias, the cage clamped to the read region, the window's slot,
// the front share, the confidence.
//
// THE CONSUMER DEFINES, before it includes this file (and jah_field_window.glsl):
//   JAH_FIELD_IRRAD( uv )   vec4: the irradiance atlas at uv, bilinear, level 0 (rgb E/pi, alpha the sample count)
//   JAH_FIELD_DEPTH( uv )   vec2: the depth atlas at uv, bilinear, level 0 (the mean distance and its square, grid units)
// and hands the cage its inputs IN GRID SPACE (one unit a probe spacing per axis: the field's world-to-grid rows):
//   gridPos      the shaded point, [0; numProbes)
//   gridNormal   the shading normal, normalised in grid space
//   gridViewDir  the direction towards the eye rotated into grid space, NOT re-normalised (an anisotropic grid biases
//                per axis: the sample point's bias below is in probe spacings)
//   counts       the probes per axis; counts.y <= 0.5 = the counts are absent (no clamp, no edge fade: upstream's
//                behaviour exactly - a state the host does not produce)
//   windowOffset the slot of the window's first probe per axis (jah_field_window.glsl)
//   numProbesAggregated  ( Nx, Nx * Ny ), the flat index's strides
//   depthAtlas / irradAtlas  ( the bordered tile resolution, the atlas width, 1 / width, 1 / height )
//
// THE ANSWER: `irradiance` is what the pixel ADDS to its diffuse (the cage's normalised sum times the confidence and
// the weight fade - upstream's composite line, ifdIrradiance x scale / sumValidWeight, or zero), and `confidence` the
// share of the pixel the field answers, in [0, 1]: its fallback takes the rest (the environment, or under a chain the
// cones - the consumer's).
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahFieldRead): no character of it may be the Hlms
// directive mark, comments included.

struct JahFieldAnswer
{
	vec3 irradiance;
	float confidence;
};

vec2 jahFieldOctWrap( vec2 v )
{
	vec2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return ( 1.0 - abs( v.yx ) ) * signVal;
}

vec2 jahFieldOctEncode( vec3 n )
{
	// https://twitter.com/Stubbesaurus/status/937994790553227264
	n /= ( abs( n.x ) + abs( n.y ) + abs( n.z ) );
	n.xy = n.z >= 0.0 ? n.xy : jahFieldOctWrap( n.xy );
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

/// The atlas uv of direction `dir` in probe `probeIdx`'s bordered tile (upstream's getIrradianceFieldXY).
vec2 jahFieldAtlasXY( vec3 dir, uint probeIdx, vec4 atlas )
{
	const float borderedRes = atlas.x, fullWidth = atlas.y;
	vec2 retVal;
	// ( probeIdx * borderedRes ) % fullWidth, ( ( probeIdx * borderedRes ) / fullWidth ) * borderedRes, in floats
	const float temp = float( probeIdx ) * borderedRes;
	retVal.y = floor( temp * atlas.z );
	retVal.x = temp - retVal.y * fullWidth;
	retVal.y *= borderedRes;
	// the tile's top-left border skipped
	retVal.xy += 1.0;
	// THE DIRECTION QUANTISED (upstream issue 29): the texels the octahedral encoding outputs otherwise keep
	// jumping where its wrap is too sharp
	dir = ceil( dir * 1048576.0 );
	dir *= 1.0 / 1048576.0;
	const vec2 uv = jahFieldOctEncode( dir ) * ( borderedRes - 2.0 );
	retVal += uv;
	retVal *= atlas.zw;
	return retVal;
}

JahFieldAnswer jahFieldRead( vec3 gridPos, vec3 gridNormal, vec3 gridViewDir, vec3 counts, uvec3 windowOffset,
							 vec2 numProbesAggregated, vec4 depthAtlas, vec4 irradAtlas )
{
	// ---- THE SAMPLE POINT (ENGINE-6 item 1; FIELD-EDGE-1): off the surface, in probe spacings -------------------
	// The cage looked up AT the shaded point evaluates the Chebyshev test on its own knife edge (a wall's two faces a
	// texel apart in the depth moments): the DDGI paper's answer is a MOVED SAMPLE POINT (Majercik et al. 2019 4.2),
	// here at a quarter of the paper's magnitude (the knee of the leak against the interior brightening, measured on the
	// PHOTON-S2 leak room). THE NORMAL SHARE IS CONSTANT - nb + vb, the head-on value - so the lookup point stands a fixed
	// height above the surface and one cage serves a whole floor; the view half keeps only its TANGENTIAL share, the part
	// that moves a grazing lookup towards the eye (the leak pair: a wall seen edge-on).
	const float ifdNormalBias = 0.05;   // probe spacings
	const float ifdViewBias   = 0.20;   // probe spacings
	const vec3 ifdViewTangent = gridViewDir - gridNormal * dot( gridViewDir, gridNormal );
	gridPos += gridNormal * ( ifdNormalBias + ifdViewBias ) + ifdViewTangent * ifdViewBias;

	// ---- THE READ REGION (PHOTON-7 stage 5, 5-5: the window's outer planes are its guard band, never read) ---------
	const bool ifdHaveCounts = counts.y > 0.5;
	// (an axis of fewer than four probes has no band: all of it is read - a field the host never steps)
	const vec3 ifdBand = ifdHaveCounts ? step( vec3( 3.5, 3.5, 3.5 ), counts ) : vec3( 0.0, 0.0, 0.0 );
	const vec3 ifdMinProbe = ifdBand;
	const vec3 ifdMaxProbe = ifdHaveCounts ? counts - vec3( 1.0, 1.0, 1.0 ) - ifdBand : vec3( 1e9, 1e9, 1e9 );
	// the share of the cage's weight from probes IN FRONT of the surface, and what their trilinear weights alone give
	float ifdFrontWeight = 0.0;
	float ifdFrontTrilinear = 0.0;

	const vec3 ifdGridProbeOrigin = trunc( gridPos );
	// how far from the floor of gridPos, on [0, 1] per axis
	const vec3 ifdAlpha = clamp( fract( gridPos ), 0.0, 1.0 );

	vec3 ifdIrradiance = vec3( 0.0, 0.0, 0.0 );
	float sumIfdWeight = 0.0;
	// (PHOTON-FIELD-ROTATE-1) the weight of the probes that HOLD a value: the composite divides by it
	float sumIfdValidWeight = 0.0;

	{
		for( int i = 0; i < 8; ++i )
		{
			// the cage's corner: 0 or 1 along each axis, CLAMPED to the read region (upstream's comment promised the clamp
			// and its render params could not perform it: a pixel outside the grid wrapped its flat index into another row)
			const vec3 probeOffset = vec3( ivec3( i, i >> 1, i >> 2 ) & ivec3( 1, 1, 1 ) );
			vec3 probeOrigin = ifdGridProbeOrigin + probeOffset;
			probeOrigin = clamp( probeOrigin, ifdMinProbe, ifdMaxProbe );
			// THE PROBE'S ATLAS SLOT (PHOTON-WRITER-1): the cage is built in the window's coordinates, the tile that holds
			// a probe is where the window's offset puts it
			vec3 ifdProbeSlot = probeOrigin;
			if( ifdHaveCounts )
				ifdProbeSlot = vec3( jahFieldSlot( uvec3( probeOrigin ), windowOffset, uvec3( counts ) ) );
			const uint probeIdx = uint( ifdProbeSlot.x + ifdProbeSlot.y * numProbesAggregated.x +
										ifdProbeSlot.z * numProbesAggregated.y );

			vec3 dir = probeOrigin - gridPos;
			// max(): a probe can sit exactly under the shaded point (the clamp makes it a half-space), and 1/0 is a NaN cage
			const float r = max( length( dir ), 1e-6 );
			dir *= 1.0 / r;

			// A PROBE BEHIND THE SHADED POINT'S TANGENT PLANE HAS NO SAY (PHOTON-WRITER-1): the cage always holds one on
			// the point's own side, and one behind an opaque surface measured the other side's light
			float weight = clamp( dot( dir, gridNormal ) * 200.0, 0.0, 1.0 );

			// the trilinear weights: 1 - a where the offset is 0, a where it is 1
			vec3 trilinear;
			trilinear.x = probeOffset.x < 1.0 ? ( 1.0 - ifdAlpha.x ) : ifdAlpha.x;
			trilinear.y = probeOffset.y < 1.0 ? ( 1.0 - ifdAlpha.y ) : ifdAlpha.y;
			trilinear.z = probeOffset.z < 1.0 ? ( 1.0 - ifdAlpha.z ) : ifdAlpha.z;

			// VISIBILITY (Chebyshev, on the probe's depth moments towards the point)
			const vec2 ifdDepthUV = jahFieldAtlasXY( -dir, probeIdx, depthAtlas );
			const vec2 temp = JAH_FIELD_DEPTH( ifdDepthUV );
			const float mean = temp.x;
			const float meanSq = temp.y;
			if( r > mean )
			{
				const float variance = abs( mean * mean - meanSq ) + 1e-6;
				const float dev = r - mean;
				float chebyshevWeight = variance / ( variance + dev * dev );
				chebyshevWeight = max( chebyshevWeight * chebyshevWeight * chebyshevWeight, 0.0 );
				weight *= chebyshevWeight;
			}

			// never a zero weight (the border probes), the crush below the threshold
			weight = max( 0.000001, weight );
			const float crushThreshold = 0.2;
			if( weight < crushThreshold )
				weight *= weight * weight * ( 1.0 / ( crushThreshold * crushThreshold ) );

			weight *= trilinear.x * trilinear.y * trilinear.z;

			const vec2 ifdColourUV = jahFieldAtlasXY( gridNormal, probeIdx, irradAtlas );
			const vec4 irrSample = JAH_FIELD_IRRAD( ifdColourUV );
			sumIfdWeight += weight;
			// A PROBE NO INTEGRATION HAS REACHED HAS NO SAY (PHOTON-FIELD-ROTATE-1): the atlas's alpha is the probe's sample
			// count (0 until its first integration)
			weight *= clamp( irrSample.w, 0.0, 1.0 );
			ifdIrradiance += irrSample.xyz * weight;
			sumIfdValidWeight += weight;

			// THE FRONT SHARE (P9): +0.5 separates BEHIND from COPLANAR - a probe in the shading surface keeps half its say
			const float ifdFrontSide = clamp( dot( dir, gridNormal ) * 200.0 + 0.5, 0.0, 1.0 );
			ifdFrontWeight += weight * ifdFrontSide;
			ifdFrontTrilinear += trilinear.x * trilinear.y * trilinear.z * ifdFrontSide;
		}
	}

	// ---- HOW MUCH THE FIELD IS TRUSTED (P9) ------------------------------------------------------------------------
	// (a) OUTSIDE THE READ REGION: full confidence at the region's face, faded to nothing half a probe spacing outside
	// it (SQUARE-1: a ramp inside the face drew a dark line where the cone term is not continuous)
	const vec3 ifdInside = min( gridPos - ifdMinProbe, ifdMaxProbe - gridPos );
	const float ifdEdgeRamp = 0.5;   // probe spacings, outside the face
	const vec3 ifdAxisFade = clamp( ifdInside * ( 1.0 / ifdEdgeRamp ) + 1.0, 0.0, 1.0 );
	float ifdEdgeFade = ifdAxisFade.x * ifdAxisFade.y * ifdAxisFade.z;
	ifdEdgeFade = ifdEdgeFade * ifdEdgeFade * ( 3.0 - 2.0 * ifdEdgeFade );
	// (b) NO PROBE IN FRONT OF THE SURFACE: the front share of the normalised weight (a scale-free ratio: a zero
	// divisor is "no share to compute", a share of one)
	const float ifdFrontLo = 0.005;
	const float ifdFrontHi = 0.05;
	const float ifdFrontFade = smoothstep( ifdFrontLo, ifdFrontHi,
										   sumIfdWeight > 0.0 ? ifdFrontWeight / sumIfdWeight : 1.0 );
	float ifdConfidence = min( ifdHaveCounts ? ifdEdgeFade : 1.0, ifdFrontFade );
	// ...times the cage's VALID share (PHOTON-2 J1): 1 in a field whose probes are whole
	ifdConfidence *= sumIfdWeight > 0.0 ? sumIfdValidWeight / sumIfdWeight : 1.0;
	// (c) NO PROBE OF THE CAGE CAN SEE THE POINT (ENGINE-6 item 1's second half): the cage's weight as a share of what
	// its front probes' trilinear weights alone give - a guard against a division, fading the field's answer to nothing
	// (a real answer: "nothing lights it from here"), never to the fallback
	const float ifdWeightLo = 0.0005;
	const float ifdWeightHi = 0.0050;
	const float ifdWeightFade = smoothstep( ifdWeightLo, ifdWeightHi,
											ifdFrontTrilinear > 0.0 ? sumIfdWeight / ifdFrontTrilinear : sumIfdWeight );

	JahFieldAnswer answer;
	answer.irradiance = vec3( 0.0, 0.0, 0.0 );
	answer.confidence = ifdConfidence;
	// THE COMPOSITE (upstream's line, the scale the confidence and the weight fade - every factor exactly 1 inside a
	// whole field): under the cage's own condition, sumIfdValidWeight being its divisor
	if( sumIfdValidWeight > 0.0 )
	{
		const vec3 ifdIrradianceScale = vec3( ifdConfidence * ifdWeightFade );
		answer.irradiance = ifdIrradiance * ( ifdIrradianceScale / sumIfdValidWeight );
	}
	return answer;
}
