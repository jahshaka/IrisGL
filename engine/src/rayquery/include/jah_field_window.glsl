// THE IRRADIANCE FIELD'S WINDOW — which atlas tile holds which probe
// (PHOTON-WRITER-1, P4 item FIELD-SCROLL). Stated once, read by the field's
// generation and integration jobs and by the pixel's reader, so the three
// cannot disagree about where a probe lives.
//
// THE SHAPE. The field is a toroidal window over a probe lattice fixed in the
// world. Window-LOCAL probe i (0 .. N-1 per axis, the cage the pixel builds and
// the grid the generation job places) lives in atlas SLOT (i + offset) mod N,
// where `offset` is the slot of the window's first probe. A scroll of d
// spacings moves the window by d and the offset by d: the N - |d| planes of
// probes that stay inside keep their slots and their values, and the |d|
// planes that entered take the tiles of the ones that left. The probe counts
// are powers of two (IrradianceFieldSettings), so the modulo is a mask.
//
// THE WORK. The generation and integration jobs walk a LIST of probes, stored
// as up to three disjoint boxes of slots (IrradianceField::scrollWindow: the
// planes that entered, one box per moved axis; reset(): the whole grid, one
// box). Box k covers list positions boxLo[k].w .. + its size (all ones in w for
// an unused box), its slots boxLo[k].xyz + b (mod N) for b inside boxSize[k].
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahFieldWindow):
// no character of it may be the Hlms directive mark, comments included.

uvec3 jahFieldSlot( uvec3 local, uvec3 offset, uvec3 counts )
{
	return ( local + offset ) & ( counts - uvec3( 1u, 1u, 1u ) );
}

uvec3 jahFieldLocal( uvec3 slot, uvec3 offset, uvec3 counts )
{
	return ( slot + counts - offset ) & ( counts - uvec3( 1u, 1u, 1u ) );
}

uint jahFieldSlotIndex( uvec3 slot, uvec3 counts )
{
	return slot.x + slot.y * counts.x + slot.z * counts.x * counts.y;
}

uvec3 jahFieldWorkSlot( uint p, uvec4 lo0, uvec4 lo1, uvec4 lo2,
						uvec4 size0, uvec4 size1, uvec4 size2, uvec3 counts )
{
	uvec4 lo = lo0;
	uvec4 sz = size0;
	if( p >= lo2.w ) { lo = lo2; sz = size2; }
	else if( p >= lo1.w ) { lo = lo1; sz = size1; }
	uint q = p - lo.w;
	uvec3 b = uvec3( q % sz.x, ( q / sz.x ) % sz.y, q / ( sz.x * sz.y ) );
	return ( lo.xyz + b ) & ( counts - uvec3( 1u, 1u, 1u ) );
}
