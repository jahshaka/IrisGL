// THE RAY SEAM (DOORS-0 stage 1, V2 plan 0.5): every ray query of the engine's shaders is spelled with these names,
// and the card's ray API is spelled in this file ONLY (source.ray_seam). Each name is the GL_EXT_ray_query call it
// stands for, so the compiled words are the card's own (40 of 40 build-time modules, every media permutation: the
// byte proof); a software tracer standing in for ray hardware (WebGPU, a Mac without it) replaces this one file.
// DEFINES ONLY: no function, so the seam adds no OpFunction and no OpName.
// PER VIEW (multiview is the target): every name takes the query and its ray inputs as arguments - no camera, eye or
// view state lives here. `c` true reads the committed intersection, false the candidate.
// The includer enables GL_EXT_ray_query (its own extension line). This file is also the first input of the Hlms piece
// JahLampRayLib (engine/CMakeLists.txt): no Hlms directive mark in it.
#ifndef JAH_RQ_GLSL
#define JAH_RQ_GLSL

// the query and the top-level structure (a binding, or built from a device address)
#define JahRq rayQueryEXT
#define JahTlas accelerationStructureEXT

// the traversal
#define jahRqInit( q, tlas, flags, mask, origin, tMin, dir, tMax ) rayQueryInitializeEXT( q, tlas, flags, mask, origin, tMin, dir, tMax )
#define jahRqProceed( q ) rayQueryProceedEXT( q )
#define jahRqConfirm( q ) rayQueryConfirmIntersectionEXT( q )
#define jahRqTerminate( q ) rayQueryTerminateEXT( q )

// the intersection, committed (c true) or candidate (c false)
#define jahRqType( q, c ) rayQueryGetIntersectionTypeEXT( q, c )
#define jahRqT( q, c ) rayQueryGetIntersectionTEXT( q, c )
#define jahRqCustomIndex( q, c ) rayQueryGetIntersectionInstanceCustomIndexEXT( q, c )
#define jahRqInstanceId( q, c ) rayQueryGetIntersectionInstanceIdEXT( q, c )
#define jahRqGeometry( q, c ) rayQueryGetIntersectionGeometryIndexEXT( q, c )
#define jahRqPrimitive( q, c ) rayQueryGetIntersectionPrimitiveIndexEXT( q, c )
#define jahRqBary( q, c ) rayQueryGetIntersectionBarycentricsEXT( q, c )
#define jahRqObjectToWorld( q, c ) rayQueryGetIntersectionObjectToWorldEXT( q, c )
#define jahRqFrontFace( q, c ) rayQueryGetIntersectionFrontFaceEXT( q, c )
// a pane's word (jah_rq_alpha.glsl): carried today in the instance's shader-binding-table offset
#define jahRqPaneWord( q, c ) rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT( q, c )

// the intersection types and the ray flags
#define JAH_RQ_HIT_TRIANGLE gl_RayQueryCommittedIntersectionTriangleEXT
#define JAH_RQ_CANDIDATE_TRIANGLE gl_RayQueryCandidateIntersectionTriangleEXT
#define JAH_RQ_FLAGS_NONE gl_RayFlagsNoneEXT
#define JAH_RQ_FLAGS_FIRST_HIT gl_RayFlagsTerminateOnFirstHitEXT
#define JAH_RQ_FLAGS_OPAQUE gl_RayFlagsOpaqueEXT

#endif
