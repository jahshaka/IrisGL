// THE DIFFUSE LOBE'S DIRECTIONAL ALBEDO — what a surface under HlmsPbs's
// normalised Disney diffuse (Frostbite's energy-conserving form: the energy
// factor lerp( 1, 1 / 1.51, r ) and the retro-reflective fd90 = 0.5 r +
// 2 r (V.H)^2 scatter terms) actually reflects of a unit irradiance, as a
// fraction of what a Lambertian surface of the same albedo reflects
// (PHOTON-WRITER-1, P4 items ROUGHNESS-INTO-THE-VOXEL-STORE and THE EXACT
// ENVIRONMENT FACTOR). Stated once, read by three consumers:
//
//   * the pixel's ENVIRONMENT lobe (BRDF_EnvMap, 200.BRDFs_piece_ps.any): every
//     diffuse irradiance that reaches envColourD (the sky's SH, the voxel cones,
//     the irradiance field, the screen-probe gather) is reflected with
//     jahDiffuseAlbedo( NdotV, r ) — the lobe integrated over the incoming
//     hemisphere for a UNIFORM environment, by reciprocity the same function of
//     the view angle. It replaces the constant energy factor that stood there
//     (0.662 at r = 1, where the lobe reflects 0.688 at normal view and 0.97
//     at a grazing one);
//   * the LIGHT INJECTION (LightInjection_piece_cs.any): a voxel's surface lit by
//     a lamp at cos theta_l re-emits jahDiffuseAlbedo( cos theta_l, r ) of the
//     Lambertian answer, integrated over every outgoing direction — the voxel
//     holds what its surface renders, not rho E / pi (which was 1.51x the
//     floor's own picture at r = 1);
//   * the BOUNCE (LightVctBounceInject_piece_cs.any): a voxel lit by the
//     diffuse gather of its surroundings re-emits the HEMISPHERICAL mean,
//     jahDiffuseAlbedoHemi( r ) = 2 * integral of jahDiffuseAlbedo( mu, r ) mu dmu.
//
// THE NUMBERS ARE A FIT TO OGRE'S OWN TABLE, not a new model: the third channel
// of the DFG lookup HlmsPbs samples when it has one bound (brtfLutDfg.dds, z,
// indexed ( NdotV, 1 - perceptual roughness )) IS this function — checked by
// direct integration of the lobe at seven points (within 0.5 %). The table is
// bound only while an area light exists (loadLtcMatrix is lazy, and it costs a
// texture slot in every pass), so the function is carried as a 13-term
// polynomial in r and t = 1 - v, least-squares over the table's 4,096 texels:
// worst 0.0096, rms 0.0017. The hemispherical mean is a cubic in r over the
// table's 64 rows: worst 0.0016 (0.702 at r = 1, 0.905 at r = 0).
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahDiffuseAlbedo):
// no character of it may be the Hlms directive mark, comments included.

float jahDiffuseAlbedo( float v, float r )
{
	v = clamp( v, 0.0, 1.0 );
	r = clamp( r, 0.0, 1.0 );
	float t = 1.0 - v;
	float t2 = t * t;
	float t5 = t2 * t2 * t;
	float r2 = r * r;
	float r3 = r2 * r;
	return 0.950057 - 0.236510 * r - 0.029870 * r2 + 0.004401 * r3
	     + t * ( -0.010296 - 0.066581 * r - 0.014983 * r2 )
	     + t2 * ( 0.016806 + 0.175365 * r )
	     + t5 * ( -0.934679 + 1.559543 * r - 0.308360 * r2 - 0.057401 * r3 );
}

float jahDiffuseAlbedoHemi( float r )
{
	r = clamp( r, 0.0, 1.0 );
	return 0.904896 + r * ( -0.155158 + r * ( -0.049610 + r * 0.001704 ) );
}
