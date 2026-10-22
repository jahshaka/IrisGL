// IS THIS A NUMBER, AND THE ONE CEILING — the one copy of each (GATHER-1a; PHOTON-7 stage 6, D12).
//
// `x == x` IS NOT A NaN TEST IN THIS TREE (CLAUDE.md, lane HDR-1, measured on
// NVIDIA 595.84): the compiler folds a self-equality to true, so fork feab041c6 (was 0034)'s
// guard never fired and its "fix" worked by accident. The BITS are the test —
// an exponent of all ones is Inf or NaN, whatever the sign.
//
// Its own header, with its own guard, because two files that both need it are
// included together (the probe parameters and the hit-radiance functions) and a
// second definition of one function is a compile error, not a duplicate.
#ifndef JAH_RQ_FINITE_GLSL
#define JAH_RQ_FINITE_GLSL

bool finite1( float x ) { return ( floatBitsToUint( x ) & 0x7FFFFFFFu ) < 0x7F800000u; }
bool finite3( vec3 v ) { return finite1( v.x ) && finite1( v.y ) && finite1( v.z ); }

// THE RADIANCE CEILING every ray job, the screen reflection's raster passes and the gather hold a radiance to before it
// enters a history, an atlas or a mean: a finite value brighter than this is held AT it (clamp, never min, so a channel
// below its negative comes back too). One number, here; the raster SSR passes include the raw copy of this file
// (JahRqFinite.glsl, staged by engine/CMakeLists.txt), so no shader carries a literal of its own.
const float kMaxRadiance = 1024.0;

#endif   // JAH_RQ_FINITE_GLSL
