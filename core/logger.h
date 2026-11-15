#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QDebug>

#include <functional>


#define LOG_FILE_NAME "log.txt"

class QFile;
class QTextStream;

#define irisDebug() \
	qDebug()<<"["<<__FILE__<<":"<<__LINE__<<"]"; \
	qDebug()

void irisLog(const QString &text);

namespace iris
{

class Logger
{
    QFile* file;
    QTextStream* out;

    static Logger* instance;
    Logger();

    /// The one write path behind info/warn/error. `tag` is the legacy file's
    /// "[info]: "/"[warn]: "/"[error]: " prefix; `severity` is the sink's.
    void emitRecord(const char *tag, int severity, const QString &text);

public:
    /// A host's log sink. `severity` is 0 = info, 1 = warn, 2 = error — an int
    /// rather than an enum so this header stays free of the host's level type.
    ///
    /// WHY THIS EXISTS (SESSION_LOG_SPEC fork F5-A, "absorb at the sink"):
    /// Studio's session log wants every one of the ~72 irisLog() call sites to
    /// gain a timestamp, a category, routing and rotation — without editing 72
    /// lines, and without IrisGL ever depending on Studio. So the DOCUMENT
    /// library keeps its own file and grows one hook; the APP installs the
    /// forwarder (JahLog::absorbIrisLogger). When a sink is installed it OWNS
    /// the qInfo/qWarning/qCritical duplication that used to happen here, so
    /// stderr stays exactly what it always was and nothing is printed twice.
    using Sink = std::function<void(int severity, const QString &text)>;
    static void setSink(Sink sink);
    static bool hasSink();

    void init(QString logFilePath);

    void info(QString text);
    void warn(QString text);
    void error(QString text);

    /// Pushes the buffered records to disk. Necessary since the per-line flush
    /// went away; the host calls it on the same schedule as its own sink's.
    void flush();

    static Logger* getSingleton();
};

}

#endif // LOGGER_H
