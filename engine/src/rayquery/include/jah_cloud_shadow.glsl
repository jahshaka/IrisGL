// THE CLOUD LAYER'S SHADOW — the fraction of the sun's beam that reaches a
// world point through the 2D cloud sheet (CLOUDS-2D-1 / CLOUDS-2D-2). ONE copy,
// read by the pixel's first-light and non-caster factors (JahFog piece), the
// voxel light injection (LightInjection) and the surface cache's card relight
// (JahCardLight), so a pixel, a voxel and a card under one overcast are
// darkened by one factor.
//
// The sun ray from the point meets the sheet (altitude cloudSun.z) at the
// point's world xz thrown towards the sun by (altitude - y) / tan(elevation)
// (cloudSun.xy = toSun.xz / toSun.y); there the beam crosses exp( -tau / mu_s )
// of the sheet (cloudSun.w = 1 / mu_s), tau read from the baked field tile
// (cloudMap.x = 1 / the tile in metres, cloudMap.zw = the scroll in metres);
// cloudMap.y is the shadow strength (0 = no shadow, 1 = the real one).
//
// The caller defines JAH_CLOUD_TAU( uv ) — the field's optical depth at a tile
// coordinate — with its own texture and sampler, before inserting this.
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahCloudShadow):
// no character of it may be the Hlms directive mark, comments included.

float jahCloudTransmittance( vec3 worldPos, vec4 cloudMap, vec4 cloudSun )
{
	vec2 at = worldPos.xz + cloudSun.xy * max( cloudSun.z - worldPos.y, 0.0 );
	vec2 uv = ( at + cloudMap.zw ) * cloudMap.x;
	float t = exp( -JAH_CLOUD_TAU( uv ) * cloudSun.w );
	return mix( 1.0, t, cloudMap.y );
}
