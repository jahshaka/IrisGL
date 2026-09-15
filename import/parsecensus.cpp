/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/parsecensus.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMutex>
#include <QThread>

namespace iris {
namespace ParseCensus {

namespace {

QMutex &lock()
{
    static QMutex m;
    return m;
}

Counts &counts()
{
    static Counts c;
    return c;
}

/// The thread QCoreApplication was created on — the UI thread in the app, and
/// the test's own thread in a document-only suite. With no QCoreApplication
/// (nothing in this tree runs without one, but the library links without it)
/// every parse counts as a worker parse: there is no UI thread to block.
bool onMainThread()
{
    const QCoreApplication *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

/// The elapsed clock of one Record. A QElapsedTimer per parse costs two
/// clock reads against a parse measured in hundreds of milliseconds.
QElapsedTimer &clockFor(qint64 &slot)
{
    static thread_local QElapsedTimer timer;
    Q_UNUSED(slot);
    return timer;
}

}   // namespace

Counts snapshot()
{
    QMutexLocker locked(&lock());
    return counts();
}

void reset()
{
    QMutexLocker locked(&lock());
    counts() = Counts();
}

void recordBake(bool hit)
{
    QMutexLocker locked(&lock());
    if (hit) ++counts().bakeHits;
    else     ++counts().bakeMisses;
}

Record::Record(const QString &path) : mPath(path), mStartNs(0)
{
    // A thread-local monotonic reference, started once per thread: a parse's
    // span is the difference of two reads of it. Nested parses do not exist
    // in this tree (an importer reads one file), and would still measure
    // their own span.
    QElapsedTimer &timer = clockFor(mStartNs);
    if (!timer.isValid()) timer.start();
    mStartNs = timer.nsecsElapsed();
}

Record::~Record()
{
    QElapsedTimer &timer = clockFor(mStartNs);
    const double ms = timer.isValid() ? double(timer.nsecsElapsed() - mStartNs) / 1.0e6 : 0.0;
    const bool main = onMainThread();
    QMutexLocker locked(&lock());
    Counts &c = counts();
    const bool resource = mPath.startsWith(QLatin1Char(':'))
                          || mPath.startsWith(QLatin1String("qrc:"));
    if (main && resource) {
        ++c.mainThreadResourceParses;
        c.mainThreadResourceMs += ms;
    } else if (main) {
        ++c.mainThreadParses;
        c.mainThreadMs += ms;
        c.lastMainThreadPath = mPath;
    } else {
        ++c.workerParses;
        c.workerMs += ms;
    }
}

}   // namespace ParseCensus
}   // namespace iris
