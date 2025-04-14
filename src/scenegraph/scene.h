/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef SCENE_H
#define SCENE_H

#include <QList>
#include "../irisglfwd.h"
#include "../graphics/texture2d.h"
#include "../materials/defaultskymaterial.h"
#include "../geometry/frustum.h"

// temp
#include <QJsonObject>

class QMediaPlayer;
class QMediaPlaylist;

namespace iris
{

class RenderItem;
class RenderList;
class Environment;

enum class SceneRenderFlags : int
{
    Vr = 0x1
};

struct PickingResult
{
    iris::SceneNodePtr hitNode;
    QVector3D hitPoint;

    float distanceFromStartSqrd;
};

enum class SkyType : int
{
	SINGLE_COLOR = 0,
	CUBEMAP,
	EQUIRECTANGULAR,
	GRADIENT,
	MATERIAL,
	REALISTIC
};

struct SkyRealistic
{
	float luminance;
	float reileigh;
	float mieCoefficient;
	float mieDirectionalG;
	float turbidity;
	float sunPosX;
	float sunPosY;
	float sunPosZ;
};

class Scene: public QEnableSharedFromThis<Scene>
{
    QSharedPointer<Environment> environment;

	friend class ForwardRenderer;

public:
    CameraNodePtr camera;
    SceneNodePtr rootNode;

    QSharedPointer<Environment> getPhysicsEnvironment() {
        return environment;
    }

    /*
     * This is the default viewer that the scene
     * will use when playing in vr mode
     */
    ViewerNodePtr vrViewer;

    QHash<QString, LightNodePtr> lights;
	QHash<QString, MeshNodePtr> meshes;
	QHash<QString, ParticleSystemNodePtr> particleSystems;
	QHash<QString, ViewerNodePtr> viewers;
	QHash<QString, GrabNodePtr> grabbers;
	QHash<QString, SceneNodePtr> nodes;

    QColor clearColor;
    bool renderSky;
    MeshPtr skyMesh;
    Texture2DPtr skyTexture;
    QColor skyColor;
    QColor ambientColor;
    DefaultSkyMaterialPtr skyMaterial;
    RenderItem* skyRenderItem;
	QColor gradientTop;
	QColor gradientMid;
	QColor gradientBot;
	float gradientOffset;

	TextureCubePtr skyCapture;
	bool skyCaptured = false; // has the sky been captured?
	bool shouldCaptureSky = true; // should the sky be captured on the next frame?
	int skyCaptureSize = 1024;
	bool shouldResizeSky = false;

    // fog properties
    QColor fogColor;
    float fogStart;
    float fogEnd;
    bool fogEnabled;

    float gravity;
    bool shadowEnabled;

	SkyType skyType;
	SkyRealistic skyRealistic;
    QString skyGuid;
    QString ambientMusicGuid;

	QJsonObject skyDataSingleColor;
	QJsonObject skyDataRealistic;
	QJsonObject skyDataGradient;
	QJsonObject skyDataEqui;
	QJsonObject skyDataCubemap;
	QJsonObject skyDataMaterial;

	QMap<QString, QJsonObject> skyData;

    RenderList* geometryRenderList;
    RenderList* shadowRenderList;
    RenderList* gizmoRenderList;// for gizmos and lines

	void setWorldGravity(float gravity);

    QString skyBoxTextures[6];

    /*
     * customizations that can be passed in and applied to a scene. ideally these
     * should or can be GLOBAL but a scene is the highest prioritized obj atm...
     * @future maybe have a __GlobalWorldSettings__ object?
     * @future todo could include camera speed, motion blur px, clipping (near/far plane) pos
     */
    int outlineWidth;
    QColor outlineColor;

	// time counter to pass to shaders that do time-based animation
	float time;

	// needed for playing music
	QMediaPlayer* mediaPlayer;
	// a playlist is needed to play looping sounds
	QMediaPlaylist* playList;
	QString ambientMusicPath;
	float ambientMusicVolume;

    Scene();
public:
    static ScenePtr create();

    /**
     * Returns the scene's root node. A scene should always have a root node so it should be assumed
     * that the returned value is never null.
     * @return
     */
    SceneNodePtr getRootNode() {
        return rootNode;
    }

	QStringList skyTypeToStr = {
		"SingleColor",
		"Cubemap",
		"Equirectangular",
		"Gradient",
		"Material",
		"Realistic"
	};

	void switchSkyTexture(iris::SkyType skyType);

    void setSkyTexture(Texture2DPtr tex);
    void setSkyTextureSource(QString src) {
        skyTexture->source = src;
    }

	float getRunningTime()
	{
		return time;
	}

    QString getSkyTextureSource();
    void clearSkyTexture();
    void setSkyColor(QColor color);
    void setAmbientColor(QColor color);

	// causes the sky to be recaptured each frame
	void queueSkyCapture();

	void setAmbientMusic(QString path);
	void stopPlayingAmbientMusic();
	void startPlayingAmbientMusic();
	void setAmbientMusicVolume(float volume);

    void updateSceneAnimation(float time);
    void update(float dt);
    void render();

    void rayCast(const QVector3D& segStart,
                 const QVector3D& segEnd,
                 QList<PickingResult>& hitList,
			     uint64_t pickingMask = 0,
				 bool allowUnpickable = false);

    void rayCast(const QSharedPointer<iris::SceneNode>& sceneNode,
                 const QVector3D& segStart,
                 const QVector3D& segEnd,
                 QList<iris::PickingResult>& hitList,
				 uint64_t pickingMask = 0,
				 bool allowUnpickable = false);

	ViewerNodePtr getActiveVrViewer() { return vrViewer; }
	ViewerNodePtr setActiveVrViewer(ViewerNodePtr viewer) { this->vrViewer = viewer; }

    /**
     * Adds node to scene. If node is a LightNode then it is added to a list of lights.
     * @param node
     */
    void addNode(SceneNodePtr node);

    /**
     *  Removes node from scene. If node is a LightNode then it is removed to a list of lights.
     * @param node
     */
    void removeNode(SceneNodePtr node);

    /**
     * Sets the active camera of the scene
     * @param cameraNode
     */
    void setCamera(CameraNodePtr cameraNode);

	/*
	Return scene's active camera
	*/
	iris::CameraNodePtr getCamera() { return camera; }

    /**
     * Sets the viewport stencil width
     * @param width
     */
    void setOutlineWidth(int width);

    /**
     * Sets the viewport stencil color
     * @param color
     */
    void setOutlineColor(QColor color);

    void cleanup();
};

}


#endif // SCENE_H
