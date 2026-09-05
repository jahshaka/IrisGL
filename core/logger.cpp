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

void Logger::info(QString text)
{
    {
        QMutexLocker lock(&sLogMutex);
        if (out != nullptr) {
            *out << "[info]: "<<text<<"\n";
            out->flush();
        }
    }
    qInfo() << text;
}

void Logger::warn(QString text)
{
    {
        QMutexLocker lock(&sLogMutex);
        if (out != nullptr) {
            *out << "[warn]: "<<text<<"\n";
            out->flush();
        }
    }
    qWarning() << text;
}

void Logger::error(QString text)
{
    {
        QMutexLocker lock(&sLogMutex);
        if (out != nullptr) {
            *out << "[error]: "<<text<<"\n";
            out->flush();
        }
    }
    qCritical() << text;
}

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