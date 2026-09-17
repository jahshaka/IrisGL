// Jahshaka — THE HDR METER, part 2: the histogram becomes one exposure
// (EXPOSURE-2). Replaces HDR/DownScale03_SumLumEnd, whose arithmetic from
// `newLum` onwards is reproduced here EXACTLY so that the only thing that
// changed about the grade is HOW the frame was measured.
//
// ONE INVOCATION, on purpose. 128 bins read twice is 256 loads and a serial
// walk; a parallel reduction of 128 values would cost more in barriers than it
// saves, and the walk below is a running CUMULATIVE weight, which is inherently
// ordered. The dispatch is one thread group of one thread.
//
// THE PERCENTILE CLIPS (Scene::exposureMeterLowPercent / HighPercent;
// PostFxDesc::meterLowPercent / meterHighPercent; Unreal's MinPercent /
// MaxPercent by another name). The walk accumulates the metered weight darkest
// bin first and averages ONLY the slice between the two percentiles - 10 % and
// 90 % by default. What that buys, in the render audit's terms: a sun disc, a
// blown window or a specular firefly is a few percent of the metered weight and
// is CUT, instead of pulling a mean it has no business pulling; and a deep
// shadow region is cut from the other end for the same reason. A bin the window
// cuts through contributes the FRACTION of its weight that falls inside, at
// that bin's own reconstructed mean - so the answer moves smoothly as the
// picture changes and does not step from bin to bin.
//
// THE RECONSTRUCTION. Row 1 holds each bin's weighted log-luminance OFFSET
// within the bin (see JahHdrMeterBuild_cs), so a bin's true weighted mean is
// binMin + (sum/4096) / (weight/256). The bin width therefore bounds NOTHING
// except the two cut bins: a flat grey frame lands every sample in one bin and
// is still reported to ~1e-4 nats, where bin centres alone would have been out
// by up to +/-0.117 stops - about 5/255 in a mid tone, which is visible.
//
// AN UNMEASURABLE FRAME HOLDS THE GRADE, which is patch 0042's rule and the
// only answer that cannot invent a grade change: a missing measurement carries
// no information about the scene. With a histogram "unmeasurable" no longer
// means "one bad sample" - unusable samples were never binned - it means the
// pattern found NOTHING to measure, which in practice is only a 0x0 target.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

@property( syntax == glsl )
	#define ogre_t0 binding = 0
	#define ogre_u0 binding = 0
	#define ogre_u1 binding = 1
@end

vulkan_layout( ogre_t0 ) uniform texture2D oldLumRt;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image2D lumOut;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece( uav1_pf_type ) )
uniform restrict readonly uimage2D histogram;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 exposure;
	uniform vec4 meterClip;
	uniform float timeSinceLast;
vulkan( }; )

// Verbatim from HdrUtils::setExposure: x = 1024 * e^(E-2), y = 7.5 - maxEv,
// z = 7.5 - minEv. y and z are the window ON THE MEASUREMENT's axis, which is
// why iris::lens::meterGreyCardChain exists on the document side (EXPOSURE-1).
#define p_exposureScale exposure.x
#define p_logLumMin     exposure.y
#define p_logLumMax     exposure.z
#define p_lowFrac       meterClip.x
#define p_highFrac      meterClip.y

const float c_histMinLog  = -2.7725887;     // ln( 1024 ) - 14 * ln 2
const float c_histSpanLog = 20.7944154;     // 30 * ln 2
const float c_weightScale = 256.0;
const float c_logScale    = 4096.0;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	const int bins = imageSize( histogram ).x;
	const float binWidth = c_histSpanLog / float( bins );

	float total = 0.0;
	for( int i = 0; i < bins; ++i )
		total += float( imageLoad( histogram, ivec2( i, 0 ) ).x );

	float measured = 0.0;
	bool haveMeasurement = false;

	if( total > 0.0 )
	{
		const float lowW  = total * p_lowFrac;
		const float highW = total * p_highFrac;
		float cum = 0.0, clipW = 0.0, clipL = 0.0, allW = 0.0, allL = 0.0;
		for( int i = 0; i < bins; ++i )
		{
			const float c = float( imageLoad( histogram, ivec2( i, 0 ) ).x );
			if( c > 0.0 )
			{
				const float sum = float( imageLoad( histogram, ivec2( i, 1 ) ).x );
				// The bin's own weighted mean, reconstructed exactly.
				const float mean = c_histMinLog + float( i ) * binWidth +
								   ( sum / c_logScale ) / ( c / c_weightScale );
				allW += c;
				allL += c * mean;
				// ...and the fraction of it the percentile window keeps.
				const float a = max( cum, lowW );
				const float b = min( cum + c, highW );
				if( b > a )
				{
					const float f = ( b - a ) / c;
					clipW += f * c;
					clipL += f * c * mean;
				}
				cum += c;
			}
		}
		// A window that keeps nothing (min == max) falls back to the whole
		// frame rather than to no measurement: the user asked for a degenerate
		// clip, not for the meter to stop working.
		if( clipW > 0.0 )      { measured = clipL / clipW; haveMeasurement = true; }
		else if( allW > 0.0 )  { measured = allL / allW;   haveMeasurement = true; }
	}

	// ---- from here down this is HDR/DownScale03_SumLumEnd, unchanged --------
	float newLum = p_exposureScale /
				   exp( clamp( haveMeasurement ? measured : p_logLumMin,
							   p_logLumMin, p_logLumMax ) );
	float oldLum = texelFetch( oldLumRt, ivec2( 0, 0 ), 0 ).x;
	const bool haveHistory = ( ( floatBitsToUint( oldLum ) & 0x7FFFFFFFu ) < 0x7F800000u );
	oldLum = haveHistory ? oldLum : newLum;
	newLum = ( !haveMeasurement && haveHistory ) ? oldLum : newLum;

	// Adapt 75 % per second, on the engine's FIXED clock (frame_time is
	// ControllerManager's frame-delay value, 1/60 s - the engine has no wall
	// clock, CLAUDE.md). Counted in frames, like every settle in the suites.
	imageStore( lumOut, ivec2( 0, 0 ),
				vec4( mix( newLum, oldLum, pow( 0.25, timeSinceLast ) ), 0.0, 0.0, 0.0 ) );
}
