/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

// THE ONE TRANSLATION UNIT THAT COMPILES clusterlod.h's IMPLEMENTATION (ATOM
// stage 2, lane ATOM-CLUSTER-1). The header is a vendored COPY with its own
// provenance (thirdparty/meshoptimizer-clusterlod/PROVENANCE.md); its
// implementation needs meshoptimizer.h included first and CLUSTERLOD_IMPLEMENTATION
// defined in exactly one TU. Its only caller is the mesh bake
// (import/meshbake.cpp, `clusterdag::build`), and the header is in the bake's
// producer hash (irisgl/CMakeLists.txt) because what it computes is written into
// every .jmb.
//
// <assert.h> first: the implementation uses assert() and relies on its includer
// for the declaration (meshoptimizer.h happens to provide it today; the order
// here does not depend on that).
#include <assert.h>
#include "meshoptimizer.h"
#define CLUSTERLOD_IMPLEMENTATION
#include "thirdparty/meshoptimizer-clusterlod/clusterlod.h"
