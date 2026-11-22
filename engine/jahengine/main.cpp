// jahengine — THE ENGINE, SCRIPTED, WITH NO STUDIO AND NO QT (SCRIPT-IN-ENGINE-1 stage 1). It links the engine library and
// nothing of Studio, IrisGL's document or Qt (source.script_neutral reads its NEEDED list).
//
//     jahengine --script <file.js> [--test-doors] [--name <row>]
//     jahengine --list
//
// It boots the engine as the engine fixtures do (enginetest::testEngineConfig's fields: the build's render-system plugins
// and Hlms media; the engine's thread is this one, no render thread; the fixtures' shared warm shader cache when
// JAHSHAKA_SHADER_CACHE_DIR names it — read ONCE, here — held to the fixtures' 1 GiB cap; the Ogre log
// `<script stem>-ogre.log` in the working directory), makes the process's script host, registers the engine's commands
// (a failed boot registers them too: each says why), runs the script on this thread, and EXITS WITH ITS VERDICT
// (Host::verdict): 0 when the script ran to its end (its queued async work included), at least one host.check ran and
// every one held, and every shader program compiled, else 1. Its LAST LINE is
// `<name>: PASSED` or `<name>: FAILED (<n>)` - <name> the row's (--name; the script's stem without it). --test-doors opens
// the engine's TEST arms (ArmInfo::test). --list boots nothing: it prints every command the host serves - its signature
// (the declared kinds, choices, ranges and units, then its access: [reads] | [changes] | [files] | [test]) and its doc,
// the seed of the generated documentation - and exits 0, or 1 after a FAULT line per command the registry refused.
// The process ends at its engine's death: the exit-only teardown (ProcessExit.h), then _Exit with the verdict.
#include "jahshaka/engine/Engine.h"
#include "jahshaka/engine/EngineInspect.h"
#include "jahshaka/engine/ProcessExit.h"
#include "jahshaka/engine/Script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace jahshaka::engine;

int main(int argc, char **argv)
{
    std::string scriptPath, rowName;
    bool testDoors = false, list = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--script") && i + 1 < argc) scriptPath = argv[++i];
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) rowName = argv[++i];
        else if (!std::strcmp(argv[i], "--test-doors")) testDoors = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else scriptPath.clear(), list = false, i = argc;
    }
    if (list) {
        // THE COMMANDS, AS THE HOST SERVES THEM (no engine: each would say why - the list is the registration's)
        script::Host host;
        script::registerEngineCommands(host, nullptr, "--list boots no engine", testDoors);
        const std::vector<std::string> faults = host.registry().validate();
        for (const script::CommandInfo *c : host.registry().commands())
            std::printf("%s\n    %s\n", script::signatureText(*c).c_str(), c->doc.c_str());
        for (const std::string &f : faults) std::printf("FAULT: %s\n", f.c_str());
        std::fflush(nullptr);
        return faults.empty() ? 0 : 1;
    }
    std::ifstream in(scriptPath, std::ios::binary);
    if (scriptPath.empty() || !in) {
        std::printf("usage: jahengine --script <file.js> [--test-doors] [--name <row>] | --list%s%s\n",
                    scriptPath.empty() ? "" : " - cannot read ",
                    scriptPath.c_str());
        std::printf("jahengine: FAILED (1)\n");
        return 2;
    }
    std::ostringstream source;
    source << in.rdbuf();
    std::string stem = scriptPath.substr(scriptPath.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));
    const std::string verdictName = rowName.empty() ? stem : rowName;

    EngineConfig cfg;
    cfg.pluginDir = JAHENGINE_OGRE_PLUGIN_DIR;
    cfg.hlmsMediaDir = JAHENGINE_MEDIA_DIR;
    cfg.logFile = stem + "-ogre.log";
    inspect::applyHarnessShaderCache(cfg);   // the fixtures' shared warm cache and its cap: ONE function (EngineInspect.h)
    std::string bootError;
    std::unique_ptr<Engine> engine = Engine::create(cfg, bootError);

    int failures = 0;
    {
        // THE HOST DIES BEFORE ITS ENGINE (THE OWNERSHIP LAW, registerEngineCommands): its end destroys what the script
        // made - the nodes, meshes and materials, the views, the scenes - while the engine lives
        script::Host host(script::Host::Options{});   // its options stated: the 512 MiB picture cap
        script::registerEngineCommands(host, engine.get(), bootError, testDoors);
        failures = host.verdict(host.run(source.str(), scriptPath));
    }
    if (const unsigned long long failed = Engine::processShaderCompileErrors()) {
        std::printf("FAIL: %llu shader program(s) failed to compile (the readings are of draws that never happened) - see "
                    "%s\n", failed, cfg.logFile.c_str());
        ++failures;
    }
    if (engine) {
        markProcessExit();
        engine.reset();
    }
    if (failures) std::printf("%s: FAILED (%d)\n", verdictName.c_str(), failures);
    else std::printf("%s: PASSED\n", verdictName.c_str());
    std::fflush(nullptr);
    std::_Exit(failures ? 1 : 0);
}
