#include "core/logger.h"
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QDebug>

namespace iris
{

Logger* Logger::instance = nullptr;

// The log is written from the UI thread AND from workers (the engine error
// pump, the import pipeline, the database). QTextStream is not reentrant and
// two threads writing one stream interleave inside a single record — this lock
// guards init() and all three write paths.
static QMutex sLogMutex;

/// The host sink (SESSION_LOG_SPEC F5-A). Guarded by the same mutex as the
/// file: it is installed once at startup and read from every logging thread.
static Logger::Sink sSink;

void Logger::setSink(Sink sink)
{
    QMutexLocker lock(&sLogMutex);
    sSink = std::move(sink);
}

bool Logger::hasSink()
{
    QMutexLocker lock(&sLogMutex);
    return bool(sSink);
}

Logger::Logger()
{
    file = nullptr;
    out = nullptr;
}

void Logger::init(QString logFilePath)
{
    QMutexLocker lock(&sLogMutex);
    // init() is called twice on some startup paths (mainwindow.cpp picks one
    // of two locations); re-initialising used to leak the previous pair.
    delete out;
    out = nullptr;
    delete file;
    file = new QFile(logFilePath);
    file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (file->isOpen()) {
        out = new QTextStream(file);
    }
}

/// The ONE write path (SESSION_LOG_SPEC F5-A). Two changes from the original
/// three near-identical bodies:
///
///  * NO per-line flush. It only ever bought surviving a hard kill, and the
///    host sink's own flush policy (Warning-and-above immediately, plus an
///    idle timer) covers that case properly. This used to be a formatted
///    QTextStream write AND a flush AND a q* call PER RECORD — on a path that
///    included PlayBack::update's per-frame mismatch line (spec §8-R2).
///  * When a host sink is installed it OWNS the q* duplication, so a record is
///    never both forwarded and printed twice.
void Logger::emitRecord(const char *tag, int severity, const QString &text)
{
    Sink sink;
    {
        QMutexLocker lock(&sLogMutex);
        if (out != nullptr) *out << tag << text << "\n";
        sink = sSink;
    }
    if (sink) { sink(severity, text); return; }
    switch (severity) {
    case 0:  qInfo() << text; break;
    case 1:  qWarning() << text; break;
    default: qCritical() << text; break;
    }
}

void Logger::flush()
{
    QMutexLocker lock(&sLogMutex);
    if (out != nullptr) out->flush();
}

void Logger::info(QString text)  { emitRecord("[info]: ",  0, text); }
void Logger::warn(QString text)  { emitRecord("[warn]: ",  1, text); }
void Logger::error(QString text) { emitRecord("[error]: ", 2, text); }

Logger *Logger::getSingleton()
{
    // Thread-safe construction: the language guarantees exactly one
    // initialisation of a function-local static, where the old
    // `if (instance == nullptr) instance = new Logger()` raced (two loggers,
    // one of them leaked, and whichever lost the race kept writing to a file
    // nobody had opened).
    //
    // Deliberately NEVER destroyed — the same lifetime it has always had.
    // A logger that dies at static-destruction time is a read-after-destroy
    // waiting for the first qWarning past main().
    static Logger *const singleton = [] {
        auto *logger = new Logger();
        instance = logger;
        return logger;
    }();
    return singleton;
}

}

void irisLog(const QString &text)
{
    iris::Logger::getSingleton()->info(text);
}