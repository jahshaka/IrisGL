// jahengine — THE ENGINE, SCRIPTED, WITH NO STUDIO AND NO QT (SCRIPT-IN-ENGINE-1 stage 1). It links the engine library and
// nothing of Studio, IrisGL's document or Qt (source.script_neutral reads its NEEDED list).
//
//     jahengine --script <file.js> [--test-doors] [--name <row>]
//     jahengine --list
//     jahengine --mcp [--test-doors] [--data-root <dir>]
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
// --mcp (MCP-ON-HOST-1; irisgl/engine/mcp/include/mcp/session.h): an MCP server on stdin / stdout - fd 1 is claimed for
// the protocol FIRST (everything else the process writes to stdout goes to stderr); a documentation-only host answers
// until the first engine-reaching tools/call boots the engine (this config; the Ogre log in --data-root, else the
// working directory); at stdin's end the scoped handles are released, the engine's exit-only teardown runs, exit 0.
#include "jahshaka/engine/Engine.h"
#include "jahshaka/engine/EngineInspect.h"
#include "jahshaka/engine/ProcessExit.h"
#include "jahshaka/engine/Script.h"
#include "mcp/stdio.h"

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
    std::string scriptPath, rowName, dataRoot;
    bool testDoors = false, list = false, serveMcp = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--script") && i + 1 < argc) scriptPath = argv[++i];
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) rowName = argv[++i];
        else if (!std::strcmp(argv[i], "--test-doors")) testDoors = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--mcp")) serveMcp = true;
        else if (!std::strcmp(argv[i], "--data-root") && i + 1 < argc) dataRoot = argv[++i];
        else scriptPath.clear(), list = serveMcp = false, i = argc;
    }
    if (serveMcp) {
        std::string why;
        const int proto = mcp::claimProtocolFd(why);   // L1: before anything can write to stdout
        if (proto < 0) {
            std::fprintf(stderr, "jahengine --mcp: %s\n", why.c_str());
            return 2;
        }
        EngineConfig cfg;
        cfg.pluginDir = JAHENGINE_OGRE_PLUGIN_DIR;
        cfg.hlmsMediaDir = JAHENGINE_MEDIA_DIR;
        cfg.logFile = (dataRoot.empty() ? std::string() : dataRoot + "/") + "jahengine-mcp-ogre.log";
        inspect::applyHarnessShaderCache(cfg);   // the cache and its cap: ONE function (EngineInspect.h)
        std::string bootError;
        std::unique_ptr<Engine> engine;
        mcp::Session::Doors doors;
        doors.version = "dev";   // (the binary's identity is its BUILT_FROM stamp: stage 4's proxy reads it)
        doors.makeHost = [&](const script::Host::Options &options, bool withEngine) {
            if (withEngine && !engine && bootError.empty()) {   // THE LAZY BOOT: once, at the first engine-reaching call
                engine = Engine::create(cfg, bootError);
                if (!engine) std::fprintf(stderr, "jahengine --mcp: the engine did not start: %s\n", bootError.c_str());
            }
            auto host = std::make_unique<script::Host>(options);
            script::registerEngineCommands(*host, withEngine ? engine.get() : nullptr,
                                           withEngine ? bootError : std::string("documentation only"), testDoors);
            return host;
        };
        mcp::serveStdio(0, proto, std::move(doors));   // until stdin's end; its scoped handles released (L13)
        if (engine) {
            markProcessExit();
            engine.reset();
        }
        std::fflush(nullptr);
        std::_Exit(0);
    }
    if (list) {
        // THE COMMANDS, AS THE HOST SERVES THEM (no engine: each would say why - the list is the registration's)
        script::Host::Options docs;
        docs.documentationOnly = true;
        script::Host host(docs);
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
        std::printf("usage: jahengine --script <file.js> [--test-doors] [--name <row>] | --list | --mcp [--test-doors] "
                    "[--data-root <dir>]%s%s\n",
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
