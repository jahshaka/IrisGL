// THE FACES A RAY CROSSES IN ONE VOXEL — ONE MODEL for every march that passes through what a voxel holds (PHOTON-7 10-4
// R7; the lead's cell rule, 2026-10-27): the light injection's march to a lamp (LightInjection_piece_cs.any's
// jahInjectVisibility, SUN-INJECTION-RES-1 / ENGINE-UPGRADE-U5 decision 22.2, where this model was born) and the
// occupancy ray's TRANSMISSIVE cells (jah_voxel_ray.glsl: a cell whose every face is a pane's).
//
// A ray that crosses a solid meets it twice - where it enters (a FRONT face, looking back at the ray) and where it leaves
// (a BACK face, looking the ray's way) - and both faces carry the same coverage of the same rows of the cell. So the
// march carries T, the light left, and f, the fraction of the ray's cross-section INSIDE a solid, and takes every face it
// crosses IN THE ORDER IT CROSSES THEM:
//   a FRONT face of coverage c blocks (T x (1 - c)) and enters (f += c (1 - f));
//   a BACK face of coverage c first leaves what was entered (min( c, f ) leaves f and blocks nothing - the solid's front
//   already did), and its REMAINDER c - min( c, f ) is an open surface seen from behind, which blocks (T x (1 - rest)).
// One solid crossed end to end: once. Two solids: twice. A march starting inside a solid: stopped by its exit (nothing
// entered). Two open faces back to back: (1 - c)^2. A PANE's coverage is its projected area x its alpha (the
// voxeliser's), so a pane crossed whole passes 1 - alpha: Ogre's blend.
// The events: up to six (two a axis), each its crossing distance, its coverage (at most 1) and whether it is a front;
// at one distance the front first (a face the ray enters there).
//
// THE AT-SIGN RULE: this file becomes Hlms input (the pieces JahVoxelFaces and JahVoxelRay), so no character of it may be
// the Hlms directive mark, comments included.

#ifndef JAH_VOXEL_FACES_GLSL
#define JAH_VOXEL_FACES_GLSL

void jahVoxelFacesCross( float evT[6], float evC[6], bool evFront[6], int evN, inout float T, inout float fIn )
{
	// IN THE ORDER THE RAY CROSSES THEM (at one place: the front first - a face the ray enters there).
	for( int k = 1; k < evN; ++k )
	{
		const float kt = evT[k], kc = evC[k];
		const bool kf = evFront[k];
		int jj = k - 1;
		while( jj >= 0 && ( evT[jj] > kt || ( evT[jj] == kt && !evFront[jj] && kf ) ) )
		{
			evT[jj + 1] = evT[jj];
			evC[jj + 1] = evC[jj];
			evFront[jj + 1] = evFront[jj];
			--jj;
		}
		evT[jj + 1] = kt;
		evC[jj + 1] = kc;
		evFront[jj + 1] = kf;
	}
	for( int k = 0; k < evN; ++k )
	{
		const float c = evC[k];
		if( evFront[k] )
		{
			T *= 1.0 - c;
			fIn += c * ( 1.0 - fIn );
		}
		else
		{
			const float leave = min( c, fIn );
			fIn -= leave;
			T *= 1.0 - ( c - leave );
		}
	}
}

#endif   // JAH_VOXEL_FACES_GLSL
