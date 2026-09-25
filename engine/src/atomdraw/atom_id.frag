// ATOM S3-DRAW — THE ID PASS'S FRAGMENT STAGE: the AtomId words, nothing else. No
// material, no texture (alpha-tested items never reach this pass: the split keeps
// them on PBS).
#version 460

layout( location = 0 ) flat in uint inSlotLevel;
layout( location = 1 ) flat in uint inTriangle;

layout( location = 0 ) out uvec2 outIds;

void main()
{
	outIds = uvec2( inSlotLevel, inTriangle );
}
