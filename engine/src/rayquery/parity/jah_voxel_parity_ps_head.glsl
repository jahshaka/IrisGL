#version ogre_glsl_ver_450
// engine.voxel_reader_parity's FRAGMENT half (PHOTON-READER-1). GENERATED at
// media staging: this head, then jah_rq_finite.glsl, jah_voxel_sample.glsl,
// jah_voxel_march.glsl and jah_voxel_parity.glsl verbatim, then the main - so
// the pixel stage runs exactly the text the pixel shader's cones, the bounce job
// and the irradiance field run (irisgl/engine/CMakeLists.txt).
//
// The volumes: twenty-four units, every cascade's isotropic volume, then every
// cascade's X, Y, Z, then every cascade's per-axis coverage, then every cascade's
// surface position (the generation job's order), up to four cascades; one
// sampler, the chain's own trilinear samplerblock. Sixteen textures is past
// Ogre's standard prefab root layout (four): the program declaration asks for
// the "max" prefab (JahVoxelReaderParity.material).


vulkan_layout( ogre_t0 ) uniform texture3D vIso0;
vulkan_layout( ogre_t1 ) uniform texture3D vIso1;
vulkan_layout( ogre_t2 ) uniform texture3D vIso2;
vulkan_layout( ogre_t3 ) uniform texture3D vIso3;
vulkan_layout( ogre_t4 ) uniform texture3D vX0;
vulkan_layout( ogre_t5 ) uniform texture3D vX1;
vulkan_layout( ogre_t6 ) uniform texture3D vX2;
vulkan_layout( ogre_t7 ) uniform texture3D vX3;
vulkan_layout( ogre_t8 ) uniform texture3D vY0;
vulkan_layout( ogre_t9 ) uniform texture3D vY1;
vulkan_layout( ogre_t10 ) uniform texture3D vY2;
vulkan_layout( ogre_t11 ) uniform texture3D vY3;
vulkan_layout( ogre_t12 ) uniform texture3D vZ0;
vulkan_layout( ogre_t13 ) uniform texture3D vZ1;
vulkan_layout( ogre_t14 ) uniform texture3D vZ2;
vulkan_layout( ogre_t15 ) uniform texture3D vZ3;
// PHOTON-VOXEL-3/-4: every cascade's per-half-axis coverage (+a, -a) and surface position
// (+a, -a), the light-volume list's last kinds.
vulkan_layout( ogre_t16 ) uniform texture3D vCovP0;
vulkan_layout( ogre_t17 ) uniform texture3D vCovP1;
vulkan_layout( ogre_t18 ) uniform texture3D vCovP2;
vulkan_layout( ogre_t19 ) uniform texture3D vCovP3;
vulkan_layout( ogre_t20 ) uniform texture3D vCovN0;
vulkan_layout( ogre_t21 ) uniform texture3D vCovN1;
vulkan_layout( ogre_t22 ) uniform texture3D vCovN2;
vulkan_layout( ogre_t23 ) uniform texture3D vCovN3;
vulkan_layout( ogre_t24 ) uniform texture3D vPosP0;
vulkan_layout( ogre_t25 ) uniform texture3D vPosP1;
vulkan_layout( ogre_t26 ) uniform texture3D vPosP2;
vulkan_layout( ogre_t27 ) uniform texture3D vPosP3;
vulkan_layout( ogre_t28 ) uniform texture3D vPosN0;
vulkan_layout( ogre_t29 ) uniform texture3D vPosN1;
vulkan_layout( ogre_t30 ) uniform texture3D vPosN2;
vulkan_layout( ogre_t31 ) uniform texture3D vPosN3;

vulkan( layout( ogre_s0 ) uniform sampler vSmp );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 chainInvRes[8];
	uniform vec4 chainFromPrev[14];
	uniform vec4 cones[256];
	uniform vec4 counts;	// x = cones, y = cascades, z = anisotropic
	uniform vec4 sdf;		// x = the SDF skip's max mip, y = its factor
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

#define JAH_PARITY_PICK( base, c, u, l ) ( ( c ) == 0 ? textureLod( sampler3D( base##0, vSmp ), u, l ) : ( c ) == 1 ? textureLod( sampler3D( base##1, vSmp ), u, l ) : ( c ) == 2 ? textureLod( sampler3D( base##2, vSmp ), u, l ) : textureLod( sampler3D( base##3, vSmp ), u, l ) )
#define JAH_VOX_MAX_CASCADES 4
#define JAH_VOX_COUNT int( counts.y )
#define JAH_VOX_HAS_ANISO 1
#define JAH_VOX_ANISO ( counts.z > 0.5 )
#define JAH_VOX_SAMPLE_ISO( c, u, l ) JAH_PARITY_PICK( vIso, c, u, l )
#define JAH_VOX_SAMPLE_X( c, u, l ) JAH_PARITY_PICK( vX, c, u, l )
#define JAH_VOX_SAMPLE_Y( c, u, l ) JAH_PARITY_PICK( vY, c, u, l )
#define JAH_VOX_SAMPLE_Z( c, u, l ) JAH_PARITY_PICK( vZ, c, u, l )
#define JAH_VOX_SAMPLE_COVP( c, u, l ) JAH_PARITY_PICK( vCovP, c, u, l )
#define JAH_VOX_SAMPLE_COVN( c, u, l ) JAH_PARITY_PICK( vCovN, c, u, l )
#define JAH_VOX_SAMPLE_POSP( c, u, l ) JAH_PARITY_PICK( vPosP, c, u, l )
#define JAH_VOX_SAMPLE_POSN( c, u, l ) JAH_PARITY_PICK( vPosN, c, u, l )
#define JAH_VOX_INVRES( c ) chainInvRes[c].xyz
#define JAH_VOX_MAXLOD( c ) chainInvRes[c].w
#define JAH_VOX_FROM_PREV_SCALE( c ) chainFromPrev[( (c) - 1 ) * 2]
#define JAH_VOX_FROM_PREV_OFFSET( c ) chainFromPrev[( (c) - 1 ) * 2 + 1]
#define JAH_VOX_SDF_MAXMIP sdf.x
#define JAH_VOX_SDF_FACTOR sdf.y
#define JAH_PARITY_CONE( i ) cones[i]
