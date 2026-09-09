// PER-MAP SHADOW-ATLAS CLEAR (SPECS/SHADOW_TOOLING_SPEC.md phase 0, option B2).
//
// Writes the FAR depth value over exactly one shadow map's rectangle of the
// shared depth atlas. It exists because a compositor PASS_CLEAR cannot do that:
// on Vulkan a clear is a render-pass load action whose render area is the whole
// attachment (VkRenderPassBeginInfo.renderArea = 0,0,targetWidth,targetHeight —
// OgreVulkanRenderPassDescriptor.cpp), so upstream's one-clear-per-atlas would
// wipe every static shadow map in the atlas every frame, which is why upstream
// says "do not put static and dynamic shadow maps in the same UV atlas".
//
// A quad pass CAN: any compositor pass carrying a valid mShadowMapIdx gets its
// viewport and scissor fitted to that map's UV rect
// (CompositorShadowNodeDef::_validateAndFinish), which is how the point-light
// DPSM copy already writes one map's rect of this same atlas. This shader is
// that copy with the cubemap fetch removed.
//
// FAR, not zero: under reverse depth (Vulkan's default here) the far plane is
// 0.0 and Ogre's own clear does `1.0 - clearDepth` in the render system; on a
// non-reverse-depth backend it is 1.0. The C++ picks the material variant that
// matches RenderSystem::isReverseDepth(), so the value written here is always
// the same one upstream's whole-atlas clear would have written.
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
	gl_FragDepth = 0.0;
#else
	gl_FragDepth = 1.0;
#endif
}
