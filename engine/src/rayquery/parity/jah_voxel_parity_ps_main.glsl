// Four texels per cone, in jahParityAnswer's order.
void main()
{
	int px = int( gl_FragCoord.x );
	int cone = px / 4;
	if( cone >= int( counts.x ) )
	{
		fragColour = vec4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	fragColour = jahParityAnswer( cone, px - 4 * cone );
}
