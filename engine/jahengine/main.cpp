// jahengine — THE ENGINE, SCRIPTED, WITH NO STUDIO AND NO QT (SCRIPT-IN-ENGINE-1 stage 1). It links the engine library and
// nothing of Studio, IrisGL's document or Qt (source.script_neutral reads its NEEDED list).
//
//     jahengine --script <file.js> [--test-doors]
//
// It boots the engine as the engine fixtures do (enginetest::testEngineConfig's fields: the build's render-system plugins
// and Hlms media; the engine's thread is this one, no render thread; the fixtures' shared warm shader cache when
// JAHSHAKA_SHADER_CACHE_DIR names it — read ONCE, here — held to the fixtures' 1 GiB cap; the Ogre log
// `<script stem>-ogre.log` in the working directory), makes the process's script host, registers the engine's commands
// (a failed boot registers them too: each says why), runs the script on this thread, and EXITS WITH ITS VERDICT
// (Host::verdict): 0 when the script ran to its end (its queued async work included), at least one host.check ran and
// every one held, and every shader program compiled, else 1. Its LAST LINE is
// `<script stem>: PASSED` or `<script stem>: FAILED (<n>)`. --test-doors opens the engine's TEST arms (ArmInfo::test).
// The process ends at its engine's death: the exit-only teardown (ProcessExit.h), then _Exit with the verdict.
#include "jahshaka/engine/Engine.h"
#include "jahshaka/engine/ProcessExit.h"
#include "jahshaka/engine/Script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace jahshaka::engine;

int main(int argc, char **argv)
{
    std::string scriptPath;
    bool testDoors = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--script") && i + 1 < argc) scriptPath = argv[++i];
        else if (!std::strcmp(argv[i], "--test-doors")) testDoors = true;
        else scriptPath.clear(), i = argc;
    }
    std::ifstream in(scriptPath, std::ios::binary);
    if (scriptPath.empty() || !in) {
        std::printf("usage: jahengine --script <file.js> [--test-doors]%s%s\n", scriptPath.empty() ? "" : " - cannot read ",
                    scriptPath.c_str());
        std::printf("jahengine: FAILED (1)\n");
        return 2;
    }
    std::ostringstream source;
    source << in.rdbuf();
    std::string stem = scriptPath.substr(scriptPath.find_last_of('/') + 1);
    stem = stem.substr(0, stem.rfind('.'));

    EngineConfig cfg;
    cfg.pluginDir = JAHENGINE_OGRE_PLUGIN_DIR;
    cfg.hlmsMediaDir = JAHENGINE_MEDIA_DIR;
    cfg.logFile = stem + "-ogre.log";
    if (const char *cache = std::getenv("JAHSHAKA_SHADER_CACHE_DIR")) {
        // THE FIXTURES' SHARED CACHE (tests/CMakeLists.txt's engine rows): the union of every family's shaders outgrows the
        // app's 256 MiB cap, and a save past the cap wipes the directory - the fixtures' 1 GiB (enginetesthelpers.h).
        cfg.shaderCacheDir = cache;
        cfg.shaderCacheMaxBytes = 1024ull * 1024ull * 1024ull;
    }
    std::string bootError;
    std::unique_ptr<Engine> engine = Engine::create(cfg, bootError);

    script::Host host;
    script::registerEngineCommands(host.registry(), engine.get(), bootError, testDoors);
    int failures = host.verdict(host.run(source.str(), scriptPath));
    if (const unsigned long long failed = Engine::processShaderCompileErrors()) {
        std::printf("FAIL: %llu shader program(s) failed to compile (the readings are of draws that never happened) - see "
                    "%s\n", failed, cfg.logFile.c_str());
        ++failures;
    }
    if (engine) {
        markProcessExit();
        engine.reset();
    }
    if (failures) std::printf("%s: FAILED (%d)\n", stem.c_str(), failures);
    else std::printf("%s: PASSED\n", stem.c_str());
    std::fflush(nullptr);
    std::_Exit(failures ? 1 : 0);
}
