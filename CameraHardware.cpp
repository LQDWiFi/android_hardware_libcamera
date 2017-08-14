/*
**
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "CameraHardware"

#define DEBUG_FRAME 0

#if DEBUG_FRAME
#define LOG_FRAME ALOGD
#else
#define LOG_FRAME ALOGV
#endif

#include <utils/Log.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h> /* for mode definitions */
#include <unistd.h> /* for sleep */

#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include "CameraHardware.h"
#include "Converter.h"
#include "Metadata.h"

/*  Concurrency:

    There are three threads to consider: 

    * one that Android uses to feed in commands via the
      CameraHardware::mDeviceOps dispatch;

    * the hotplug thread that waits for the camera;

    * the thread that is driving the camera.

    We don't want the camera thread to block forever while waiting
    for a frame. The camera might have gone bad.

    We can't be holding a mutex while waiting for a frame.  This
    will block Android too much.

    The thread system is based around calling a function e.g.
    previewThread() periodically with a check for an exit flag
    between calls.  So GrabRawFrame() must be polled with a time-out.
    The camera thread don't hold a mutex on while running.  If
    Android sends a command that changes the behaviour then the
    camera thread must be stopped first e.g. see stopPreview().
    This will release all buffers so that they can be reallocated.

    We don't have much need for a mutex at all as long as there is
    only one Android thread sending commands. We'll keep one just
    in case.

    The hotplug thread terminates as soon as it sees the camera.
    The mutex allows it to communicate the ready status to the
    Android thread.

*/

static bool
isLocked(android::Mutex& mLock)
{
    if (mLock.tryLock() == 0) {
        mLock.unlock();
        return false;
    }
    return true;
}


#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define UNUSED(x) ((void)(x))

#define MIN_WIDTH       320
#define MIN_HEIGHT      240

#ifndef PIXEL_FORMAT_RGB_888
#define PIXEL_FORMAT_RGB_888 3 /* */
#endif

#ifndef PIXEL_FORMAT_RGBA_8888
#define PIXEL_FORMAT_RGBA_8888 1 /* [ov] */
#endif

#ifndef PIXEL_FORMAT_RGBX_8888
#define PIXEL_FORMAT_RGBX_8888 2
#endif

#ifndef PIXEL_FORMAT_BGRA_8888
#define PIXEL_FORMAT_BGRA_8888 5 /* [ov] */
#endif

#ifndef PIXEL_FORMAT_RGB_565
#define PIXEL_FORMAT_RGB_565  4 /* [ov] */
#endif

// We need this format to allow special preview modes
#ifndef PIXEL_FORMAT_YCrCb_422_I
#define PIXEL_FORMAT_YCrCb_422_I 100
#endif

#ifndef PIXEL_FORMAT_YCbCr_422_SP
#define PIXEL_FORMAT_YCbCr_422_SP 0x10    /* NV16  [ov] */
#endif

#ifndef PIXEL_FORMAT_YCbCr_420_SP
#define PIXEL_FORMAT_YCbCr_420_SP 0x21    /* NV12 */
#endif

#ifndef PIXEL_FORMAT_UNKNOWN
#define PIXEL_FORMAT_UNKNOWN 0
#endif

    /*
     * Android YUV format:
     *
     * This format is exposed outside of the HAL to software
     * decoders and applications.
     * EGLImageKHR must support it in conjunction with the
     * OES_EGL_image_external extension.
     *
     * YV12 is 4:2:0 YCrCb planar format comprised of a WxH Y plane followed
     * by (W/2) x (H/2) Cr and Cb planes.
     *
     * This format assumes
     * - an even width
     * - an even height
     * - a horizontal stride multiple of 16 pixels
     * - a vertical stride equal to the height
     *
     *   y_size = stride * height
     *   c_size = ALIGN(stride/2, 16) * height/2
     *   size = y_size + c_size * 2
     *   cr_offset = y_size
     *   cb_offset = y_size + c_size
     *
     */
#ifndef PIXEL_FORMAT_YV12
#define PIXEL_FORMAT_YV12  0x32315659 /* YCrCb 4:2:0 Planar */
#endif

#ifndef PIXEL_FORMAT_YV16
#define PIXEL_FORMAT_YV16  0x36315659 /* YCrCb 4:2:2 Planar */
#endif

// File to control camera power
#define CAMERA_POWER_FILE  "camera.power_file"

namespace android {


CameraHardware::CameraHardware(char* devLocation) :
        mReady(false),
        mWin(0),
        mPreviewWinFmt(PIXEL_FORMAT_UNKNOWN),
        mPreviewWinWidth(0),
        mPreviewWinHeight(0),

        mParameters(),

        mRawPreviewHeap(0),
        mRawPreviewFrameSize(0),

        mRawPreviewWidth(0),
        mRawPreviewHeight(0),

        mPreviewHeap(0),
        mPreviewFrameSize(0),
        mPreviewFmt(PIXEL_FORMAT_UNKNOWN),

        mRawPictureHeap(0),
        mRawPictureBufferSize(0),

        mRecordingHeap(0),
        mRecordingFrameSize(0),
        mRecFmt(PIXEL_FORMAT_UNKNOWN),

        mJpegPictureHeap(0),
        mJpegPictureBufferSize(0),

        mRecordingEnabled(0),

        mNotifyCb(0),
        mDataCb(0),
        mDataCbTimestamp(0),
        mRequestMemory(0),
        mCallbackCookie(0),

        mMsgEnabled(0),
        mCurrentPreviewFrame(0),
        mCurrentRecordingFrame(0),
        mCameraPowerFile(0),
        mCameraMetadata(0)
{
    //Store the video device location
    mVideoDevice = devLocation;

    // Initialize camera_device descriptor for this object.

    /* Common header */
    common.tag = HARDWARE_DEVICE_TAG;
    common.version = 0;
    common.module = NULL;       // supplied at connectCamera() time
    common.close = CameraHardware::close;

    /* camera_device fields. */
    ops = &mDeviceOps;
    priv = this;

    // Power on camera
    // PowerOn();

    // Load some initial default parmeters
    // We can skip the lock in the constructor.
    FromCamera fc;
    fc.set(*this);

    mHotPlugThread = new HotPlugThread(this);

    // REVISIT no longer used
    initStaticCameraMetadata();
}



CameraHardware::~CameraHardware()
{
    ALOGD("CameraHardware::destruct");
    if (mPreviewThread != 0) {
        stopPreview();
    }

    if (mHotPlugThread != 0) {
        mHotPlugThread->requestExitAndWait();
        mHotPlugThread.clear();
    }

    // Release all memory heaps
    if (mRawPreviewHeap) {
        mRawPreviewHeap->release(mRawPreviewHeap);
        mRawPreviewHeap = NULL;
    }

    if (mPreviewHeap) {
        mPreviewHeap->release(mPreviewHeap);
        mPreviewHeap = NULL;
    }

    if (mRawPictureHeap) {
        mRawPictureHeap->release(mRawPictureHeap);
        mRawPictureHeap = NULL;
    }

    if (mRecordingHeap) {
        mRecordingHeap->release(mRecordingHeap);
        mRecordingHeap = NULL;
    }

    if (mJpegPictureHeap) {
        mJpegPictureHeap->release(mJpegPictureHeap);
        mJpegPictureHeap = NULL;
    }

    if (mCameraMetadata) {
        free_camera_metadata(mCameraMetadata);
        mCameraMetadata = NULL;
    }

    // Power off camera
    //PowerOff();
}



bool CameraHardware::PowerOn()
{
    ALOGD("PowerOn: Power ON camera.");

    mCameraPowerFile = new char[PROPERTY_VALUE_MAX];

    if (!property_get(CAMERA_POWER_FILE, mCameraPowerFile, "")) {
        ALOGD("PowerOn: no power_file set");
        delete [] mCameraPowerFile;
        mCameraPowerFile = 0;
        return true;
    }

    // power on camera
    int handle = ::open(mCameraPowerFile,O_RDWR);
    if (handle >= 0) {
        ::write(handle,"1\n",2);
        ::close(handle);
    } else {
        ALOGE("Could not open %s for writing.", mCameraPowerFile);
        return false;
    }

    // Wait until the camera is recognized or timed out
    int timeOut = 500;
    do {
        // Try to open the video capture device
        handle = ::open(mVideoDevice.string(),O_RDWR);
        if (handle >= 0)
            break;
        // Wait a bit
        ::usleep(10000);
    } while (--timeOut > 0);

    if (handle >= 0) {
        ALOGD("Camera powered on");
        ::close(handle);
        return true;
    } else {
        ALOGE("Unable to power camera");
    }

    return false;
}



bool CameraHardware::PowerOff()
{
    ALOGD("CameraHardware::PowerOff: Power OFF camera.");

    if (!mCameraPowerFile)
        return true;
    // power on camera
    int handle = ::open(mCameraPowerFile,O_RDWR);
    if (handle >= 0) {
        ::write(handle,"0\n",2);
        ::close(handle);
    } else {
        ALOGE("Could not open %s for writing.", mCameraPowerFile);
        return false;
    }
    delete [] mCameraPowerFile;
    mCameraPowerFile = 0;
    return true;
}



bool CameraHardware::NegotiatePreviewFormat(struct preview_stream_ops* win)
{
    ALOGD("NegotiatePreviewFormat");

    // Get the preview size... If we are recording, use the recording video size instead of the preview size
    int pw, ph;
    if (mRecordingEnabled && mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        mParameters.getVideoSize(&pw, &ph);
    } else {
        mParameters.getPreviewSize(&pw, &ph);
    }

    ALOGD("Trying to set preview window geometry to %dx%d",pw,ph);
    mPreviewWinFmt = PIXEL_FORMAT_UNKNOWN;
    mPreviewWinWidth = 0;
    mPreviewWinHeight = 0;

    // Set the buffer geometry of the surface and YV12 as the preview format
    if (win->set_buffers_geometry(win,pw,ph,PIXEL_FORMAT_RGBA_8888) != NO_ERROR) {
        ALOGE("Unable to set buffer geometry");
        return false;
    }

    // Store the preview window format
    mPreviewWinFmt = PIXEL_FORMAT_RGBA_8888;
    mPreviewWinWidth = pw;
    mPreviewWinHeight = ph;

    return true;
}



CameraHardware::HotPlugThread::HotPlugThread(CameraHardware* hw)
  : mHardware(hw)
{
}



void CameraHardware::HotPlugThread::onFirstRef()
{
    run("CameraHotPlugThread", PRIORITY_BACKGROUND);
}



bool CameraHardware::HotPlugThread::threadLoop()
{
    /*  The thread will be terminated after the camera has been opened.
    */
    auto ok = mHardware->tryOpenCamera(mHardware->mVideoDevice);

    if (!ok) {
        ::usleep(1000 * 1000);
    }

    return !ok;
}



/****************************************************************************
 * Camera API implementation.
 ***************************************************************************/

status_t CameraHardware::connectCamera(const hw_module_t* module, hw_device_t** device)
{
    ALOGD("connectCamera");

    common.module = const_cast<hw_module_t*>(module);
    *device = &common;
    return NO_ERROR;
}



status_t CameraHardware::closeCamera()
{
    ALOGD("closeCamera");
    releaseCamera();
    return NO_ERROR;
}



status_t CameraHardware::getCameraInfo(struct camera_info* info, int facing,
                                       int orientation)
{
    ALOGD("getCameraInfo");
    info->facing = facing;
    info->orientation = orientation;
    info->device_version = CAMERA_DEVICE_API_VERSION_1_0;
    info->static_camera_characteristics = mCameraMetadata;      // REVISIT not used?

    return NO_ERROR;
}



status_t CameraHardware::setPreviewWindow(struct preview_stream_ops* window)
{
    ALOGD("setPreviewWindow: preview_stream_ops: %p", window);
    {
        Mutex::Autolock lock(mLock);

        if (window != NULL) {
            /* The CPU will write each frame to the preview window buffer.
             * Note that we delay setting preview window buffer geometry until
             * frames start to come in. */
            status_t res = window->set_usage(window, GRALLOC_USAGE_SW_WRITE_OFTEN);
            if (res != NO_ERROR) {
                res = -res; // set_usage returns a negative errno.
                ALOGE("setPreviewWindow: Error setting preview window usage %d -> %s", res, strerror(res));
                return res;
            }
        }

        mWin = window;

        // setup the preview window geometry to be able to use the full preview window
        if (mPreviewThread != 0 && mWin != 0) {
            ALOGD("setPreviewWindow - Negotiating preview format");
            NegotiatePreviewFormat(mWin);
        }

    }

    return NO_ERROR;
}



void CameraHardware::setCallbacks(camera_notify_callback notify_cb,
                                  camera_data_callback data_cb,
                                  camera_data_timestamp_callback data_cb_timestamp,
                                  camera_request_memory get_memory,
                                  void* user)
{
    ALOGD("setCallbacks");

    Mutex::Autolock lock(mLock);

    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mRequestMemory = get_memory;
    mCallbackCookie = user;
}



void CameraHardware::enableMsgType(int32_t msgType)
{
    ALOGD("enableMsgType: 0x%x", msgType);

    Mutex::Autolock lock(mLock);

    int32_t old = mMsgEnabled;
    mMsgEnabled |= msgType;

    // If something changed related to the starting or stopping of
    //  the recording process...
    if ((msgType & CAMERA_MSG_VIDEO_FRAME) &&
            (mMsgEnabled ^ old) & CAMERA_MSG_VIDEO_FRAME && mRecordingEnabled) {

        // Recreate the heaps if toggling recording changes the raw preview size
        //  and also restart the preview so we use the new size if needed
        initHeapLocked();
    }
}



void CameraHardware::disableMsgType(int32_t msgType)
{
    ALOGD("disableMsgType: %d", msgType);

    Mutex::Autolock lock(mLock);

    int32_t old = mMsgEnabled;
    mMsgEnabled &= ~msgType;

    // If something changed related to the starting or stopping of
    //  the recording process...
    if ((msgType & CAMERA_MSG_VIDEO_FRAME) &&
            (mMsgEnabled ^ old) & CAMERA_MSG_VIDEO_FRAME && mRecordingEnabled) {

        // Recreate the heaps if toggling recording changes the raw preview size
        //  and also restart the preview so we use the new size if needed
        initHeapLocked();
    }
}



/**
 * Query whether a message, or a set of messages, is enabled.
 * Note that this is operates as an AND, if any of the messages
 * queried are off, this will return false.
 */
int CameraHardware::isMsgTypeEnabled(int32_t msgType)
{
    Mutex::Autolock lock(mLock);

    // All messages queried must be enabled to return true
    int enabled = (mMsgEnabled & msgType) == msgType;

    ALOGD("isMsgTypeEnabled(%d): %d", msgType, enabled);
    return enabled;
}



bool
CameraHardware::isReady()
{
    Mutex::Autolock lock(mLock);
    return mReady;
}



void
CameraHardware::awaitReady()
{
    ALOGD("awaitReady");
    Mutex::Autolock lock(mLock);

    for (;;) {
        if (mReady) {
            return;
        }

        mReadyCond.wait(mLock);
    }
}



bool
CameraHardware::awaitReady(nsecs_t reltime)
{
    ALOGD("awaitReady reltime");
    Mutex::Autolock lock(mLock);

    for (;;) {
        if (mReady) {
            return true;
        }

        // std::condition_variable
        auto status = mReadyCond.waitRelative(mLock, reltime);

        if (status == TIMED_OUT) {
            return false;
        }
    }

    // unreachable
    return false;
}



CameraHardware::PreviewThread::PreviewThread(CameraHardware* hw) :
        Thread(false),
        mHardware(hw)
{
    //ALOGD("PreviewThread: constructed");
}



void CameraHardware::PreviewThread::onFirstRef()
{
    run("CameraPreviewThread", PRIORITY_URGENT_DISPLAY);
}



bool CameraHardware::PreviewThread::threadLoop()
{
    return mHardware->previewThread();
}



status_t CameraHardware::startPreviewLocked()
{
    //ALOGD("startPreviewLocked");

    if (!mReady) {
        ALOGD("startPreviewLocked: camera not ready");
        return NO_INIT;
    }


    if (mPreviewThread != 0) {
        ALOGD("startPreviewLocked: preview already running");
        return NO_ERROR;
    }

    int width, height;

    // If we are recording, use the recording video size instead of the preview size
    if (mRecordingEnabled && mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        mParameters.getVideoSize(&width, &height);
    } else {
        mParameters.getPreviewSize(&width, &height);
    }

    int fps = mParameters.getPreviewFrameRate();

    status_t ret = camera.Open(mVideoDevice);
    if (ret != NO_ERROR) {
        ALOGE("startPreviewLocked: Failed to initialize Camera");
        return ret;
    }

    ret = camera.Init(width, height, fps);
    if (ret != NO_ERROR) {
        ALOGE("startPreviewLocked: Failed to setup streaming");
        return ret;
    }

    /* Retrieve the real size being used */
    camera.getSize(width, height);

    ALOGD("startPreviewLocked: effective size: %dx%d", width, height);

    // If we are recording, use the recording video size instead of the preview size
    if (mRecordingEnabled && mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        /* Store it as the video size to use */
        mParameters.setVideoSize(width, height);
    } else {
        /* Store it as the preview size to use */
        mParameters.setPreviewSize(width, height);
    }

    /* And reinit the memory heaps to reflect the real used size if needed */
    initHeapLocked();

    ALOGD("startPreviewLocked: start streaming");
    ret = camera.StartStreaming();
    if (ret != NO_ERROR) {
        ALOGE("startPreviewLocked: Failed to start streaming");
        return ret;
    }

    // setup the preview window geometry in order to use it to zoom the image
    if (mWin != 0) {
        ALOGD("CameraHardware::setPreviewWindow - Negotiating preview format");
        NegotiatePreviewFormat(mWin);
    }

    ALOGD("startPreviewLocked: starting the preview thread");
    mPreviewThread = new PreviewThread(this);

    ALOGD("startPreviewLocked: done");
    return NO_ERROR;
}



status_t CameraHardware::startPreview()
{
    ALOGD("startPreview");

    Mutex::Autolock lock(mLock);
    return startPreviewLocked();
}



void CameraHardware::stopPreviewLocked()
{
    //ALOGD("stopPreviewLocked");

    if (mPreviewThread != 0) {
        mPreviewThread->requestExitAndWait();
        mPreviewThread.clear();

        camera.Uninit(frameTimeout());
        camera.StopStreaming();
        camera.Close();
    }
}



void CameraHardware::stopPreview()
{
    ALOGD("stopPreview");

    Mutex::Autolock lock(mLock);
    stopPreviewLocked();
}



int CameraHardware::isPreviewEnabled()
{
    int enabled = 0;
    {
        Mutex::Autolock lock(mLock);
        enabled = (mPreviewThread != 0);
    }

    ALOGD("isPreviewEnabled: %d", enabled);
    return enabled;
}



status_t CameraHardware::storeMetaDataInBuffers(int value)
{
    ALOGD("storeMetaDataInBuffers: %d", value);

    // Do not accept to store metadata in buffers - We will always store
    //  YUV data on video buffers. Metadata, in the case of Nvidia Tegra2
    //  is a descriptor of an OpenMax endpoint that was filled with the
    //  data.
    return (value) ? INVALID_OPERATION : NO_ERROR;
}



status_t CameraHardware::startRecording()
{
    ALOGD("startRecording");

    Mutex::Autolock lock(mLock);

    if (!mRecordingEnabled) {
        mRecordingEnabled = true;

        // If something changed related to the starting or stopping of
        //  the recording process...
        if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {

            // Recreate the heaps if toggling recording changes the raw preview size
            //  and also restart the preview so we use the new size if needed
            initHeapLocked();
        }
    }

    return NO_ERROR;
}



void CameraHardware::stopRecording()
{
    ALOGD("stopRecording");

    Mutex::Autolock lock(mLock);

    if (mRecordingEnabled) {
        mRecordingEnabled = false;

        // If something changed related to the starting or stopping of
        //  the recording process...
        if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {

            // Recreate the heaps if toggling recording changes the raw preview size
            //  and also restart the preview so we use the new size if needed
            initHeapLocked();
        }
    }
}



int CameraHardware::isRecordingEnabled()
{
    int enabled;
    {
        Mutex::Autolock lock(mLock);
        enabled = mRecordingEnabled;
    }
    ALOGD("isRecordingEnabled: %d", mRecordingEnabled);
    return enabled;
}



void CameraHardware::releaseRecordingFrame(const void* mem)
{
    UNUSED(mem);
    ALOGD("releaseRecordingFrame");
}



status_t CameraHardware::setAutoFocus()
{
    ALOGD("setAutoFocus");
    Mutex::Autolock lock(mLock);

    if (createThread(beginAutoFocusThread, this) == false)
        return UNKNOWN_ERROR;

    return NO_ERROR;
}



status_t CameraHardware::cancelAutoFocus()
{
    ALOGD("cancelAutoFocus");
    return NO_ERROR;
}



status_t CameraHardware::takePicture()
{
    ALOGD("takePicture");
    Mutex::Autolock lock(mLock);

    if (createThread(beginPictureThread, this) == false)
        return UNKNOWN_ERROR;

    return NO_ERROR;
}



status_t CameraHardware::cancelPicture()
{
    ALOGD("cancelPicture");
    return NO_ERROR;
}



status_t CameraHardware::setParameters(const char* parms)
{
    ALOGD("setParameters");

    Mutex::Autolock lock(mLock);
    return setParametersLocked(parms);
}



status_t CameraHardware::setParametersLocked(const char* parms)
{
    //ALOGD("setParametersLocked");

    CameraParameters params;
    String8 str8_param(parms);
    params.unflatten(str8_param);

    // If no changes, trivially accept it!
    if (params.flatten() == mParameters.flatten()) {
        ALOGD("Trivially accept it. No changes detected");
        return NO_ERROR;
    }

    if (strcmp(params.getPreviewFormat(),"yuv422i-yuyv") &&
            strcmp(params.getPreviewFormat(),"yuv422sp") &&
            strcmp(params.getPreviewFormat(),"yuv420sp") &&
            strcmp(params.getPreviewFormat(),"yuv420p")) {
        ALOGE("setParameters: Unsupported format '%s' for preview",params.getPreviewFormat());
        return BAD_VALUE;
    }

    if (strcmp(params.getPictureFormat(), CameraParameters::PIXEL_FORMAT_JPEG)) {
        ALOGE("setParameters: Only jpeg still pictures are supported");
        return BAD_VALUE;
    }

    if (strcmp(params.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv422i-yuyv") &&
            strcmp(params.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv422sp") &&
            strcmp(params.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv420sp") &&
            strcmp(params.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv420p")) {
        ALOGE("setParameters: Unsupported format '%s' for recording",params.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT));
        return BAD_VALUE;
    }

#if 0
    {
        // For debugging
        int w, h;

        params.getPreviewSize(&w, &h);
        ALOGI("setParameters: PREVIEW: Size %dx%d, %d fps, format: %s", w, h, params.getPreviewFrameRate(), params.getPreviewFormat());

        params.getPictureSize(&w, &h);
        ALOGI("setParameters: PICTURE: Size %dx%d, format: %s", w, h, params.getPictureFormat());

        params.getVideoSize(&w, &h);
        ALOGI("setParameters: VIDEO: Size %dx%d, format: %s", w, h, params.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT));

        w = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
        h = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);
        ALOGI("setParameters: THUMBNAIL: Size %dx%d (2)", w, h);
    }
#endif

    // Store the new parameters
    mParameters = params;

    // Recreate the heaps if toggling recording changes the raw preview size
    // and also restart the preview so we use the new size if needed
    initHeapLocked();

    return NO_ERROR;
}



/* A dumb variable indicating "no params" / error on the exit from
 * EmulatedCamera::getParameters(). */
static char lNoParam = '\0';

char* CameraHardware::getParameters()
{
    ALOGD("getParameters");

    String8 params;
    {
        Mutex::Autolock lock(mLock);
        params = mParameters.flatten();
    }

    if (!params.isEmpty()) {
        char* ret_str = reinterpret_cast<char*>(malloc(sizeof(char) * (params.length()+1)));
        memset(ret_str, 0, params.length()+1);

        if (ret_str != NULL) {
            strncpy(ret_str, params.string(), params.length()+1);
            return ret_str;
        }

        ALOGE("%s: Unable to allocate string for %s", __FUNCTION__, params.string());
    }

    /* Apparently, we can't return NULL fron this routine. */
    return &lNoParam;
}



void CameraHardware::putParameters(char* params)
{
    //ALOGD("CameraHardware::putParameters");
    /* This method simply frees parameters allocated in getParameters(). */
    if (params != NULL && params != &lNoParam) {
        free(params);
    }
}



status_t CameraHardware::sendCommand(int32_t command, int32_t arg1, int32_t arg2)
{
    UNUSED(command);
    UNUSED(arg1);
    UNUSED(arg2);
    ALOGD("sendCommand");
    return 0;
}



void CameraHardware::releaseCamera()
{
    ALOGD("releaseCamera");
    if (mPreviewThread != 0) {
        stopPreview();
    }
}



status_t CameraHardware::dumpCamera(int fd)
{
    UNUSED(fd);
    ALOGD("dump");
    return -EINVAL;
}


// ---------------------------------------------------------------------------


bool CameraHardware::tryOpenCamera(const String8& videoFile)
{
    /*  This will be called from the hotplug thread to try to
        open the video file.  It may take a long time for the camera to
        be enumerated.
        
        This returns true if the parameters are set.
    */
    FromCamera fc;

    ALOGD("tryOpenCamera");

    if (camera.Open(videoFile) != NO_ERROR) {
        ALOGI("did not open %s", videoFile.string());
        return false;
    }

    ALOGI("opened %s", videoFile.string());

    // Get the default preview format
    fc.pw = camera.getBestPreviewFmt().getWidth();
    fc.ph = camera.getBestPreviewFmt().getHeight();
    fc.pfps = camera.getBestPreviewFmt().getFps();

    // Get the default picture format
    fc.fw = camera.getBestPictureFmt().getWidth();
    fc.fh = camera.getBestPictureFmt().getHeight();

    // Get all the available sizes
    fc.avSizes = camera.getAvailableSizes();

    // Get all the available Fps
    fc.avFps = camera.getAvailableFps();

    Mutex::Autolock lock(mLock);

    // Allow the preview thread to start
    mReady = true;

    /*  This will call setParametersLocked() which will
        start the preview thread.
    */
    auto ok = fc.set(*this);

    // Signal that the camera is ready.
    mReadyCond.broadcast();

    //ALOGD("tryOpenCamera exit");
    return ok;
}



CameraHardware::FromCamera::FromCamera()
{
    pw = MIN_WIDTH;
    ph = MIN_HEIGHT;
    pfps = 30;
    fw = MIN_WIDTH;
    fh = MIN_HEIGHT;

    // We need something in lieu of real camera parameters
    avSizes.add(SurfaceSize(640,480)); // VGA
    avFps.add(30);
}


bool CameraHardware::FromCamera::set(CameraHardware& ch)
{
    /*  This calls setParametersLocked(). The caller must have
        the camera locked.
    */
    CameraParameters p;

    // Convert the sizes to text
    String8 szs("");
    for (size_t i = 0; i < avSizes.size(); i++) {
        char descr[32];
        SurfaceSize ss = avSizes[i];
        sprintf(descr,"%dx%d",ss.getWidth(),ss.getHeight());
        szs.append(descr);
        if (i < avSizes.size() - 1) {
            szs.append(",");
        }
    }

    // Convert the fps to ranges in text
    String8 fpsranges("");
    for (size_t i = 0; i < avFps.size(); i++) {
        char descr[32];
        int ss = avFps[i];
        sprintf(descr,"(%d,%d)",ss,ss);
        fpsranges.append(descr);
        if (i < avFps.size() - 1) {
            fpsranges.append(",");
        }
    }

    // Convert the fps to text
    String8 fps("");
    for (size_t i = 0; i < avFps.size(); i++) {
        char descr[32];
        int ss = avFps[i];
        sprintf(descr,"%d",ss);
        fps.append(descr);
        if (i < avFps.size() - 1) {
            fps.append(",");
        }
    }

    ALOGI("Default preview size: (%d x %d), fps:%d\n",pw,ph,pfps);
    ALOGI("All available formats: %s",(const char*)szs);
    ALOGI("All available fps: %s",(const char*)fpsranges);
    ALOGI("Default picture size: (%d x %d)\n",fw,fh);

    // Now store the data

    // Antibanding
    p.set(CameraParameters::KEY_SUPPORTED_ANTIBANDING,"auto");
    p.set(CameraParameters::KEY_ANTIBANDING,"auto");

    // Effects
    p.set(CameraParameters::KEY_SUPPORTED_EFFECTS,"none"); // "none,mono,sepia,negative,solarize"
    p.set(CameraParameters::KEY_EFFECT,"none");

    // Flash modes
    p.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES,"off");
    p.set(CameraParameters::KEY_FLASH_MODE,"off");

    // Focus modes
    p.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,"fixed");
    p.set(CameraParameters::KEY_FOCUS_MODE,"fixed");

    // Picture - Only JPEG supported
    p.set(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS,CameraParameters::PIXEL_FORMAT_JPEG); // ONLY jpeg
    p.setPictureFormat(CameraParameters::PIXEL_FORMAT_JPEG);
    p.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES, szs);
    p.setPictureSize(fw,fh);
    p.set(CameraParameters::KEY_JPEG_QUALITY, 85);

    // Preview - Supporting yuv422i-yuyv,yuv422sp,yuv420sp, defaulting to yuv420sp, as that is the android Defacto default
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,"yuv422i-yuyv,yuv422sp,yuv420sp,yuv420p"); // All supported preview formats
    p.setPreviewFormat(CameraParameters::PIXEL_FORMAT_YUV422SP); // For compatibility sake ... Default to the android standard
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE, fpsranges);
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES, fps);
    p.setPreviewFrameRate( pfps );
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES, szs);
    p.setPreviewSize(pw,ph);

    // Video - Supporting yuv422i-yuyv,yuv422sp,yuv420sp and defaulting to yuv420p
    p.set("video-size-values"/*CameraParameters::KEY_SUPPORTED_VIDEO_SIZES*/, szs);
    p.setVideoSize(pw,ph);
    p.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT, CameraParameters::PIXEL_FORMAT_YUV420P);
    p.set("preferred-preview-size-for-video", "640x480");

    // supported rotations
    p.set("rotation-values","0");
    p.set(CameraParameters::KEY_ROTATION,"0");

    // scenes modes
    p.set(CameraParameters::KEY_SUPPORTED_SCENE_MODES,"auto");
    p.set(CameraParameters::KEY_SCENE_MODE,"auto");

    // white balance
    p.set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE,"auto");
    p.set(CameraParameters::KEY_WHITE_BALANCE,"auto");

    // zoom
    p.set(CameraParameters::KEY_SMOOTH_ZOOM_SUPPORTED,"false");
    p.set("max-video-continuous-zoom", 0 );
    p.set(CameraParameters::KEY_ZOOM, "0");
    p.set(CameraParameters::KEY_MAX_ZOOM, "100");
    p.set(CameraParameters::KEY_ZOOM_RATIOS, "100");
    p.set(CameraParameters::KEY_ZOOM_SUPPORTED, "false");

    // missing parameters for Camera2
    p.set(CameraParameters::KEY_FOCAL_LENGTH, 4.31);
    p.set(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE, 90);
    p.set(CameraParameters::KEY_VERTICAL_VIEW_ANGLE, 90);
    p.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, "6");
    p.set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP, "1.5");

    p.set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES, "640x480");
    p.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,640);
    p.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY,75);
    p.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,480);

    /* Set exposure compensation. */
    p.set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION, "6");
    p.set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION, "-6");
    p.set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP, "0.5");
    p.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, "0");

    if (ch.setParametersLocked(p.flatten()) != NO_ERROR) {
        ALOGE("CameraHardware::FromCamera: Failed to set default parameters.");
        return false;
    }

    return true;
}




void CameraHardware::initStaticCameraMetadata()
{
    Metadata m;

    /* android.control */
    int32_t android_control_ae_available_target_fps_ranges[] = {30, 30};
    m.addInt32(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
            ARRAY_SIZE(android_control_ae_available_target_fps_ranges),
            android_control_ae_available_target_fps_ranges);

    int32_t android_control_ae_compensation_range[] = {-4, 4};
    m.addInt32(ANDROID_CONTROL_AE_COMPENSATION_RANGE,
            ARRAY_SIZE(android_control_ae_compensation_range),
            android_control_ae_compensation_range);

    camera_metadata_rational_t android_control_ae_compensation_step[] = {{2,1}};
    m.addRational(ANDROID_CONTROL_AE_COMPENSATION_STEP,
            ARRAY_SIZE(android_control_ae_compensation_step),
            android_control_ae_compensation_step);

    int32_t android_control_max_regions[] = {/*AE*/ 1,/*AWB*/ 1,/*AF*/ 1};
    m.addInt32(ANDROID_CONTROL_MAX_REGIONS,
            ARRAY_SIZE(android_control_max_regions),
            android_control_max_regions);

    /* android.jpeg */
    int32_t android_jpeg_available_thumbnail_sizes[] = {0, 0, 128, 96};
    m.addInt32(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
            ARRAY_SIZE(android_jpeg_available_thumbnail_sizes),
            android_jpeg_available_thumbnail_sizes);

    int32_t android_jpeg_max_size[] = {13 * 1024 * 1024}; // 13MB
    m.addInt32(ANDROID_JPEG_MAX_SIZE,
            ARRAY_SIZE(android_jpeg_max_size),
            android_jpeg_max_size);

    /* android.lens */
    float android_lens_info_available_focal_lengths[] = {1.0};
    m.addFloat(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
            ARRAY_SIZE(android_lens_info_available_focal_lengths),
            android_lens_info_available_focal_lengths);

    /* android.request */
    int32_t android_request_max_num_output_streams[] = {0, 3, 1};
    m.addInt32(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
            ARRAY_SIZE(android_request_max_num_output_streams),
            android_request_max_num_output_streams);

    /* android.scaler */
    int32_t android_scaler_available_formats[] = {
            HAL_PIXEL_FORMAT_RAW16,
            HAL_PIXEL_FORMAT_BLOB,
            HAL_PIXEL_FORMAT_RGBA_8888,
            HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
            // These are handled by YCbCr_420_888
            //        HAL_PIXEL_FORMAT_YV12,
            //        HAL_PIXEL_FORMAT_YCrCb_420_SP,
            HAL_PIXEL_FORMAT_YCbCr_420_888};
    m.addInt32(ANDROID_SCALER_AVAILABLE_FORMATS,
            ARRAY_SIZE(android_scaler_available_formats),
            android_scaler_available_formats);

    int64_t android_scaler_available_jpeg_min_durations[] = {1};
    m.addInt64(ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS,
            ARRAY_SIZE(android_scaler_available_jpeg_min_durations),
            android_scaler_available_jpeg_min_durations);

    int32_t android_scaler_available_jpeg_sizes[] = {640, 480};
    m.addInt32(ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
            ARRAY_SIZE(android_scaler_available_jpeg_sizes),
            android_scaler_available_jpeg_sizes);

    float android_scaler_available_max_digital_zoom[] = {1};
    m.addFloat(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
            ARRAY_SIZE(android_scaler_available_max_digital_zoom),
            android_scaler_available_max_digital_zoom);

    int64_t android_scaler_available_processed_min_durations[] = {1};
    m.addInt64(ANDROID_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS,
            ARRAY_SIZE(android_scaler_available_processed_min_durations),
            android_scaler_available_processed_min_durations);

    int32_t android_scaler_available_processed_sizes[] = {640, 480};
    m.addInt32(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
            ARRAY_SIZE(android_scaler_available_processed_sizes),
            android_scaler_available_processed_sizes);

    int64_t android_scaler_available_raw_min_durations[] = {1};
    m.addInt64(ANDROID_SCALER_AVAILABLE_RAW_MIN_DURATIONS,
            ARRAY_SIZE(android_scaler_available_raw_min_durations),
            android_scaler_available_raw_min_durations);

    int32_t android_scaler_available_raw_sizes[] = {640, 480};
    m.addInt32(ANDROID_SCALER_AVAILABLE_RAW_SIZES,
            ARRAY_SIZE(android_scaler_available_raw_sizes),
            android_scaler_available_raw_sizes);

    /* android.sensor */

    int32_t android_sensor_info_active_array_size[] = {0, 0, 640, 480};
    m.addInt32(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
            ARRAY_SIZE(android_sensor_info_active_array_size),
            android_sensor_info_active_array_size);

    int32_t android_sensor_info_sensitivity_range[] =
            {100, 1600};
    m.addInt32(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
            ARRAY_SIZE(android_sensor_info_sensitivity_range),
            android_sensor_info_sensitivity_range);

    int64_t android_sensor_info_max_frame_duration[] = {30000000000};
    m.addInt64(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
            ARRAY_SIZE(android_sensor_info_max_frame_duration),
            android_sensor_info_max_frame_duration);

    float android_sensor_info_physical_size[] = {3.2, 2.4};
    m.addFloat(ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
            ARRAY_SIZE(android_sensor_info_physical_size),
            android_sensor_info_physical_size);

    int32_t android_sensor_info_pixel_array_size[] = {640, 480};
    m.addInt32(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
            ARRAY_SIZE(android_sensor_info_pixel_array_size),
            android_sensor_info_pixel_array_size);

    int32_t android_sensor_orientation[] = {0};
    m.addInt32(ANDROID_SENSOR_ORIENTATION,
            ARRAY_SIZE(android_sensor_orientation),
            android_sensor_orientation);

    /* End of static camera characteristics */

    mCameraMetadata = clone_camera_metadata(m.get());
}



void CameraHardware::initHeapLocked()
{
    ALOGD("initHeapLocked");

    int preview_width, preview_height;
    int picture_width, picture_height;
    int video_width, video_height;

    if (!mRequestMemory) {
        ALOGE("No memory allocator available");
        return;
    }

    bool restart_preview = false;

    mParameters.getPreviewSize(&preview_width, &preview_height);
    mParameters.getPictureSize(&picture_width, &picture_height);
    mParameters.getVideoSize(&video_width, &video_height);

    ALOGD("initHeapLocked: preview size %dx%d", preview_width, preview_height);
    ALOGD("initHeapLocked: picture size %dx%d", picture_width, picture_height);
    ALOGD("initHeapLocked: video size %dx%d", video_width, video_height);

    int how_raw_preview_big = 0;

    // If we are recording, use the recording video size instead of the preview size
    if (mRecordingEnabled && mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        how_raw_preview_big = video_width * video_height << 1;      // Raw preview heap always in YUYV

        // If something changed ...
        if (mRawPreviewWidth != video_width || mRawPreviewHeight != video_height) {

            // Stop the preview thread if needed
            if (mPreviewThread != 0) {
                restart_preview = true;
                stopPreviewLocked();
                ALOGD("Stopping preview to allow changes");
            }

            // Store the new effective size
            mRawPreviewWidth = video_width;
            mRawPreviewHeight = video_height;
        }

    } else {
        how_raw_preview_big = preview_width * preview_height << 1;  // Raw preview heap always in YUYV

        // If something changed ...
        if (mRawPreviewWidth != preview_width ||
            mRawPreviewHeight != preview_height) {

            // Stop the preview thread if needed
            if (mPreviewThread != 0) {
                restart_preview = true;
                stopPreviewLocked();
                ALOGD("Stopping preview to allow changes");
            }

            // Store the effective size
            mRawPreviewWidth = preview_width;
            mRawPreviewHeight = preview_height;
        }
    }

    if (how_raw_preview_big != mRawPreviewFrameSize) {

        // Stop the preview thread if needed
        if (!restart_preview && mPreviewThread != 0) {
            restart_preview = true;
            stopPreviewLocked();
            ALOGD("Stopping preview to allow changes");
        }

        mRawPreviewFrameSize = how_raw_preview_big;

        // Create raw picture heap.
        if (mRawPreviewHeap) {
            mRawPreviewHeap->release(mRawPreviewHeap);
            mRawPreviewHeap = NULL;
        }
        mRawPreviewBuffer = NULL;

        mRawPreviewHeap = mRequestMemory(-1,mRawPreviewFrameSize,1,mCallbackCookie);

        if (mRawPreviewHeap) {
            ALOGD("initHeapLocked: Raw preview heap allocated");
            mRawPreviewBuffer = mRawPreviewHeap->data;
        } else {
            ALOGE("Unable to allocate memory for RawPreview");
        }
    }

    int how_preview_big = 0;
    if (!strcmp(mParameters.getPreviewFormat(),"yuv422i-yuyv")) {
        mPreviewFmt = PIXEL_FORMAT_YCrCb_422_I;
        how_preview_big = preview_width * preview_height << 1; // 2 bytes per pixel
    } else if (!strcmp(mParameters.getPreviewFormat(),"yuv422sp")) {
        mPreviewFmt = PIXEL_FORMAT_YCbCr_422_SP;
        how_preview_big = (preview_width * preview_height * 3) >> 1; // 1.5 bytes per pixel
    } else if (!strcmp(mParameters.getPreviewFormat(),"yuv420sp")) {
        mPreviewFmt = PIXEL_FORMAT_YCbCr_420_SP;
        how_preview_big = (preview_width * preview_height * 3) >> 1; // 1.5 bytes per pixel
    } else if (!strcmp(mParameters.getPreviewFormat(),"yuv420p")) {
        mPreviewFmt = PIXEL_FORMAT_YV12;

        /*
         * This format assumes
         * - an even width
         * - an even height
         * - a horizontal stride multiple of 16 pixels
         * - a vertical stride equal to the height
         *
         *   y_size = stride * height
         *   c_size = ALIGN(stride/2, 16) * height/2
         *   cr_offset = y_size
         *   cb_offset = y_size + c_size
         *   size = y_size + c_size * 2
         */

        int stride      = (preview_width + 15) & (-16); // Round to 16 pixels
        int y_size      = stride * preview_height;
        int c_stride    = ((stride >> 1) + 15) & (-16); // Round to 16 pixels
        int c_size      = c_stride * preview_height >> 1;
        int cr_offset   = y_size;
        int cb_offset   = y_size + c_size;
        int size        = y_size + (c_size << 1);

        how_preview_big = size;
    }

    if (how_preview_big != mPreviewFrameSize) {

        // Stop the preview thread if needed
        if (!restart_preview && mPreviewThread != 0) {
            restart_preview = true;
            stopPreviewLocked();
            ALOGD("Stopping preview to allow changes");
        }

        mPreviewFrameSize = how_preview_big;

        // Make a new mmap'ed heap that can be shared across processes.
        // use code below to test with pmem
        if (mPreviewHeap) {
            mPreviewHeap->release(mPreviewHeap);
            mPreviewHeap = NULL;
        }
        memset(mPreviewBuffer,0,sizeof(mPreviewBuffer));

        mPreviewHeap = mRequestMemory(-1,mPreviewFrameSize,kBufferCount,mCallbackCookie);
        if (mPreviewHeap) {
            // Make an IMemory for each frame so that we can reuse them in callbacks.
            for (int i = 0; i < kBufferCount; i++) {
                mPreviewBuffer[i] = (char*)mPreviewHeap->data + (i * mPreviewFrameSize);
            }
            ALOGD("initHeapLocked: preview heap allocated");
        } else {
            ALOGE("Unable to allocate memory for Preview");
        }
    }

    int how_recording_big = 0;
    if (!strcmp(mParameters.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv422i-yuyv")) {
        mRecFmt = PIXEL_FORMAT_YCrCb_422_I;
        how_recording_big = video_width * video_height << 1; // 2 bytes per pixel
    } else if (!strcmp(mParameters.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv422sp")) {
        mRecFmt = PIXEL_FORMAT_YCbCr_422_SP;
        how_recording_big = (video_width * video_height * 3) >> 1; // 1.5 bytes per pixel
    } else if (!strcmp(mParameters.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv420sp")) {
        mRecFmt = PIXEL_FORMAT_YCbCr_420_SP;
        how_recording_big = (video_width * video_height * 3) >> 1; // 1.5 bytes per pixel
    } else if (!strcmp(mParameters.get(CameraParameters::KEY_VIDEO_FRAME_FORMAT),"yuv420p")) {
        mRecFmt = PIXEL_FORMAT_YV12;

        /*
         * This format assumes
         * - an even width
         * - an even height
         * - a horizontal stride multiple of 16 pixels
         * - a vertical stride equal to the height
         *
         *   y_size = stride * height
         *   c_size = ALIGN(stride/2, 16) * height/2
         *   cr_offset = y_size
         *   cb_offset = y_size + c_size
         *   size = y_size + c_size * 2
         */

        int stride      = (video_width + 15) & (-16);   // Round to 16 pixels
        int y_size      = stride * video_height;
        int c_stride    = ((stride >> 1) + 15) & (-16); // Round to 16 pixels
        int c_size      = c_stride * video_height >> 1;
        int cr_offset   = y_size;
        int cb_offset   = y_size + c_size;
        int size        = y_size + (c_size << 1);

        how_recording_big = size;
    }

    if (how_recording_big != mRecordingFrameSize) {

        // Stop the preview thread if needed
        if (!restart_preview && mPreviewThread != 0) {
            restart_preview = true;
            stopPreviewLocked();
            ALOGD("Stopping preview to allow changes");
        }

        mRecordingFrameSize = how_recording_big;

        if (mRecordingHeap) {
            mRecordingHeap->release(mRecordingHeap);
            mRecordingHeap = NULL;
        }
        memset(mRecBuffers,0,sizeof(mRecBuffers));

        mRecordingHeap = mRequestMemory(-1,mRecordingFrameSize,kBufferCount,mCallbackCookie);
        if (mRecordingHeap) {
            // Make an IMemory for each frame so that we can reuse them in callbacks.
            for (int i = 0; i < kBufferCount; i++) {
                mRecBuffers[i] = (char*)mRecordingHeap->data + (i * mRecordingFrameSize);
            }
            ALOGD("initHeapLocked: recording heap allocated");
        } else {
            ALOGE("Unable to allocate memory for Recording");
        }
    }

    int how_picture_big = picture_width * picture_height << 1; // Raw picture heap always in YUYV
    if (how_picture_big != mRawPictureBufferSize) {

        // Picture does not need to stop the preview, as the mutex ensures
        //  the picture memory pool is not being used, and the camera is not
        //  capturing pictures right now

        mRawPictureBufferSize = how_picture_big;

        // Create raw picture heap.
        if (mRawPictureHeap) {
            mRawPictureHeap->release(mRawPictureHeap);
            mRawPictureHeap = NULL;
        }
        mRawBuffer = NULL;

        mRawPictureHeap = mRequestMemory(-1,mRawPictureBufferSize,1,mCallbackCookie);
        if (mRawPictureHeap) {
            mRawBuffer = mRawPictureHeap->data;
            ALOGD("initHeapLocked: Raw picture heap allocated");
        } else {
            ALOGE("Unable to allocate memory for RawPicture");
        }
    }

    int how_jpeg_big = picture_width * picture_height << 1; // jpeg maximum size
    if (how_jpeg_big != mJpegPictureBufferSize) {

        // Picture does not need to stop the preview, as the mutex ensures
        //  the picture memory pool is not being used, and the camera is not
        //  capturing pictures right now

        mJpegPictureBufferSize = how_jpeg_big;

        // Create Jpeg picture heap.
        if (mJpegPictureHeap) {
            mJpegPictureHeap->release(mJpegPictureHeap);
            mJpegPictureHeap = NULL;
        }
        mJpegPictureHeap = mRequestMemory(-1,how_jpeg_big,1,mCallbackCookie);

        if (mJpegPictureHeap) {
            ALOGD("initHeapLocked: Jpeg picture heap allocated");
        } else {
            ALOGE("Unable to allocate memory for JpegPicture");
        }
    }

    // Don't forget to restart the preview if it was stopped...
    if (restart_preview) {
        ALOGD("Restarting preview");
        startPreviewLocked();
    }

    //ALOGD("initHeapLocked: OK");
}



bool CameraHardware::previewThread()
{
    /*  We return true to continue the thread. 

	We don't hold the mutex while doing this. The other threads
	are expected to stop this thread before changing anything.
    */
    //ALOGD("previewThread: this=%p",this);

    // Buffers to send messages
    int previewBufferIdx = 0;

    // If no raw preview buffer, we can't do anything...
    if (mRawPreviewBuffer == 0) {
        ALOGE("No Raw preview buffer!");
        return false;
    }

    // Get the preview buffer for the current frame
    // This is always valid, even if the client died -- the memory
    // is still mapped in our process.
    uint8_t *frame = (uint8_t *)mPreviewBuffer[mCurrentPreviewFrame];

    // If no preview buffer, we cant do anything...
    if (frame == 0) {
        ALOGE("No preview buffer!");
        return false;
    }

    //  Get a pointer to the memory area to use... In case of previewing in YUV422I, we
    // can save a buffer copy by directly using the output buffer. But ONLY if NOT recording
    // or, in case of recording, when size matches
    uint8_t* rawBase = (mPreviewFmt == PIXEL_FORMAT_YCrCb_422_I &&
                        (!mRecordingEnabled || mRawPreviewFrameSize == mPreviewFrameSize))
                        ? frame : (uint8_t*)mRawPreviewBuffer;

    // Grab a frame in the raw format YUYV
    auto status = camera.GrabRawFrame(rawBase, mRawPreviewFrameSize, frameTimeout());

    if (status == TIMED_OUT) {
        return true;
    }

    if (status != NO_ERROR) {
        // Give up
        ALOGE("The camera has failed");
        return false;
    }

    // If the recording is enabled...
    if (mRecordingEnabled && mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        //ALOGD("CameraHardware::previewThread: posting video frame...");

        // Get the video size. We are warrantied here that the current capture
        // size IS exacty equal to the video size, as this condition is enforced
        // by this driver, that priorizes recording size over preview size requirements

        uint8_t *recFrame = (uint8_t *) mRecBuffers[mCurrentRecordingFrame];
        if (recFrame != 0) {

            // Convert from our raw frame to the one the Record requires
            switch (mRecFmt) {

            // Note: Apparently, Android's "YCbCr_422_SP" is merely an arbitrary label
            // The preview data comes in a YUV 4:2:0 format, with Y plane, then VU plane
            case PIXEL_FORMAT_YCbCr_422_SP:
                yuyv_to_yvu420sp(recFrame, mRawPreviewWidth, mRawPreviewHeight, rawBase, (mRawPreviewWidth<<1), mRawPreviewWidth, mRawPreviewHeight);
                break;

            case PIXEL_FORMAT_YCbCr_420_SP:
                yuyv_to_yvu420sp(recFrame, mRawPreviewWidth, mRawPreviewHeight, rawBase, (mRawPreviewWidth<<1), mRawPreviewWidth, mRawPreviewHeight);
                break;

            case PIXEL_FORMAT_YV12:
                /* OMX recorder needs YUV */
                yuyv_to_yuv420p(recFrame, mRawPreviewWidth, mRawPreviewHeight, rawBase, (mRawPreviewWidth<<1), mRawPreviewWidth, mRawPreviewHeight);
                break;

            case PIXEL_FORMAT_YCrCb_422_I:
                memcpy(recFrame, rawBase, mRecordingFrameSize);
                break;
            }

            // Advance the buffer pointer.
            auto recBufferIdx = mCurrentRecordingFrame;
            mCurrentRecordingFrame = (mCurrentRecordingFrame + 1) % kBufferCount;

            // Record callback uses a timestamped frame
            nsecs_t timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
            mDataCbTimestamp(timestamp, CAMERA_MSG_VIDEO_FRAME, mRecordingHeap, recBufferIdx, mCallbackCookie);
        }
    }

    if (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
        //ALOGD("CameraHardware::previewThread: posting preview frame...");

        // Here we could eventually have a problem: If we are recording, the recording size
        //  takes precedence over the preview size. So, the rawBase buffer could be of a
        //  different size than the preview buffer. Handle this situation by centering/cropping
        //  if needed.

        // Get the preview size
        int width = 0, height = 0;
        mParameters.getPreviewSize(&width,&height);

        // Assume we will be able to copy at least those pixels
        int cwidth = width;
        int cheight = height;

        // If we are trying to display a preview larger than the effective capture, truncate to it
        if (cwidth > mRawPreviewWidth)
            cwidth = mRawPreviewWidth;
        if (cheight > mRawPreviewHeight)
            cheight = mRawPreviewHeight;

        // Convert from our raw frame to the one the Preview requires
        switch (mPreviewFmt) {

            // Note: Apparently, Android's "YCbCr_422_SP" is merely an arbitrary label
            // The preview data comes in a YUV 4:2:0 format, with Y plane, then VU plane
        case PIXEL_FORMAT_YCbCr_422_SP: // This is misused by android...
            yuyv_to_yvu420sp(frame, width, height, rawBase, (mRawPreviewWidth<<1), cwidth, cheight);
            break;

        case PIXEL_FORMAT_YCbCr_420_SP:
            yuyv_to_yvu420sp(frame, width, height, rawBase, (mRawPreviewWidth<<1), cwidth, cheight);
            break;

        case PIXEL_FORMAT_YV12:
            yuyv_to_yvu420p(frame, width, height, rawBase, (mRawPreviewWidth<<1), cwidth, cheight);
            break;

        case PIXEL_FORMAT_YCrCb_422_I:
            // Nothing to do here. Is is handled as a special case without buffer copies...
            //  but ONLY in special cases... Otherwise, handle the copy!
            if (mRecordingEnabled && mRawPreviewFrameSize != mPreviewFrameSize) {
                // We need to copy ... do it
                uint8_t* dst = frame;
                uint8_t* src = rawBase;
                int h;
                for (h = 0; h < cheight; h++) {
                    memcpy(dst,src,cwidth<<1);
                    dst += width << 1;
                    src += mRawPreviewWidth<<1;
                }
            }
            break;

        default:
            ALOGE("Unhandled pixel format");

        }

        // Advance the buffer pointer.
        auto previewBufferIdx = mCurrentPreviewFrame;
        mCurrentPreviewFrame = (mCurrentPreviewFrame + 1) % kBufferCount;

        mDataCb(CAMERA_MSG_PREVIEW_FRAME, mPreviewHeap, previewBufferIdx, NULL, mCallbackCookie);
    }

    // Display the preview image
    fillPreviewWindow(rawBase, mRawPreviewWidth, mRawPreviewHeight);

    return true;
}



void CameraHardware::fillPreviewWindow(uint8_t* yuyv, int srcWidth, int srcHeight)
{
    // Preview to a preview window...
    if (mWin == 0) {
        ALOGE("%s: No preview window",__FUNCTION__);
        return;
    }

    // Get a videobuffer
    buffer_handle_t* buf = NULL;
    int stride = 0;
    status_t res = mWin->dequeue_buffer(mWin, &buf, &stride);
    if (res != NO_ERROR || buf == NULL) {
        ALOGE("%s: Unable to dequeue preview window buffer: %d -> %s",
            __FUNCTION__, -res, strerror(-res));
        return;
    }

    /* Let the preview window to lock the buffer. */
    res = mWin->lock_buffer(mWin, buf);
    if (res != NO_ERROR) {
        ALOGE("%s: Unable to lock preview window buffer: %d -> %s",
             __FUNCTION__, -res, strerror(-res));
        mWin->cancel_buffer(mWin, buf);
        return;
    }

    /* Now let the graphics framework to lock the buffer, and provide
     * us with the framebuffer data address. */
    void* vaddr = NULL;

    const Rect bounds(srcWidth, srcHeight);
    GraphicBufferMapper& grbuffer_mapper(GraphicBufferMapper::get());
    res = grbuffer_mapper.lock(*buf, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &vaddr);
    if (res != NO_ERROR || vaddr == NULL) {
        ALOGE("%s: grbuffer_mapper.lock failure: %d -> %s",
             __FUNCTION__, res, strerror(res));
        mWin->cancel_buffer(mWin, buf);
        return;
    }

    // Calculate the source stride...
    int srcStride = srcWidth<<1;
    uint8_t* src  = (uint8_t*)yuyv;

    // Center into the preview surface if needed
    int xStart = (mPreviewWinWidth   - srcWidth ) >> 1;
    int yStart = (mPreviewWinHeight  - srcHeight) >> 1;

    // Make sure not to overflow the preview surface
    if (xStart < 0 || yStart < 0) {
        ALOGE("Preview window is smaller than video preview size - Cropping image.");

        if (xStart < 0) {
            srcWidth += xStart;
            src += ((-xStart) >> 1) << 1;       // Center the crop rectangle
            xStart = 0;
        }

        if (yStart < 0) {
            srcHeight += yStart;
            src += ((-yStart) >> 1) * srcStride; // Center the crop rectangle
            yStart = 0;
        }
    }

    // Calculate the bytes per pixel
    int bytesPerPixel = 2;
    if (mPreviewWinFmt == PIXEL_FORMAT_YCbCr_422_SP ||
        mPreviewWinFmt == PIXEL_FORMAT_YCbCr_420_SP ||
        mPreviewWinFmt == PIXEL_FORMAT_YV12 ||
        mPreviewWinFmt == PIXEL_FORMAT_YV16 ) {
        bytesPerPixel = 1; // Planar Y
    } else if (mPreviewWinFmt == PIXEL_FORMAT_RGB_888) {
        bytesPerPixel = 3;
    } else if (mPreviewWinFmt == PIXEL_FORMAT_RGBA_8888 ||
        mPreviewWinFmt == PIXEL_FORMAT_RGBX_8888 ||
        mPreviewWinFmt == PIXEL_FORMAT_BGRA_8888) {
        bytesPerPixel = 4;
    } else if (mPreviewWinFmt == PIXEL_FORMAT_YCrCb_422_I) {
        bytesPerPixel = 2;
    }

    LOG_FRAME("ANativeWindow: bits:%p, stride in pixels:%d, w:%d, h: %d, format: %d",vaddr,stride,mPreviewWinWidth,mPreviewWinHeight,mPreviewWinFmt);

    // Based on the destination pixel type, we must convert from YUYV to it
    int dstStride = bytesPerPixel * stride;
    uint8_t* dst  = ((uint8_t*)vaddr) + (xStart * bytesPerPixel) + (dstStride * yStart);

    switch (mPreviewWinFmt) {
    case PIXEL_FORMAT_YCbCr_422_SP: // This is misused by android...
        yuyv_to_yvu420sp( dst, dstStride, mPreviewWinHeight, src, srcStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_YCbCr_420_SP:
        yuyv_to_yvu420sp( dst, dstStride, mPreviewWinHeight,src, srcStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_YV12:
        yuyv_to_yvu420p( dst, dstStride, mPreviewWinHeight, src, srcStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_YV16:
        yuyv_to_yvu422p( dst, dstStride, mPreviewWinHeight, src, srcStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_YCrCb_422_I:
    {
        // We need to copy ... do it
        uint8_t* pdst = dst;
        uint8_t* psrc = src;
        int h;
        for (h = 0; h < srcHeight; h++) {
            memcpy(pdst,psrc,srcWidth<<1);
            pdst += dstStride;
            psrc += srcStride;
        }
        break;
    }

    case PIXEL_FORMAT_RGB_888:
        yuyv_to_rgb24(src, srcStride, dst, dstStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_RGBA_8888:
        yuyv_to_rgb32(src, srcStride, dst, dstStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_RGBX_8888:
        yuyv_to_rgb32(src, srcStride, dst, dstStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_BGRA_8888:
        yuyv_to_bgr32(src, srcStride, dst, dstStride, srcWidth, srcHeight);
        break;

    case PIXEL_FORMAT_RGB_565:
        yuyv_to_rgb565(src, srcStride, dst, dstStride, srcWidth, srcHeight);
        break;

    default:
        ALOGE("Unhandled pixel format");
    }

    /* Show it. */
    mWin->enqueue_buffer(mWin, buf);

    // Post the filled buffer!
    grbuffer_mapper.unlock(*buf);
}



nsecs_t CameraHardware::frameTimeout()
{
    // Calculate how long to wait between frames and add 20%.
    int previewFrameRate = mParameters.getPreviewFrameRate();
    return (1200000000 / previewFrameRate);
}



int CameraHardware::beginAutoFocusThread(void *cookie)
{
    ALOGD("beginAutoFocusThread");
    CameraHardware *c = (CameraHardware *)cookie;
    return c->autoFocusThread();
}



int CameraHardware::autoFocusThread()
{
    ALOGD("autoFocusThread");
    if (mMsgEnabled & CAMERA_MSG_FOCUS)
        mNotifyCb(CAMERA_MSG_FOCUS, true, 0, mCallbackCookie);
    return NO_ERROR;
}



int CameraHardware::beginPictureThread(void *cookie)
{
    ALOGD("beginPictureThread");
    CameraHardware *c = (CameraHardware *)cookie;
    return c->pictureThread();
}



int CameraHardware::pictureThread()
{
    ALOGD("pictureThread");

    bool raw = false;
    bool jpeg = false;
    bool shutter = false;
    {
        Mutex::Autolock lock(mLock);

        int w, h;
        mParameters.getPictureSize(&w, &h);
        ALOGD("pictureThread: taking picture of %dx%d", w, h);

        /* Make sure to remember if the shutter must be enabled or not */
        if (mMsgEnabled & CAMERA_MSG_SHUTTER) {
            shutter = true;
        }

        /* The camera application will restart preview ... */
        if (mPreviewThread != 0) {
            stopPreviewLocked();
        }

        ALOGD("pictureThread: taking picture (%d x %d)", w, h);

        if (camera.Open(mVideoDevice) == NO_ERROR) {
            camera.Init(w, h, 1);

            /* Retrieve the real size being used */
            camera.getSize(w,h);

            ALOGD("pictureThread: effective size: %dx%d",w, h);

            /* Store it as the picture size to use */
            mParameters.setPictureSize(w, h);

            /* And reinit the capture heap to reflect the real used size if needed */
            initHeapLocked();

            camera.StartStreaming();

            ALOGD("pictureThread: waiting until camera picture stabilizes...");

            int maxFramesToWait = 8;
            int luminanceStableFor = 0;
            int prevLuminance = 0;
            int prevDif = -1;
            int stride = w << 1;
            int thresh = (w >> 4) * (h >> 4) * 12; // 5% of full range

            while (maxFramesToWait > 0 && luminanceStableFor < 4) {
                uint8_t* ptr = (uint8_t *)mRawBuffer;

                // Get the image
                camera.GrabRawFrame(ptr, (w * h << 1), frameTimeout()); // Always YUYV

                // luminance metering points
                int luminance = 0;
                for (int x = 0; x < (w<<1); x += 32) {
                    for (int y = 0; y < h*stride; y += 16*stride) {
                        luminance += ptr[y + x];
                    }
                }

                // Calculate variation of luminance
                int dif = prevLuminance - luminance;
                if (dif < 0) dif = -dif;
                prevLuminance = luminance;

                // Wait until variation is less than 5%
                if (dif > thresh) {
                    luminanceStableFor = 1;
                } else {
                    luminanceStableFor++;
                }

                maxFramesToWait--;

                ALOGD("luminance: %4d, dif: %4d, thresh: %d, stableFor: %d, maxWait: %d", luminance, dif, thresh, luminanceStableFor, maxFramesToWait);
            }

            ALOGD("pictureThread: picture taken");

            if (mMsgEnabled & CAMERA_MSG_RAW_IMAGE) {

                ALOGD("pictureThread: took raw picture");
                raw = true;
            }

            if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {

                int quality = mParameters.getInt(CameraParameters::KEY_JPEG_QUALITY);

                uint8_t* jpegBuff = (uint8_t*) malloc(mJpegPictureBufferSize);
                if (jpegBuff) {

                    // Compress the raw captured image to our buffer
                    int fileSize = yuyv_to_jpeg((uint8_t *)mRawBuffer, jpegBuff, mJpegPictureBufferSize, w, h, w << 1,quality);

                    // Create a buffer with the exact compressed size
                    if (mJpegPictureHeap) {
                        mJpegPictureHeap->release(mJpegPictureHeap);
                        mJpegPictureHeap = NULL;
                    }

                    mJpegPictureHeap = mRequestMemory(-1,fileSize,1,mCallbackCookie);
                    if (mJpegPictureHeap) {
                        memcpy(mJpegPictureHeap->data,jpegBuff,fileSize);
                        ALOGD("pictureThread: took jpeg picture compressed to %d bytes, q=%d", fileSize, quality);
                        jpeg = true;
                    } else {
                        ALOGE("Unable to allocate memory for RawPicture");
                    }
                    free(jpegBuff);

                } else {

                    ALOGE("Unable to allocate temporary memory for Jpeg compression");
                }

            }

            camera.Uninit(frameTimeout());
            camera.StopStreaming();
            camera.Close();

        } else {
            ALOGE("pictureThread: failed to grab image");
        }
    }

    /* All this callbacks can potentially call one of our methods.
    Make sure to dispatch them OUTSIDE the lock! */
    if (shutter) {
        ALOGD("Sending the Shutter message");
        mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);
    }

    if (raw) {
        ALOGD("Sending the raw message");
        mDataCb(CAMERA_MSG_RAW_IMAGE, mRawPictureHeap, 0, NULL, mCallbackCookie);
    }

    if (jpeg) {
        ALOGD("Sending the jpeg message");
        mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, mJpegPictureHeap, 0, NULL, mCallbackCookie);
    }

    ALOGD("pictureThread OK");

    return NO_ERROR;
}



/****************************************************************************
 * Camera API callbacks as defined by camera_device_ops structure.
 *
 * Callbacks here simply dispatch the calls to an appropriate method inside
 * CameraHardware instance, defined by the 'dev' parameter.
 ***************************************************************************/

int CameraHardware::set_preview_window(struct camera_device* dev,
                                       struct preview_stream_ops* window)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->setPreviewWindow(window);
}



void CameraHardware::set_callbacks(
        struct camera_device* dev,
        camera_notify_callback notify_cb,
        camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory,
        void* user)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->setCallbacks(notify_cb, data_cb, data_cb_timestamp, get_memory, user);
}



void CameraHardware::enable_msg_type(struct camera_device* dev, int32_t msg_type)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->enableMsgType(msg_type);
}



void CameraHardware::disable_msg_type(struct camera_device* dev, int32_t msg_type)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->disableMsgType(msg_type);
}



int CameraHardware::msg_type_enabled(struct camera_device* dev, int32_t msg_type)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->isMsgTypeEnabled(msg_type);
}



int CameraHardware::start_preview(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->startPreview();
}



void CameraHardware::stop_preview(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->stopPreview();
}



int CameraHardware::preview_enabled(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->isPreviewEnabled();
}



int CameraHardware::store_meta_data_in_buffers(struct camera_device* dev,
                                               int enable)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->storeMetaDataInBuffers(enable);
}



int CameraHardware::start_recording(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->startRecording();
}



void CameraHardware::stop_recording(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->stopRecording();
}



int CameraHardware::recording_enabled(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->isRecordingEnabled();
}



void CameraHardware::release_recording_frame(struct camera_device* dev,
                                             const void* opaque)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->releaseRecordingFrame(opaque);
}



int CameraHardware::auto_focus(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->setAutoFocus();
}



int CameraHardware::cancel_auto_focus(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->cancelAutoFocus();
}



int CameraHardware::take_picture(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->takePicture();
}



int CameraHardware::cancel_picture(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->cancelPicture();
}



int CameraHardware::set_parameters(struct camera_device* dev, const char* parms)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->setParameters(parms);
}



char* CameraHardware::get_parameters(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return NULL;
    }
    return ec->getParameters();
}



void CameraHardware::put_parameters(struct camera_device* dev, char* params)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->putParameters(params);
}



int CameraHardware::send_command(struct camera_device* dev,
                                 int32_t cmd,
                                 int32_t arg1,
                                 int32_t arg2)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->sendCommand(cmd, arg1, arg2);
}



void CameraHardware::release(struct camera_device* dev)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return;
    }
    ec->releaseCamera();
}



int CameraHardware::dump(struct camera_device* dev, int fd)
{
    CameraHardware* ec = reinterpret_cast<CameraHardware*>(dev->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->dumpCamera(fd);
}



int CameraHardware::close(struct hw_device_t* device)
{
    CameraHardware* ec =
        reinterpret_cast<CameraHardware*>(reinterpret_cast<struct camera_device*>(device)->priv);
    if (ec == NULL) {
        ALOGE("%s: Unexpected NULL camera device", __FUNCTION__);
        return -EINVAL;
    }
    return ec->closeCamera();
}

/****************************************************************************
 * Static initializer for the camera callback API
 ****************************************************************************/

camera_device_ops_t CameraHardware::mDeviceOps = {
    CameraHardware::set_preview_window,
    CameraHardware::set_callbacks,
    CameraHardware::enable_msg_type,
    CameraHardware::disable_msg_type,
    CameraHardware::msg_type_enabled,
    CameraHardware::start_preview,
    CameraHardware::stop_preview,
    CameraHardware::preview_enabled,
    CameraHardware::store_meta_data_in_buffers,
    CameraHardware::start_recording,
    CameraHardware::stop_recording,
    CameraHardware::recording_enabled,
    CameraHardware::release_recording_frame,
    CameraHardware::auto_focus,
    CameraHardware::cancel_auto_focus,
    CameraHardware::take_picture,
    CameraHardware::cancel_picture,
    CameraHardware::set_parameters,
    CameraHardware::get_parameters,
    CameraHardware::put_parameters,
    CameraHardware::send_command,
    CameraHardware::release,
    CameraHardware::dump
};

}; // namespace android
