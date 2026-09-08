// Photometric (IES) light profiles and area-light mask textures.
//
// One more Ogre-private translation unit behind EnginePrivate.h. Both features
// are process-wide singletons (see the header comment for WHY), armed lazily on
// first use and torn down by OgreEngine's destructor before `delete Root`.
#include "EnginePrivate.h"

#include <LightProfiles/OgreLightProfiles.h>

#include <map>
#include <set>

namespace jahshaka { namespace engine { namespace detail {
namespace lightextras {

namespace {

// ---- IES profiles ---------------------------------------------------------
Ogre::LightProfiles *gProfiles = nullptr;
/// Resource-location directories already registered in the "Jahshaka" group.
std::set<std::string> gProfileDirs;
/// Absolute path -> the resource NAME (the bare file name) it was loaded under.
/// LightProfiles keys its own map by that name alone, so two different files
/// that happen to share a name would silently resolve to the first one loaded.
/// CAS object files are named by content hash, so this cannot happen in the
/// product; we detect it and refuse rather than render the wrong lobe.
std::map<std::string, std::string> gProfilePaths;
/// The reverse index, for exactly that collision check.
std::map<std::string, std::string> gProfileNames;

// ---- Area-light masks -----------------------------------------------------
Ogre::TextureGpu *gMaskPool = nullptr;
/// Absolute path -> the pooled texture holding it. Textures live for the
/// process: masks are small, the pool is fixed, and a light may be re-assigned
/// the same mask any frame.
std::map<std::string, Ogre::TextureGpu *> gMasks;
bool gAreaBudgetsRaised = false;
/// Has loadLtcMatrix run against the LIVE Root? Reset by shutdown() — see
/// armLtcMatrix's header comment for why this cannot be a function-local static.
bool gLtcLoaded = false;

Ogre::HlmsPbs *pbsOf(Ogre::Root *root) {
    if (!root || !root->getHlmsManager()) return nullptr;
    return static_cast<Ogre::HlmsPbs *>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
}

void splitPath(const std::string &path, std::string &dir, std::string &file) {
    const size_t slash = path.find_last_of("/\\");
    dir  = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    file = slash == std::string::npos ? path : path.substr(slash + 1);
}

const char *kGroup = "Jahshaka";

Ogre::ResourceGroupManager &rgm() { return Ogre::ResourceGroupManager::getSingleton(); }

}  // namespace

// ---------------------------------------------------------------------------
bool assignProfile(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                   std::string &error) {
    if (!light) { error = "assignProfile: no light"; return false; }
    if (path.empty()) {
        // Nothing was ever armed => every light already reads atlas row 0 (white).
        if (!gProfiles) return true;
        JAH_TRY { gProfiles->assignProfile(Ogre::IdString(), light); return true; }
        JAH_CATCH(error, false);
    }

    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) { error = "assignProfile: HlmsPbs is not registered yet"; return false; }

    JAH_TRY {
        std::string dir, file;
        splitPath(path, dir, file);

        auto known = gProfilePaths.find(path);
        if (known == gProfilePaths.end()) {
            auto clash = gProfileNames.find(file);
            if (clash != gProfileNames.end() && clash->second != path) {
                error = "light profile '" + file + "' is already loaded from a different file (" +
                        clash->second + "); Ogre's profile registry is keyed by file NAME, so "
                        "loading both would silently apply the first one's lobe";
                return false;
            }

            if (!rgm().resourceGroupExists(kGroup)) rgm().createResourceGroup(kGroup, false);
            if (!gProfileDirs.count(dir)) {
                rgm().addResourceLocation(dir, "FileSystem", kGroup, false);
                gProfileDirs.insert(dir);
            }
            if (!rgm().resourceExists(kGroup, file)) {
                error = "light profile not found: " + path;
                return false;
            }

            // Arm on FIRST profile only: the atlas is bound in every pass.
            if (!gProfiles) {
                gProfiles = new Ogre::LightProfiles(
                    pbs, root->getRenderSystem()->getTextureGpuManager());
            }
            gProfiles->loadIesProfile(file, kGroup, /*throwOnDuplicate*/ false);
            // build() recreates and re-uploads the atlas texture. It runs ONLY
            // here — on the frame a genuinely new profile first appears — never
            // on the per-frame setLight path.
            gProfiles->build();
            gProfilePaths[path] = file;
            gProfileNames[file] = path;
        }

        gProfiles->assignProfile(Ogre::IdString(gProfilePaths[path]), light);
        return true;
    } JAH_CATCH(error, false);
}

// ---------------------------------------------------------------------------
void armAreaLightBudgets(Ogre::Root *root) {
    if (gAreaBudgetsRaised) return;
    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) return;
    gAreaBudgetsRaised = true;
    try {
        pbs->setAreaLightForwardSettings(kAreaApproxLimit, kAreaLtcLimit);
    } catch (...) {}
}

// ---------------------------------------------------------------------------
void armLtcMatrix(Ogre::Root *root) {
    if (gLtcLoaded) return;
    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) return;
    gLtcLoaded = true;   // even a failed attempt: don't retry every frame
    try {
        pbs->loadLtcMatrix();
    } catch (...) {}
}

// ---------------------------------------------------------------------------
bool assignAreaMask(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                    std::string &error) {
    if (!light) { error = "assignAreaMask: no light"; return false; }
    if (path.empty()) {
        if (light->getTexture()) light->setTexture(nullptr);
        return true;
    }

    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) { error = "assignAreaMask: HlmsPbs is not registered yet"; return false; }

    JAH_TRY {
        auto cached = gMasks.find(path);
        if (cached == gMasks.end()) {
            Ogre::TextureGpuManager *tm = root->getRenderSystem()->getTextureGpuManager();

            std::string dir, file;
            splitPath(path, dir, file);
            if (!rgm().resourceGroupExists(kGroup)) rgm().createResourceGroup(kGroup, false);
            if (!gProfileDirs.count(dir)) {
                rgm().addResourceLocation(dir, "FileSystem", kGroup, false);
                gProfileDirs.insert(dir);
            }
            if (!rgm().resourceExists(kGroup, file)) {
                error = "area light mask not found: " + path;
                return false;
            }

            // Arm on the FIRST mask only (a bound array costs a slot per pass).
            if (!gMaskPool) {
                const Ogre::uint8 mips = Ogre::PixelFormatGpuUtils::getMaxMipmapCount(
                    kMaskResolution, kMaskResolution);
                gMaskPool = tm->reservePoolId(kMaskPoolId, kMaskResolution, kMaskResolution,
                                              kMaskSlices, mips, kMaskFormat);
                pbs->setAreaLightMasks(gMaskPool);
                armAreaLightBudgets(root);
            }
            if (gMasks.size() >= kMaskSlices) {
                error = "area light mask pool is full (" + std::to_string(kMaskSlices) +
                        " distinct masks per process)";
                return false;
            }

            // Decode on the CPU (load2 sniffs content when the extension lies —
            // the same tolerance loadTexture needs), then force the image into
            // the pool's EXACT shape. A mismatched image would land in a
            // different pool and the light would sample the wrong texture.
            Ogre::Image2 *img = new Ogre::Image2();
            {
                Ogre::DataStreamPtr stream = rgm().openResource(file, kGroup);
                img->load2(stream, file);
            }
            if (img->getWidth() != kMaskResolution || img->getHeight() != kMaskResolution)
                img->resize(kMaskResolution, kMaskResolution, Ogre::Image2::FILTER_BILINEAR);
            // Pool-wide RGBA_sRGB: convert whatever came in (grayscale gobos are
            // common) so the slice's texels mean what the shader thinks.
            if (img->getPixelFormat() != kMaskFormat) {
                Ogre::Image2 *rgba = new Ogre::Image2();
                rgba->createEmptyImage(kMaskResolution, kMaskResolution, 1u,
                                       Ogre::TextureTypes::Type2D, kMaskFormat, 1u);
                const bool oneChannel =
                    Ogre::PixelFormatGpuUtils::getNumberOfComponents(img->getPixelFormat()) == 1;
                for (Ogre::uint32 y = 0; y < kMaskResolution; ++y)
                    for (Ogre::uint32 x = 0; x < kMaskResolution; ++x) {
                        Ogre::ColourValue c = img->getColourAt(x, y, 0);
                        if (oneChannel) { c.g = c.b = c.r; }
                        c.a = 1.0f;
                        rgba->setColourAt(c, x, y, 0);
                    }
                delete img;
                img = rgba;
            }
            // A FULL mip chain is mandatory, not an optimisation: the diffuse
            // term samples near the smallest mip (mTexLightMaskDiffuseMipStart
            // defaults to 0.95 of the chain) and the specular term samples by
            // roughness. Without mips the mask reads hard and wrong.
            img->generateMipmaps(/*gammaCorrected*/ true, Ogre::Image2::FILTER_BILINEAR);

            Ogre::TextureGpu *tex = tm->createTexture(
                processUniqueName("areamask"), Ogre::GpuPageOutStrategy::Discard,
                Ogre::TextureFlags::AutomaticBatching, Ogre::TextureTypes::Type2D,
                Ogre::BLANKSTRING, 0, kMaskPoolId);
            tex->setResolution(kMaskResolution, kMaskResolution);
            tex->setPixelFormat(kMaskFormat);
            tex->setNumMipmaps(img->getNumMipmaps());
            tex->scheduleTransitionTo(Ogre::GpuResidency::Resident, img, true);   // deletes img
            tex->waitForData();

            cached = gMasks.emplace(path, tex).first;
        }

        // setTexture reads getInternalSliceStart(), so the texture must already
        // be resident (it is: waitForData above). Slice bookkeeping afterwards
        // is automatic through Light::notifyTextureChanged.
        light->setTexture(cached->second);
        return true;
    } JAH_CATCH(error, false);
}

// ---------------------------------------------------------------------------
void shutdown() {
    // Order matters: the profiles object unbinds itself from HlmsPbs as it
    // destroys its atlas, and every texture must die before its manager.
    delete gProfiles;
    gProfiles = nullptr;
    gProfileDirs.clear();
    gProfilePaths.clear();
    gProfileNames.clear();

    if (gMaskPool) {
        try {
            Ogre::HlmsPbs *pbs = nullptr;
            if (Ogre::Root::getSingletonPtr() && Ogre::Root::getSingleton().getHlmsManager())
                pbs = static_cast<Ogre::HlmsPbs *>(
                    Ogre::Root::getSingleton().getHlmsManager()->getHlms(Ogre::HLMS_PBS));
            if (pbs) pbs->setAreaLightMasks(nullptr);
            Ogre::TextureGpuManager *tm = gMaskPool->getTextureManager();
            for (auto &kv : gMasks) tm->destroyTexture(kv.second);
            tm->destroyTexture(gMaskPool);
        } catch (...) {}
    }
    gMasks.clear();
    gMaskPool = nullptr;
    gAreaBudgetsRaised = false;
    // The LTC matrix belongs to the Root's HlmsPbs, which is about to die: the
    // next Engine must load it again or its area lights render unlit.
    gLtcLoaded = false;
}

}  // namespace lightextras
}}}  // namespace jahshaka::engine::detail
