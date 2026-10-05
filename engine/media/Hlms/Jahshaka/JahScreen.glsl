// SCREEN TEXTURES IN A MULTIVIEW CHAIN (LAYERED-STEREO-1).
//
// A stereo view renders each eye into its own layer of a two-layer array, at
// the same origin, through VK_KHR_multiview: every pass of its chain runs once
// and the render pass broadcasts it to both layers, and a fragment learns its
// eye from gl_ViewIndex. A post-chain quad therefore reads its screen-sized
// input as an ARRAY, at its own view. The chain builds the multiview twin of a
// quad's fragment program by adding JAH_MULTIVIEW=1 (OgreChain.cpp,
// multiviewMaterial); the mono program is the same text without it, and these
// macros expand to exactly the old calls.
//
// Include it FIRST, right after the version line: it carries an extension
// directive.
#ifdef JAH_MULTIVIEW
	#extension GL_EXT_multiview : require
	#define JahScreenTexture texture2DArray
	#define jahScreenSampler( t, s ) vkSampler2DArray( t, s )
	#define jahScreenUv( uv ) vec3( ( uv ), float( gl_ViewIndex ) )
	#define jahScreenTexel( p ) ivec3( ( p ), int( gl_ViewIndex ) )
	#define jahScreenSize( s, l ) textureSize( s, l ).xy
#else
	#define JahScreenTexture texture2D
	#define jahScreenSampler( t, s ) vkSampler2D( t, s )
	#define jahScreenUv( uv ) ( uv )
	#define jahScreenTexel( p ) ( p )
	#define jahScreenSize( s, l ) textureSize( s, l )
#endif
