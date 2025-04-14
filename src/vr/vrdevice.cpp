/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "vrdevice.h"
#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLTexture>

#include <QOpenGLContext>
//#include "../libovr/Include/OVR_CAPI_GL.h"
#include "../libovr/Include/Extras/OVR_Math.h"
#include "../core/logger.h"

using namespace OVR;

namespace iris
{

struct VrFrameData
{
    ovrEyeRenderDesc eyeRenderDesc[2];
    ovrPosef eyeRenderPose[2];
    ovrVector3f hmdToEyeOffset[2];
    double sensorSampleTime;
};

VrTouchController::VrTouchController(int index)
{
    this->index = index;
	this->isBeingTracked = false;
}

bool VrTouchController::isButtonDown(VrTouchInput btn)
{
    return isButtonDown(inputState, btn);
}

bool VrTouchController::isButtonDown(const ovrInputState& state, VrTouchInput btn)
{
    return (state.Buttons & (int)btn) != 0;
}

void VrTouchController::setTrackingState(bool state)
{
    isBeingTracked = state;
}

bool VrTouchController::isButtonUp(VrTouchInput btn)
{
    return !isButtonDown(btn);
}

bool VrTouchController::isButtonPressed(VrTouchInput btn)
{
    return !isButtonDown(prevInputState, btn) && isButtonDown(inputState, btn);
}

bool VrTouchController::isButtonReleased(VrTouchInput btn)
{
    return isButtonDown(prevInputState, btn) && !isButtonDown(inputState, btn);
}

QVector2D VrTouchController::GetThumbstick()
{
    auto value = inputState.Thumbstick[index];
    return QVector2D(value.x, value.y);
}

bool VrTouchController::isTracking()
{
    return isBeingTracked;
}

float VrTouchController::getIndexTrigger()
{
    return inputState.IndexTrigger[index];
}

bool VrTouchController::isIndexTriggerPressed()
{
	return prevInputState.IndexTrigger[index] < 0.1f && inputState.IndexTrigger[index] > 0.1f;
}

bool VrTouchController::isIndexTriggerReleased()
{
	return prevInputState.IndexTrigger[index] > 0.1f && inputState.IndexTrigger[index] < 0.1f;
}

float VrTouchController::getHandTrigger()
{
    return inputState.HandTrigger[index];
}

VrDevice::VrDevice()
{
    this->gl = new QOpenGLFunctions_3_2_Core();
    this->gl->initializeOpenGLFunctions();
    vrSupported = false;
    frameData = new VrFrameData();

    touchControllers[0] = new VrTouchController(0);
    touchControllers[1] = new VrTouchController(1);

    mirrorTexture = nullptr;

    initialized = false;
}

bool VrDevice::isVrSupported()
{
    return vrSupported;
}

void VrDevice::initialize()
{
    // Oculus only gives one session per application it seems
    // so this part must only be done once
    // The graphics resources however must be recreated for each new opengl context
    if (!initialized){
        ovrResult result = ovr_Initialize(nullptr);
        if (!OVR_SUCCESS(result)) {
            Logger::getSingleton()->warn("Failed to initialize libOVR.");
            return;
        }

        result = ovr_Create(&session, &luid);
        if (!OVR_SUCCESS(result)) {
            Logger::getSingleton()->warn("Could not create libOVR session!");
            return;
        }

        hmdDesc = ovr_GetHmdDesc(session);
        initialized = true;
    

		// intialize framebuffers necessary for rendering to the hmd
		for (int eye = 0; eye < 2; ++eye) {
			ovrSizei texSize = ovr_GetFovTextureSize(session,
													 ovrEyeType(eye),
													 hmdDesc.DefaultEyeFov[eye], 1);
			createTextureChain(session, vr_textureChain[eye], texSize.w, texSize.h );
			vr_depthTexture[eye] = createDepthTexture(texSize.w, texSize.h);

			// should be the same for all
			eyeWidth = texSize.w;
			eyeHeight = texSize.h;
		}

		setTrackingOrigin(VrTrackingOrigin::EyeLevel);

		vrSupported = true;

		gl->glGenFramebuffers(2, vr_Fbo);
		createMirrorFbo(800, 600);
	}
}

void VrDevice::setTrackingOrigin(VrTrackingOrigin trackingOrigin)
{
    this->trackingOrigin = trackingOrigin;

    if(trackingOrigin == VrTrackingOrigin::FloorLevel)
        ovr_SetTrackingOriginType(session, ovrTrackingOrigin_FloorLevel);
    else
        ovr_SetTrackingOriginType(session, ovrTrackingOrigin_EyeLevel);
}

void VrDevice::destroySwapChain(VrSwapChain* swapChain)
{
	gl->glDeleteTextures(2, swapChain->eyeFBOs);
	gl->glDeleteTextures(1, &swapChain->mirrorFBO);

	delete swapChain;
}

GLuint VrDevice::createDepthTexture(int width,int height)
{
    GLuint texId;
    gl->glGenTextures(1, &texId);
    gl->glBindTexture(GL_TEXTURE_2D, texId);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLenum internalFormat = GL_DEPTH_COMPONENT24;
    GLenum type = GL_UNSIGNED_INT;
    //GLenum internalFormat = GL_DEPTH_COMPONENT32F;
    //GLenum type = GL_FLOAT;

    gl->glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_DEPTH_COMPONENT, type, NULL);

    return texId;
}

ovrTextureSwapChain VrDevice::createTextureChain(ovrSession session,
                                                 ovrTextureSwapChain &swapChain,
                                                 int width,
                                                 int height)
{
    ovrTextureSwapChainDesc desc = {};
    desc.Type = ovrTexture_2D;
    desc.ArraySize = 1;
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleCount = 1;
    desc.StaticImage = ovrFalse;

    ovrResult result = ovr_CreateTextureSwapChainGL(session, &desc, &swapChain);
    if (!OVR_SUCCESS(result)) {
        Logger::getSingleton()->warn("could not create swap chain!");
        return nullptr;
    }

    int length = 0;
    result = ovr_GetTextureSwapChainLength(session, swapChain, &length);
    if (!OVR_SUCCESS(result)) {
        Logger::getSingleton()->warn("could not get swapchain length!");
        return nullptr;
    }

    for (int i = 0; i < length; ++i) {
        GLuint chainTexId;
        ovr_GetTextureSwapChainBufferGL(session, swapChain, i, &chainTexId);
        gl->glBindTexture(GL_TEXTURE_2D, chainTexId);

        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    return swapChain;
}

GLuint VrDevice::createMirrorFbo(int width,int height)
{
    // delete current mirror texture
    if (mirrorTexture != nullptr)
        ovr_DestroyMirrorTexture(session, mirrorTexture);

    ovrMirrorTextureDesc desc;
    memset(&desc, 0, sizeof(desc));
    //todo: use actual viewport size
    desc.Width = width;
    desc.Height = height;
    desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;

    // Create mirror texture and an FBO used to copy mirror texture to back buffer
    ovrResult result = ovr_CreateMirrorTextureGL(session, &desc, &mirrorTexture);
    if (!OVR_SUCCESS(result))
    {
        Logger::getSingleton()->warn("Failed to create mirror texture.");
        return 0;
    }

    // Configure the mirror read buffer
    // no need to store this texture anywhere
    ovr_GetMirrorTextureBufferGL(session, mirrorTexture, &vr_mirrorTexId);
}


void VrDevice::beginFrame()
{
    frameData->eyeRenderDesc[0] = ovr_GetRenderDesc(session, ovrEye_Left, hmdDesc.DefaultEyeFov[0]);
    frameData->eyeRenderDesc[1] = ovr_GetRenderDesc(session, ovrEye_Right, hmdDesc.DefaultEyeFov[1]);

    frameData->hmdToEyeOffset[0] = frameData->eyeRenderDesc[0].HmdToEyeOffset;
    frameData->hmdToEyeOffset[1] = frameData->eyeRenderDesc[1].HmdToEyeOffset;

    ovr_GetEyePoses(session, frameIndex, ovrTrue, frameData->hmdToEyeOffset, frameData->eyeRenderPose, &frameData->sensorSampleTime);

    double timing = ovr_GetPredictedDisplayTime(session, 0);
    hmdState = ovr_GetTrackingState(session, timing, ovrTrue);

    for (int i=0; i<2; i++) {

        touchControllers[i]->prevInputState = touchControllers[i]->inputState;
        if( !OVR_SUCCESS(ovr_GetInputState(session,
                                           i == 0? ovrControllerType_LTouch : ovrControllerType_RTouch,
                                           &touchControllers[i]->inputState))) {
            Logger::getSingleton()->warn(QString("Unable to get input state of controller %1").arg(i));
        }
    }

    auto contTypes = ovr_GetConnectedControllerTypes(session);

    if(contTypes & ovrControllerType_LTouch)
        touchControllers[0]->isBeingTracked = true;
    else
        touchControllers[0]->isBeingTracked = false;

    if(contTypes & ovrControllerType_RTouch)
        touchControllers[1]->isBeingTracked = true;
    else
        touchControllers[1]->isBeingTracked = false;
}

void VrDevice::endFrame()
{
    ovrLayerEyeFov ld;
    ld.Header.Type  = ovrLayerType_EyeFov;
    ld.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;

    for (int eye = 0; eye < 2; ++eye)
    {
        ld.ColorTexture[eye] = vr_textureChain[eye];
        ld.Viewport[eye]     = Recti(Sizei(eyeWidth,eyeHeight));
        ld.Fov[eye]          = hmdDesc.DefaultEyeFov[eye];
        ld.RenderPose[eye]   = frameData->eyeRenderPose[eye];
        ld.SensorSampleTime  = frameData->sensorSampleTime;
    }

    ovrLayerHeader* layers = &ld.Header;
    ovrResult result = ovr_SubmitFrame(session, frameIndex, nullptr, &layers, 1);

    if (!OVR_SUCCESS(result))
    {
        Logger::getSingleton()->warn(QString("Error submitting frame to hmd!"));
    }

    frameIndex++;
}

QVector3D VrDevice::getHandPosition(int handIndex)
{
    auto handPos = hmdState.HandPoses[handIndex].ThePose.Position;
    return QVector3D(handPos.x, handPos.y, handPos.z);
}

QQuaternion VrDevice::getHandRotation(int handIndex)
{
    auto handRot = hmdState.HandPoses[handIndex].ThePose.Orientation;
    return QQuaternion(handRot.w, handRot.x, handRot.y, handRot.z);
}

VrTouchController* VrDevice::getTouchController(int index)
{
    return touchControllers[index];
}

QQuaternion VrDevice::getHeadRotation()
{
    auto rot = hmdState.HeadPose.ThePose.Orientation;
    return QQuaternion(rot.w, rot.x, rot.y, rot.z);
}

QVector3D VrDevice::getHeadPos()
{
    auto pos = hmdState.HeadPose.ThePose.Position;
    return QVector3D(pos.x, pos.y, pos.z);
}

VrSwapChain * VrDevice::createSwapChain()
{
	auto swapChain = new VrSwapChain();

	gl->glGenFramebuffers(2, swapChain->eyeFBOs);

	gl->glGenFramebuffers(1, &swapChain->mirrorFBO);
	gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, swapChain->mirrorFBO);
	gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vr_mirrorTexId, 0);
	gl->glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	return swapChain;
}

void VrDevice::regenerateSwapChain()
{
	// destroy old swapchain (does old context need to be bound?)
	ovr_DestroyMirrorTexture(session, mirrorTexture);
	ovr_DestroyTextureSwapChain(session, vr_textureChain[0]);
	ovr_DestroyTextureSwapChain(session, vr_textureChain[1]);

	gl->glDeleteTextures(2, vr_Fbo);
	gl->glDeleteTextures(1, &vr_mirrorTexId);

	// recreate new one
	for (int eye = 0; eye < 2; ++eye) {
		ovrSizei texSize = ovr_GetFovTextureSize(session,
			ovrEyeType(eye),
			hmdDesc.DefaultEyeFov[eye], 1);
		createTextureChain(session, vr_textureChain[eye], texSize.w, texSize.h);
		vr_depthTexture[eye] = createDepthTexture(texSize.w, texSize.h);

		// should be the same for all
		eyeWidth = texSize.w;
		eyeHeight = texSize.h;
	}

	gl->glGenFramebuffers(2, vr_Fbo);
	createMirrorFbo(800, 600);
}

void VrDevice::bindEyeTexture(int eye)
{
	GLuint curTexId;
	int curIndex;

	ovr_GetTextureSwapChainCurrentIndex(session, vr_textureChain[eye], &curIndex);
	ovr_GetTextureSwapChainBufferGL(session, vr_textureChain[eye], curIndex, &curTexId);
	gl->glBindTexture(GL_TEXTURE_2D, curTexId);
}

void VrDevice::beginEye(int eye)
{
    GLuint curTexId;
    int curIndex;

    ovr_GetTextureSwapChainCurrentIndex(session, vr_textureChain[eye], &curIndex);
    ovr_GetTextureSwapChainBufferGL(session, vr_textureChain[eye], curIndex, &curTexId);

    gl->glBindFramebuffer(GL_FRAMEBUFFER, vr_Fbo[eye]);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               curTexId,
                               0);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D,
                               vr_depthTexture[eye],
                               0);

    gl->glViewport(0, 0, eyeWidth, eyeHeight);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    //gl->glEnable(GL_FRAMEBUFFER_SRGB);
}

void VrDevice::endEye(int eye)
{
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

    ovr_CommitTextureSwapChain(session, vr_textureChain[eye]);
	gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool VrDevice::isHeadMounted()
{
    if (session == nullptr) return false;
    ovrSessionStatus sessionStatus;

    ovr_GetSessionStatus(session, &sessionStatus);
    return sessionStatus.HmdMounted;
}

QMatrix4x4 VrDevice::getEyeViewMatrix(int eye, QVector3D pivot, QMatrix4x4 transform, float vrScale)
{
    auto r = frameData->eyeRenderPose[eye].Orientation;
    auto finalYawPitchRoll = QMatrix4x4(QQuaternion(r.w, r.x, r.y, r.z).toRotationMatrix());
    auto finalUp = finalYawPitchRoll * QVector3D(0, 1, 0);
    auto finalForward = finalYawPitchRoll * QVector3D(0, 0, -1);

    auto fd = frameData->eyeRenderPose[eye].Position;
    auto framePos = QVector3D(fd.x, fd.y, fd.z) * vrScale;
    auto shiftedEyePos = framePos;
    //qDebug() << shiftedEyePos;
    //auto forward = shiftedEyePos + finalForward;
    auto forward = shiftedEyePos + finalForward;

    QMatrix4x4 view;
    view.setToIdentity();
    view.lookAt(transform * shiftedEyePos,
                transform * forward,
                QQuaternion::fromRotationMatrix(transform.normalMatrix()) * finalUp);

    return view;

}

QMatrix4x4 VrDevice::getEyeProjMatrix(int eye,float nearClip,float farClip)
{
    QMatrix4x4 proj;
    proj.setToIdentity();
    Matrix4f eyeProj = ovrMatrix4f_Projection(hmdDesc.DefaultEyeFov[eye],
                                              nearClip,
                                              farClip,
                                              ovrProjection_None);

    for (int r = 0; r < 4; r++) {
        for (int c = 0;c < 4; c++) {
            proj(r, c) = eyeProj.M[r][c];
        }
    }

    return proj;
}

GLuint VrDevice::bindMirrorTextureId()
{
    gl->glBindTexture(GL_TEXTURE_2D, vr_mirrorTexId);
    return vr_mirrorTexId;
}

}
