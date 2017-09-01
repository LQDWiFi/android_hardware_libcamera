/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Contains implementation of a class CameraFactory that manages cameras
 * available
 */

#define LOG_TAG "CameraFactory"

#define CONFIG_FILE "/etc/camera.cfg"

#include "CameraFactory.h"
#include <cutils/log.h>

extern camera_module_t HAL_MODULE_INFO_SYM;


namespace android {
//======================================================================

ANDROID_SINGLETON_STATIC_INSTANCE(CameraFactory);


CameraFactory::CameraFactory()
  : Singleton<CameraFactory>()
{
    ALOGD("CameraFactory");

    /*  The camera service will be calling getCameraInfo early, even before the
        camera device is opened.  It doesn't cope with the suite of cameras changing
        after it starts.  We must pretend to already have the CameraHardware object.

        However if the configuration file cannot be read then we pretend to have no cameras.
    */
    CameraSpec spec;

    if (spec.loadFromFile(CONFIG_FILE) == NO_ERROR) {
        mCamera.push_back(mkRef<CameraHardware>(spec));
    }
}



CameraFactory::~CameraFactory()
{
    ALOGD("~CameraFactory");
}

/****************************************************************************
 * Camera HAL API handlers.
 *
 * Each handler simply verifies existence of an appropriate Camera
 * instance, and dispatches the call to that instance.
 *
 ***************************************************************************/

int CameraFactory::cameraDeviceOpen(const hw_module_t* module,int camera_id, hw_device_t** device)
{
    ALOGD("cameraDeviceOpen: id = %d", camera_id);

    *device = NULL;

    if (mCamera.empty() || camera_id < 0 || camera_id >= getCameraNum()) {
        ALOGE("%s: Camera id %d is out of bounds (%d)", __FUNCTION__, camera_id, getCameraNum());
        return -EINVAL;
    }

    return mCamera[camera_id]->connectCamera(module, device);
}



/* Returns the number of available cameras */
int CameraFactory::getCameraNum()
{
    ALOGD("getCameraNum: %lu", mCamera.size());
    return mCamera.size();
}



int CameraFactory::getCameraInfo(int camera_id, struct camera_info* info)
{
    /*  This will be called early by the camera service. The CameraHardware
        objects must already be created.
    */
    ALOGD("getCameraInfo: id = %d", camera_id);

    if (camera_id < 0 || camera_id >= getCameraNum()) {
        ALOGE("%s: Camera id %d is out of bounds (%d)", __FUNCTION__, camera_id, getCameraNum());
        return -EINVAL;
    }

    return mCamera[camera_id]->getCameraInfo(info);
}


/****************************************************************************
 * Camera HAL API callbacks.
 ***************************************************************************/

int CameraFactory::device_open(
    const hw_module_t*  module,
    const char*         name,
    hw_device_t**       device
    )
{
    ALOGD("device_open: name = %s", name);

    /*
     * Simply verify the parameters, and dispatch the call inside the
     * CameraFactory instance.
     */

    if (module != &HAL_MODULE_INFO_SYM.common) {
        ALOGE("%s: Invalid module %p expected %p", __FUNCTION__, module, &HAL_MODULE_INFO_SYM.common);
        return -EINVAL;
    }

    if (name == NULL) {
        ALOGE("%s: NULL name is not expected here", __FUNCTION__);
        return -EINVAL;
    }

    int camera_id = atoi(name);
    return getInstance().cameraDeviceOpen(module, camera_id, device);
}



int CameraFactory::get_number_of_cameras(void)
{
    ALOGD("get_number_of_cameras");
    return getInstance().getCameraNum();
}



int CameraFactory::get_camera_info(int camera_id, struct camera_info* info)
{
    ALOGD("get_camera_info");
    return getInstance().getCameraInfo(camera_id, info);
}

/********************************************************************************
 * Initializer for the static member structure.
 *******************************************************************************/

/* Entry point for camera HAL API. */
struct hw_module_methods_t CameraFactory::mCameraModuleMethods = {
    open: CameraFactory::device_open
};

//======================================================================
}; /* namespace android */
