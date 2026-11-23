// IRIS_DOCUMENT_API - WHAT libIrisDocument EXPORTS (NOQT-DOCUMENT-1 stage 6). The document is ONE SHARED library built with
// hidden visibility (irisgl/document/CMakeLists.txt), so every process-wide table - the node graph's owners and journal, the
// live textures, the shipped meshes, the texture probe, the materials' revision - exists ONCE in a process, defined there.
// The mark is placed by one mechanical pass (scripts/lead/oneoff/noqt_export_annotate.py, the rule in the spec's EXPORT
// MARKS): every class with an out-of-line member, a static data member or a virtual function, and every namespace-scope
// function declared without a body. Never on a template or an inline function: those are compiled into each image that
// uses them.
#pragma once

#if defined(_WIN32)
#  if defined(IRIS_DOCUMENT_BUILD)
#    define IRIS_DOCUMENT_API __declspec(dllexport)
#  else
#    define IRIS_DOCUMENT_API __declspec(dllimport)
#  endif
#else
#  define IRIS_DOCUMENT_API __attribute__((visibility("default")))
#endif
