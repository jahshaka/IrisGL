// The engine's log bridge (SESSION_LOG_SPEC fork F3-B) and the device-info
// read-back (§4's two NEW header rows).
//
// ZERO OGRE PATCHES. `Ogre::LogListener` is a public interface and the default
// log takes listeners — the exact mechanism the shader cache's compile counters
// already use in-tree (OgreShaderCache.cpp). Nothing upstream is touched.
//
// WHAT IS FORWARDED AND WHY NOT MORE. Ogre logs thousands of LML_NORMAL lines
// per boot. Folding all of them into the application's session log would
// destroy the signal-to-noise the log exists for, so Ogre keeps its own
// per-session file and only the levels the host asked for cross the boundary:
// the host decides what to do with each, by level, on its side.
//
// THREE HARD RULES, all properties of Ogre::Log (OgreLog.cpp):
//   1. `messageLogged` is called with `mMutex` HELD, on whatever thread logged
//      — including Ogre's background streaming and texture threads. The sink
//      must be thread-safe.
//   2. It must NEVER call back into Ogre. That is a deadlock, not a risk.
//   3. `skipThisMessage` must stay false. Setting it suppresses Ogre's OWN file
//      and console output, which is the artifact this whole fork preserved.

#include "EnginePrivate.h"

#include <OgreRenderSystemCapabilities.h>

#include <mutex>

namespace jahshaka { namespace engine {
namespace detail {

// ---------------------------------------------------------------------------
/// The listener. Owns the host's sink and the one line Ogre states but never
/// exposes: the Vulkan API version.
class OgreEngine::LogBridge final : public Ogre::LogListener {
public:
    void setSink(Engine::LogSink sink) {
        std::lock_guard<std::mutex> lock(mMutex);
        mSink = std::move(sink);
    }

    std::string apiVersion() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mApiVersion;
    }

    void messageLogged(const Ogre::String &message, Ogre::LogMessageLevel lml,
                       bool /*maskDebug*/, const Ogre::String & /*logName*/,
                       bool &skipThisMessage) override {
        // Rule 3, restated where it can be violated: never suppress Ogre's own
        // output. The sibling file is somebody's forensics.
        skipThisMessage = false;

        // THE API-VERSION SCRAPE. RenderSystemCapabilities carries no API
        // version and the Vulkan render system states it exactly once, as
        // "Vulkan: API Version: 1.4.0 (0x...)" (OgreVulkanRenderSystem.cpp).
        // Same "the verdict exists only as a log line" pattern the shader
        // cache's pipeline verdict already uses, and the reason this listener
        // must be attached before the render system initialises.
        static const char kApiPrefix[] = "Vulkan: API Version: ";
        constexpr size_t kApiPrefixLen = sizeof(kApiPrefix) - 1;
        if (message.compare(0, kApiPrefixLen, kApiPrefix) == 0) {
            std::lock_guard<std::mutex> lock(mMutex);
            mApiVersion = message.substr(kApiPrefixLen);
            // Trim the parenthesised hex form: the printable version is what a
            // human reads out of a header block.
            const size_t paren = mApiVersion.find(" (");
            if (paren != std::string::npos) mApiVersion.resize(paren);
        }

        Engine::LogSink sink;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            sink = mSink;
        }
        if (sink) sink(lml == Ogre::LML_CRITICAL ? 1 : 0, message);
    }

private:
    mutable std::mutex mMutex;
    Engine::LogSink mSink;
    std::string mApiVersion;
};

// ---------------------------------------------------------------------------
void OgreEngine::attachLogBridge() {
    if (mLogBridge) return;
    mLogBridge = new LogBridge;
    if (Ogre::LogManager::getSingletonPtr() && Ogre::LogManager::getSingleton().getDefaultLog())
        Ogre::LogManager::getSingleton().getDefaultLog()->addListener(mLogBridge);
}

void OgreEngine::detachLogBridge() {
    if (!mLogBridge) return;
    // MUST run before Root is deleted, exactly like the shader cache's
    // counters: the Log outlives nothing.
    if (Ogre::LogManager::getSingletonPtr() && Ogre::LogManager::getSingleton().getDefaultLog())
        Ogre::LogManager::getSingleton().getDefaultLog()->removeListener(mLogBridge);
    delete mLogBridge;
    mLogBridge = nullptr;
}

void OgreEngine::setLogSink(Engine::LogSink sink) {
    if (!mLogBridge) return;
    mLogBridge->setSink(std::move(sink));
}

DeviceInfo OgreEngine::deviceInfo() const {
    DeviceInfo info;
    if (!mRoot) return info;
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    if (!rs) return info;
    info.renderSystem = rs->getName();
    // The capabilities object does not exist until the render system has been
    // initialised against a device — under the NULL system it exists but says
    // very little, which is correct and is not an error.
    if (const Ogre::RenderSystemCapabilities *caps = rs->getCapabilities()) {
        info.vendor = Ogre::RenderSystemCapabilities::vendorToString(caps->getVendor());
        info.deviceName = caps->getDeviceName();
        info.driverVersion = caps->getDriverVersion().toString();
    }
    if (mLogBridge) info.apiVersion = mLogBridge->apiVersion();
    return info;
}

}  // namespace detail
}}  // namespace jahshaka::engine
