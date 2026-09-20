// THE SHADER-DRAWN EDITOR GRID (GRID-2; Types.h GridDesc; media JahGrid.material).
//
// The sun disc's mechanism, for a helper: one Rectangle2D per scene at render
// queue 14 — a v2 FAST queue after opaque geometry (10) and before the particles
// (15), inside the opaque pass's [0, 210) so the overlay pass never sees it —
// whose fragment program intersects the camera ray with the grid plane, draws
// the lines analytically and writes the plane point's own depth (the .glsl has
// the physics). Visibility kHelperBit: every probe capture, shadow node, planar
// reflector, the Player and the Scene-grade screenshot exclude the helper
// channel already, so the grid is out of all of them by construction.
//
// Why a screen quad and not a plane mesh: a mesh has an extent and a triangle
// count, a screen quad has neither — the grid is infinite and costs one
// full-screen fragment pass whose early exit (`t <= 0`: the plane is behind the
// camera, or the ray is parallel) discards half the screen on a level view.
#include "EnginePrivate.h"

#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreRectangle2D2.h>
#include <OgreTechnique.h>

namespace jahshaka { namespace engine { namespace detail {
namespace {
// After opaque geometry (10), before the particle systems (15): the grid blends
// over the floor and the particles composite over the grid.
constexpr Ogre::uint8 kGridRenderQueue = 14u;
}  // namespace

bool OgreScene::setGrid(const GridDesc &desc) {
    JAH_TRY {
        if (desc == mGridDesc) return true;
        if (!desc.enabled) {
            // Hidden, not destroyed: the grid is toggled every day. The
            // BillboardSet2 rule applied to a Rectangle2D — visibility is an
            // include channel, so "no channel" is invisible everywhere.
            if (mGrid) mGrid->setVisibilityFlags(0u);
            mGridDesc = desc;
            return true;
        }
        if (!mGrid) {
            mGrid = mSceneMgr->createRectangle2D(Ogre::SCENE_STATIC);
            mGrid->initialize(Ogre::BT_DEFAULT, Ogre::Rectangle2D::GeometryFlagQuad);
            mGrid->setGeometry(-Ogre::Vector2::UNIT_SCALE, Ogre::Vector2(2.0f));
            // Rectangle2D's position and size are UNINITIALISED members and
            // setGeometry only raises a dirty flag: update() is not optional
            // (OgreSky.cpp's sun disc has the story).
            mGrid->update();
            mGrid->setUseIdentityView(false);
            mGrid->setUseIdentityProjection(false);
            mGrid->setRenderQueueGroup(kGridRenderQueue);
            mGrid->setCastShadows(false);
            mSceneMgr->getRootSceneNode(Ogre::SCENE_STATIC)->attachObject(mGrid);
            mSceneMgr->notifyStaticAabbDirty(mGrid);

            Ogre::MaterialManager &mm = Ogre::MaterialManager::getSingleton();
            const Ogre::String name = "Jahshaka/Grid" +
                                      Ogre::StringConverter::toString(mSceneMgr->getId());
            mGridMaterial = mm.getByName(name);
            if (!mGridMaterial) {
                Ogre::MaterialPtr base = std::static_pointer_cast<Ogre::Material>(
                    mm.load("Jahshaka/Grid",
                            Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
                if (!base) {
                    mError = "grid: Jahshaka/Grid material is not staged";
                    mSceneMgr->destroyRectangle2D(mGrid);
                    mGrid = nullptr;
                    return false;
                }
                mGridMaterial = base->clone(name);
                mGridMaterial->load();
            }
            mGrid->setMaterial(mGridMaterial);
        }
        if (!mGridMaterial) return false;

        mGrid->setVisibilityFlags(kHelperBit);
        Ogre::Pass *pass = mGridMaterial->getTechnique(0)->getPass(0);
        Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
        Ogre::Vector4 n, u, v;
        switch (desc.plane) {
        case GridDesc::Plane::FrontXY: n = Ogre::Vector4(0, 0, 1, 0); u = Ogre::Vector4(1, 0, 0, 0); v = Ogre::Vector4(0, 1, 0, 0); break;
        case GridDesc::Plane::SideYZ:  n = Ogre::Vector4(1, 0, 0, 0); u = Ogre::Vector4(0, 0, 1, 0); v = Ogre::Vector4(0, 1, 0, 0); break;
        case GridDesc::Plane::Floor:
        default:                       n = Ogre::Vector4(0, 1, 0, 0); u = Ogre::Vector4(1, 0, 0, 0); v = Ogre::Vector4(0, 0, 1, 0); break;
        }
        ps->setNamedConstant("gridNormal", n);
        ps->setNamedConstant("gridU", u);
        ps->setNamedConstant("gridV", v);
        ps->setNamedConstant("gridParams",
                             Ogre::Vector4(std::max(desc.spacing, 1e-3f), float(std::max(desc.majorEvery, 1)),
                                           std::max(desc.thicknessPx, 0.5f), std::max(desc.fadeDistance, 1.0f)));
        ps->setNamedConstant("minorColour", Ogre::Vector4(desc.minorColour.r, desc.minorColour.g,
                                                          desc.minorColour.b, desc.minorColour.a));
        ps->setNamedConstant("majorColour", Ogre::Vector4(desc.majorColour.r, desc.majorColour.g,
                                                          desc.majorColour.b, desc.majorColour.a));
        mGridDesc = desc;
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::destroyGrid() {
    if (mGrid) { mSceneMgr->destroyRectangle2D(mGrid); mGrid = nullptr; }
    mGridMaterial.reset();
    mGridDesc = GridDesc();   // forget what was pushed: the next push must rebuild
}

}}}  // namespace jahshaka::engine::detail
