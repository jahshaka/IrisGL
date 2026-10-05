#ifndef IRIS_KEYVALUESTORE_H
#define IRIS_KEYVALUESTORE_H

#include <QString>
#include <QVariant>

namespace iris {

/// A persistent key/value store the document layer may read and write — the
/// user's preferences, as the application keeps them.
///
/// The document layer does not own the file and must not know how it is
/// written: the application's store answers reads from memory and makes the
/// writes durable on a thread of its own (SHADER-WARM-2: a settings sync on the
/// UI thread waited 16 s behind the disk). Keys are QSettings-shaped
/// ("group/key"); remove(key) removes the key and every key under "key/".
class KeyValueStore
{
public:
    virtual ~KeyValueStore() = default;

    virtual bool contains(const QString &key) const = 0;
    virtual QVariant value(const QString &key, const QVariant &fallback = QVariant()) const = 0;
    virtual void setValue(const QString &key, const QVariant &value) = 0;
    virtual void remove(const QString &key) = 0;
};

}  // namespace iris

#endif  // IRIS_KEYVALUESTORE_H
