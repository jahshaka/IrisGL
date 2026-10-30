// Heavily modified version based on voxelization shader written by mattatz
//	https://github.com/mattatz/unity-voxel
// Original work Copyright (c) 2018 mattatz under MIT license.
//
// mattatz's work is a full voxelization algorithm.
// We only need to voxelize the shell (aka the contour),
// not to fill the entire voxel.
//
// Adapted for Ogre and for use for Voxel Cone Tracing by
// Matias N. Goldberg Copyright (c) 2019

@insertpiece( SetCrossPlatformSettings )

@insertpiece( DeclUavCrossPlatform )

@piece( CustomGlslExtensions )
	// Jahshaka (ATOM P4): the geometry is read where the raster keeps it, through
	// buffer device addresses - see the JahGeomRows piece. The uvec2 form is
	// deliberate: shaderInt64 is not an enabled device feature.
	#extension GL_EXT_buffer_reference: require
	#extension GL_EXT_buffer_reference_uvec2: require
@end

// (THE GROUP VOTE AND ITS EMULATION ARE DELETED, ENGINE-UPGRADE-U5 decision 12.5: a lane owns a triangle and
// skips it by its own box, Voxelizer_piece_cs.any.)

// Jahshaka (ENGINE-UPGRADE-U5 stage 2): THE GROUP'S PER-VOXEL SUMS (Voxelizer_piece_cs.any, the work
// partition): 36 integers for each of the 64 voxels - albedo 3 and the metal's Fresnel-90 weight, the raw
// (VOXEL-R13 stage 2: the raw and the folded normal deleted) the emissive 3, the roughness, the area weight, the coverage per half 3 + 3,
// the position per half 3 + 3, the casters' coverage per half 3 + 3, the metal's F0 3 (METAL-BOUNCE-1), the
// emitters' coverage per half 3 + 3 and their weight (JUNCTION-NORMAL-1), the kinds of faces (PHOTON-7 10-4 R7), and the
// halves' own faces 6 x 5 (VOXEL-R13) - indexed sum x 64 + voxel. 17 KB.
shared uint g_voxelSums[74 * 64];
// Jahshaka (PHOTON-7 stage 10-3, R2): THE EMISSIVE'S SHARED EXPONENT - each of the 64 voxels' brightest emissive
// contribution (float bits: a non-negative float orders as its bits). The max pre-pass (voxel_emissive_max) gathers it
// into the merge accumulator's channel 26; the sum pass reads it back here and quantises the emissive relative to it.
shared uint g_voxelEmissiveMax[64];

@insertpiece( PreBindingsHeaderCS )

@property( syntax == glsl )
	#define ogre_U0 binding = 0
	#define ogre_U1 binding = 1
@end

// Jahshaka (ATOM P4): U0 IS THE GEOMETRY TABLE. It replaces the two slots that used
// to bind a private vertex copy and a private index copy - and with them the
// `compressed_vertex_format` and `index_32bit` shader properties, since a dispatch no
// longer binds a format. Every image below therefore moved down a slot (two, with U1).
layout( std430, ogre_U0 ) readonly restrict buffer geometryTableLayout
{
	GeometryRow geometryTable[];
};

// Jahshaka (ATOM P4b): THE RANGES - one (start, count) per (octant, bucket), written
// on the device by the host's gather. This dispatch loops over ranges[rangeIdx]; the
// count is only a LOOP BOUND, because the dispatch's thread count is the octant's and
// never the instance count's, so no indirect dispatch is needed to take it from the GPU.
// U1 and not after the images: the root layout packs UAV buffers and UAV images as
// two contiguous ranges.
layout( std430, ogre_U1 ) readonly restrict buffer rangesLayout
{
	uvec2 ranges[];
};

layout( vulkan( ogre_u2 ) vk_comma @insertpiece(uav2_pf_type) )
uniform restrict image3D voxelAlbedoTex;
layout( vulkan( ogre_u3 ) vk_comma @insertpiece(uav3_pf_type) )
uniform restrict image3D voxelEmissiveTex;
layout( vulkan( ogre_u4 ) vk_comma @insertpiece(uav4_pf_type) )
uniform restrict uimage3D voxelAccumVal;
// Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): the per-voxel INTEGER ACCUMULATOR the merge sums into.
layout( vulkan( ogre_u5 ) vk_comma @insertpiece(uav5_pf_type) )
uniform restrict uimage3D voxelMergeAccum;
// Jahshaka (PHOTON-VOXEL-3/-4): THE PER-HALF-AXIS COVERAGE - O_a+ (the faces looking +a)
// and O_a- (looking -a), written by the resolve (VoxelMerge_piece_cs.any, THE DIRECTIONAL
// COVERAGE); the light injection, the anisotropic mip step 0 and every read sample it.
layout( vulkan( ogre_u6 ) vk_comma @insertpiece(uav6_pf_type) )
uniform restrict writeonly image3D voxelCoveragePTex;
layout( vulkan( ogre_u7 ) vk_comma @insertpiece(uav7_pf_type) )
uniform restrict writeonly image3D voxelCoverageNTex;
// Jahshaka (PHOTON-VOXEL-4; PHOTON-7 stage 1): THE SURFACE POSITION per half-axis, O-premultiplied,
// RELATIVE TO THE VOXEL'S OWN CELL (VoxelMerge_piece_cs.any, THE SURFACE POSITION).
layout( vulkan( ogre_u8 ) vk_comma @insertpiece(uav8_pf_type) )
uniform restrict writeonly image3D voxelPositionPTex;
layout( vulkan( ogre_u9 ) vk_comma @insertpiece(uav9_pf_type) )
uniform restrict writeonly image3D voxelPositionNTex;
// Jahshaka (METAL-BOUNCE-1): THE METAL - the F0 (rgb) and Fresnel-90 weight (alpha) of every surface's
// specular lobe, area means written by the resolve beside the albedo (VoxelMerge_piece_cs.any).
layout( vulkan( ogre_u10 ) vk_comma @insertpiece(uav10_pf_type) )
uniform restrict writeonly image3D voxelMetalTex;
// Jahshaka (JUNCTION-NORMAL-1): THE EMISSIVE FACTOR PER HALF-AXIS - what each half's faces emit over the
// voxel's area mean, written by the resolve (VoxelMerge_piece_cs.any, THE EMISSIVE PER HALF-AXIS).
layout( vulkan( ogre_u11 ) vk_comma @insertpiece(uav11_pf_type) )
uniform restrict writeonly image3D voxelEmitPTex;
layout( vulkan( ogre_u12 ) vk_comma @insertpiece(uav12_pf_type) )
uniform restrict writeonly image3D voxelEmitNTex;
// VOXEL-R13 stage 1: THE HALVES' OWN FACES per axis - the centroid across the axis (14..16) and the normal (17..19)
// of both halves, written by the resolve (VoxelMerge_piece_cs.any, THE HALF'S OWN FACES).
layout( vulkan( ogre_u13 ) vk_comma @insertpiece(uav13_pf_type) )
uniform restrict writeonly image3D voxelHalfSideXTex;
layout( vulkan( ogre_u14 ) vk_comma @insertpiece(uav14_pf_type) )
uniform restrict writeonly image3D voxelHalfSideYTex;
layout( vulkan( ogre_u15 ) vk_comma @insertpiece(uav15_pf_type) )
uniform restrict writeonly image3D voxelHalfSideZTex;
layout( vulkan( ogre_u16 ) vk_comma @insertpiece(uav16_pf_type) )
uniform restrict writeonly image3D voxelHalfNrmXTex;
layout( vulkan( ogre_u17 ) vk_comma @insertpiece(uav17_pf_type) )
uniform restrict writeonly image3D voxelHalfNrmYTex;
layout( vulkan( ogre_u18 ) vk_comma @insertpiece(uav18_pf_type) )
uniform restrict writeonly image3D voxelHalfNrmZTex;



layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

//layout (rgba8) uniform restrict writeonly image3D voxelAlbedoTex;
//layout (rgba8) uniform restrict writeonly image3D voxelEmissiveTex;

//layout( local_size_x = 4,
//		local_size_y = 4,
//		local_size_z = 4 ) in;

@property( syntax == glsl )
	ReadOnlyBufferF( 7, InstanceBuffer, instanceBuffer );
@else
	ReadOnlyBufferF( 0, InstanceBuffer, instanceBuffer );
@end

@property( has_diffuse_tex || has_emissive_tex )
	vulkan_layout( ogre_t1 ) uniform texture2DArray texturePool;
	vulkan( layout( ogre_s1 ) uniform sampler poolSampler );
@end


// VOXEL-R13 stage 1: the half-axes' one text (jah_voxel_halves.glsl), before the merge's resolve that encodes a half's normal
@insertpiece( JahVoxelHalves )
@insertpiece( DeclVoxelMerge )

@insertpiece( HeaderCS )


vulkan( layout( ogre_P0 ) uniform Params { )
	uniform uint2 rangeIdx_pad;
	uniform float3 voxelOrigin;
	uniform float3 voxelCellSize;
	uniform uint3 voxelPixelOrigin;
	uniform uint3 voxelPixelsToWrite;
	// PHOTON-7 stage 2: WHERE A VOXEL IS STORED - its cell (voxelPixelOrigin + the lane's id) PERMUTED by w (0: the
	// volume's own (x, y, z); 1: (y, x, z); 2: (z, x, y)) plus xyz. A whole build stores at the cell (0, 0); a scroll's
	// chunk stores into the cascade's slab-sized scratch, its slab's axis first (PhotonComponent::planScroll).
	uniform int4 voxelStoreBase;
vulkan( }; )

#define p_rangeIdx rangeIdx_pad.x
#define p_voxelOrigin voxelOrigin
#define p_voxelCellSize voxelCellSize
#define p_voxelPixelOrigin voxelPixelOrigin
#define p_voxelPixelsToWrite voxelPixelsToWrite
#define p_voxelStoreBase voxelStoreBase

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
