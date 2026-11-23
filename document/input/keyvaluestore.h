#ifndef IRIS_KEYVALUESTORE_H
#define IRIS_KEYVALUESTORE_H

#include "document/export.h"

#include "core/value.h"

#include <optional>
#include <string_view>

namespace iris {

/// A persistent key/value store the document layer may read and write — the
/// user's preferences, as the application keeps them.
///
/// The document layer does not own the file and must not know how it is
/// written: the application's store answers reads from memory and makes the
/// writes durable on a thread of its own (SHADER-WARM-2: a settings sync on the
/// UI thread waited 16 s behind the disk). Keys are QSettings-shaped
/// ("group/key", UTF-8); remove(key) removes the key and every key under "key/".
/// Values are iris::Value (NOQT-DOCUMENT-1 stage 4): the application converts
/// them to the form its file holds (a list of strings stays the QStringList the
/// ini has always held).
class IRIS_DOCUMENT_API KeyValueStore
{
public:
    virtual ~KeyValueStore() = default;

    virtual bool contains(std::string_view key) const = 0;
    /// The stored value; nullopt when there is no such key.
    virtual std::optional<Value> value(std::string_view key) const = 0;
    virtual void setValue(std::string_view key, const Value &value) = 0;
    virtual void remove(std::string_view key) = 0;
};

}  // namespace iris

#endif  // IRIS_KEYVALUESTORE_H
