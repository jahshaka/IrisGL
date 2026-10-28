// A POINT LIGHT's RECTANGLE OF atlasTD (PHOTON-7 10-4 fix round, the one read's F1; OgreShadow.cpp): the NEAR depth -
// every receiver is past it, so a point light's transmittance applies to every receiver as before (its panes are drawn
// into a cube and copied as colour: a named limit). NEAR, not far: under reverse depth the near plane is 1.0, on a
// non-reverse-depth backend 0.0 (the material variant the C++ picks). No colour. No directive mark anywhere in this file.
#version ogre_glsl_ver_330

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

in vec4 gl_FragCoord;

void main()
{
#ifdef REVERSE_DEPTH
	gl_FragDepth = 1.0;
#else
	gl_FragDepth = 0.0;
#endif
}
