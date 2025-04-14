/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef VRDEVICE_H
#define VRDEVICE_H

#include <QMatrix4x4>
#include <QOpenGLContext>
#include "../libovr/Include/OVR_CAPI_GL.h"


class QOpenGLFunctions_3_2_Core;

namespace iris
{

enum class VrTrackingOrigin
{
    EyeLevel,
    FloorLevel
};

enum class VrTouchInput : unsigned int
{
    A                   = ovrButton_A,
    B                   = ovrButton_B,
    RightThumb          = ovrButton_RThumb,
    RightIndexTrigger   = 0x00000010,
    RightShoulder       = ovrButton_RShoulder,

    X                   = ovrButton_X,
    Y                   = ovrButton_Y,
    LeftThumb           = ovrButton_LThumb,
    LeftIndexTrigger    = 0x00001000,
    LeftShoulder        = ovrButton_LShoulder,

    RightIndexPointing  = 0x00000020,
    RightThumbUp        = 0x00000040,

    LeftIndexPointing   = 0x00002000,
    LeftThumbUp         = 0x00004000
};

class VrTouchController
{
    friend class VrDevice;
    ovrInputState inputState;
    ovrInputState prevInputState;

    int index;
    bool isBeingTracked;

public:
    VrTouchController(int index);

    bool isButtonDown(VrTouchInput btn);
    bool isButtonUp(VrTouchInput btn);

    bool isButtonPressed(VrTouchInput btn);
    bool isButtonReleased(VrTouchInput btn);

    QVector2D GetThumbstick();

    bool isTracking();

	float getIndexTrigger();
	bool isIndexTriggerPressed();
	bool isIndexTriggerReleased();

    float getHandTrigger();
private:
    bool isButtonDown(const ovrInputState& state, VrTouchInput btn);
    void setTrackingState(bool state);
};

/*
This class doesnt store the swapchain but the FBOs necessar for the
swap chain to work properly. FBOs arent shared across different OpenGL
contexts so it's necessary to have seperate FBOs for each context.
*/
struct VrSwapChain
{
	GLuint eyeFBOs[2];
	GLuint mirrorFBO;
	//ovrMirrorTexture mirrorTexture;
};

struct VrFrameData;

/**
 * This class provides an interface for the OVR sdk
 */
class VrDevice
{
    friend class VrManager;
    

    
public:
	bool initialized;
	int eyeWidth;
	int eyeHeight;

	VrDevice();

	// Initializes the ovr sdk
	// An OpenGL context is required for this function to work properly
    void initialize();
    void setTrackingOrigin(VrTrackingOrigin trackingOrigin);

    GLuint createMirrorFbo(int width,int height);

    bool isVrSupported();

    void beginFrame();
    void endFrame();

    void beginEye(int eye);
    void endEye(int eye);

	void bindEyeTexture(int eye);

    /*
     * Returns whether or not the headset is being tracked
     * If orientation is being track the the headset is on the persons head
     */
    bool isHeadMounted();

    QMatrix4x4 getEyeViewMatrix(int eye,QVector3D pivot,QMatrix4x4 transform = QMatrix4x4(), float vrScale = 1.0f);
    QMatrix4x4 getEyeProjMatrix(int eye,float nearClip,float farClip);

    GLuint bindMirrorTextureId();

    QVector3D getHandPosition(int handIndex);
    QQuaternion getHandRotation(int handIndex);

    VrTouchController* getTouchController(int index);
    QQuaternion getHeadRotation();
    QVector3D getHeadPos();

	/*
	Creates per-renderer swapchain resources. An active OpenGL context is required for this to work properly.
	*/
	VrSwapChain* createSwapChain();

	/*
	Used to regenerate the swapchain. Swapchains only work properly in the context they were created in.
	This means that it will not work properly in shared contexts (artifacts show when not rendering from it's original
	context). Call this after switching between windows (or contexts) to ensure that swapchain is created on the
	active context.
	*/
	void regenerateSwapChain();

	/*
	Cleans up ovr and gl swapchain resources and deletes swapChain object
	*/
	void destroySwapChain(VrSwapChain* swapChain);

private:
    GLuint createDepthTexture(int width,int height);
    ovrTextureSwapChain createTextureChain(ovrSession session,ovrTextureSwapChain &swapChain,int width,int height);

    GLuint vr_depthTexture[2];
    ovrTextureSwapChain vr_textureChain[2];
    GLuint vr_Fbo[2];

    
    long long frameIndex;

    ovrMirrorTexture mirrorTexture;
    GLuint vr_mirrorFbo;
    GLuint vr_mirrorTexId;

    //quick bool to enable/disable vr rendering
    bool vrSupported;

    ovrSession session;
    ovrGraphicsLuid luid;
    ovrHmdDesc hmdDesc;

    VrTrackingOrigin trackingOrigin;

    QOpenGLFunctions_3_2_Core* gl;
    VrFrameData* frameData;

    ovrTrackingState hmdState;

    VrTouchController* touchControllers[2];
};

}

#endif // VRDEVICE_H
