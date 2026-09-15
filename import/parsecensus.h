/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef PARSECENSUS_H
#define PARSECENSUS_H

// ParseCensus — every assimp parse in the process, counted, and WHICH THREAD
// PAID FOR IT.
//
// A model parse is the most expensive thing an open does (measured
// 2026-09-15: 1 086 ms for the Matcaps dragon, 986 ms for the World
// Background one) and for two years there was no way to ask the running app
// whether it had just paid one on the thread that draws. The load ledger
// (Studio's services/loadtimeline.h) times the parse, but a ledger counter is
// per-OPEN and says nothing about threads — the threaded open and the
// synchronous one produced the same counter name from different threads.
//
// This is the thread-aware half, and it sits HERE, beside assimp, because
// that is the only place it cannot be bypassed: every ReadFile in this tree
// is inside IrisGL (import/importflags.h says so and means it), and each of
// them opens a Record. A future call site that forgets one is a call site
// that does not parse.
//
// THE CONTRACT IT EXISTS TO ENFORCE (OPEN-ASSIMP-1): a project open must not
// parse a model on the UI thread. `app.openStats()` reads this census, and
// open.responsive asserts the main-thread count is ZERO over the open of
// every shipped sample.
//
// Cost: one QElapsedTimer read and one mutex-guarded add per parse — against
// a parse measured in hundreds of milliseconds.

#include <QString>

namespace iris {
namespace ParseCensus {

/// What has been parsed since the last reset().
struct Counts
{
    int    mainThreadParses = 0;   ///< FILE parses on the thread QCoreApplication lives on
    double mainThreadMs     = 0.0;
    /// Main-thread parses of a QT RESOURCE (":/..." / "qrc:/..."), counted
    /// apart because they are a different animal: a built-in primitive is a
    /// few kilobytes compiled into the binary, it is parsed ONCE per process
    /// (iris::Mesh::loadMesh caches by path), and no prewarm can hoist it
    /// because every caller asks for it by name. A project's model file is
    /// arbitrary content on disk and is the parse this census exists for.
    int    mainThreadResourceParses = 0;
    double mainThreadResourceMs     = 0.0;
    int    workerParses     = 0;   ///< parses that ran on any other thread
    double workerMs         = 0.0;
    /// Bake reads ASKED FOR, and how they went: a hit is a model that came
    /// out of MESH_BAKE (a few memcpys), a miss is a source whose content has
    /// no usable bake yet — which is exactly when a parse has to happen.
    /// Counted wherever a bake is asked for FOR A SOURCE FILE: the prewarm
    /// worker and Studio's MeshBakeStore both report here, so one number
    /// covers both halves of an open.
    int bakeHits   = 0;
    int bakeMisses = 0;
    /// The last FILE parsed on the main thread — the one a failing assertion
    /// wants named. Empty when there has been none.
    QString lastMainThreadPath;
};

Counts snapshot();
void reset();

/// One bake ASKED FOR: `hit` = a usable bake was read, false = there was none
/// (or it was stale) and the caller parses instead.
void recordBake(bool hit);

/// RAII: one assimp read. Wraps the ReadFile call; the destructor banks the
/// elapsed time against the thread it ran on.
class Record
{
public:
    explicit Record(const QString &path);
    ~Record();

private:
    Record(const Record &) = delete;
    Record &operator=(const Record &) = delete;

    QString mPath;
    qint64  mStartNs;
};

}   // namespace ParseCensus
}   // namespace iris

#endif   // PARSECENSUS_H
