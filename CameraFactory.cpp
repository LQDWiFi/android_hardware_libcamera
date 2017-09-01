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

#define LOG_NDEBUG 0
#define LOG_TAG "CameraFactory"

// REVISIT
#define FORCE_PEBBLE

#include "CameraFactory.h"

#include <cutils/log.h>
#include <cutils/properties.h>

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

        However if the property is omitted then we pretend to have no cameras.
    */

    char configFile[PROPERTY_VALUE_MAX];

    if (property_get("camera.config_file", configFile, "")) {
        parseConfig(configFile);
    }
#ifdef FORCE_PEBBLE
    else parseConfig("/etc/camera.cfg");
#endif
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



/*  Parse a simple configuration file

    The camera device nodes that will be scanned include all of the /dev/video* devices
    and those mentioned in device lines but excluding those in nodevice lines.

    nodevice PATH
    device PATH
    resolution 1920x1080      : the default resolution to use
    role [front|back|other]   : defaults to other for the USB camera
*/
int CameraFactory::parseConfig(const char* configFile)
{
    ALOGD("parseConfig: configFile = %s", configFile);

    CameraSpec spec;

    auto text = utils::readFile(configFile);

    if (text.empty()) {
        ALOGE("Cannot read the configuration file ");
        return UNKNOWN_ERROR;
    }

    for (auto& line : utils::splitLines(text)) {
        auto words = utils::splitWords(line);

        if (words.empty()) {
            continue;
        }

        auto cmd = words[0];

        if (cmd[0] == '#') {
            // A comment line
            continue;
        }

        if (cmd == "device" && words.size() == 2) {
            auto& dev = words[1];
            ALOGD("parseConfig: device = %s", dev.c_str());
            spec.devices.push_back(dev);
        } else if (cmd == "nodevice" && words.size() == 2) {
            auto& dev = words[1];
            ALOGD("parseConfig: nodevice = %s", dev.c_str());
            spec.nodevices.push_back(dev);
        } else if (cmd == "resolution" && words.size() == 2) {
            auto& res = words[1];
            int w, h;

            ALOGD("parseConfig: resolution = %s", res.c_str());

            if (sscanf(res.c_str(), "%dx%d", &w, &h) == 2) {
                spec.preferredSize = SurfaceSize(w, h);
            }
        } else if (cmd == "role" && words.size() == 2) {
            auto& role = words[1];

            if (role == "front") {
                spec.facing = CAMERA_FACING_FRONT;
            } else if (role == "back") {
                spec.facing = CAMERA_FACING_BACK;
            }
        } else {
            ALOGD("Unrecognized config line '%s'", line.c_str());
        }
    }

    // Create the camera entry
    mCamera.push_back(mkRef<CameraHardware>(spec));

    return NO_ERROR;
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
