// Jahshaka — ATOM-8 stage 0 (the read's fix): THE DENSITY COUNT'S CLEAR, on the device (no per-frame upload of
// the whole count buffer). One thread a slot zeroes its JAH_SLOT_WORDS words; thread 0 zeroes the header's two
// figures (2 the hidden triangles, 3 the drawn); the header's [0] (the slot capacity) and [1] (the bitmap's bits)
// are the host's four-word upload of the frame. Runs before the count (OgreAtomDensity.cpp atomDensityRecord).
// No Hlms directive mark appears in any comment here.
@insertpiece( SetCrossPlatformSettings )

layout( std430, ogre_U0 ) restrict buffer countLayout { uint counts[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_SLOT_WORDS 20u
#define JAH_COUNT_HEADER 4u

void main()
{
	uint slot = gl_GlobalInvocationID.x;
	if( slot == 0u )
	{
		counts[2] = 0u;
		counts[3] = 0u;
	}
	if( slot >= counts[0] )
		return;
	uint base = JAH_COUNT_HEADER + slot * JAH_SLOT_WORDS;
	for( uint w = 0u; w < JAH_SLOT_WORDS; ++w )
		counts[base + w] = 0u;
}
