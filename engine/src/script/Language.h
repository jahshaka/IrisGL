#pragma once
// A SCRIPT LANGUAGE's BINDING (SCRIPT-IN-ENGINE-1 stage 1): converts its values to script::Value and back, installs the
// registry's commands as its functions and calls Host::call. Each language is ONE file of its own (src/script/js_*.cpp is
// the JavaScript one, the only file that names its engine: source.script_neutral); nothing else in the engine knows it.
#include "jahshaka/engine/Script.h"

namespace jahshaka { namespace engine { namespace script {

class Host::Language {
public:
    virtual ~Language() = default;
    virtual Result run(const std::string &source, const std::string &name) = 0;
};

namespace detail {
/// The JavaScript binding of `host` (its one file, src/script/js_*.cpp).
std::unique_ptr<Host::Language> makeJavaScript(Host &host);
}

}}}  // namespace jahshaka::engine::script
