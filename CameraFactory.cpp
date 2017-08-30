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

/*  REVISIT either get this path from a property or get the config
    as a string from the property.  The former would be better as property
    values are limited in size.
*/
#define CONFIG_FILE "/etc/camera.cfg"

#include "CameraFactory.h"
#include "Utils.h"

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
        camera device is opened.  We must have the CameraHardware objects in place.
    */
    parseConfig(CONFIG_FILE);
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

    if (mCamera.isEmpty() || camera_id < 0 || camera_id >= getCameraNum()) {
        ALOGE("%s: Camera id %d is out of bounds (%d)", __FUNCTION__, camera_id, getCameraNum());
        return -EINVAL;
    }

    return mCamera[camera_id]->connectCamera(module, device);
}



/* Returns the number of available cameras */
int CameraFactory::getCameraNum()
{
    ALOGD("getCameraNum: %d", mCamera.size());
    return mCamera.size();
}



int CameraFactory::getCameraInfo(int camera_id, struct camera_info* info)
{
    /*  This will be called early by the camera service. The CameraHardware
        objects must already be created.
    */
    ALOGD("getCameraInfo: id = %d, info = %p", camera_id, info);

    if (camera_id < 0 || camera_id >= getCameraNum()) {
        ALOGE("%s: Camera id %d is out of bounds (%d)", __FUNCTION__, camera_id, getCameraNum());
        return -EINVAL;
    }

    return mCamera[camera_id]->getCameraInfo(info);
}



/*  Parse a simple configuration file
    Each line describes one CameraHardware object. There are shell-like
    options on the line:
        camera options...
    where the options are:
        -device FILE        : a device file to open. There can be more than one of these.
        -res 1920x1080      : the default resolution to use
        -role [front|back|other]  : defaults to other for the USB camera
*/
int CameraFactory::parseConfig(const char* configFile)
{
    ALOGD("parseConfig: configFile = %s", configFile);

    auto text = utils::readFile(String8(configFile));

    if (text.isEmpty()) {
        ALOGE("Cannot read the configuration file ");
        return UNKNOWN_ERROR;
    }

    for (auto& line : utils::splitLines(text)) {
ALOGD("parseConfig: line = %s", line.string());
        auto words = utils::splitWords(line);
ALOGD("parseConfig: #words = %d", words.size());

        if (words.isEmpty()) {
            continue;
        }

        size_t ix = 0;
        auto cmd = words[ix];
ALOGD("parseConfig: cmd = %s", cmd.string());

        if (cmd[0] == '#') {
            // A comment line
            continue;
        }

        if (cmd == "camera") {
            CameraSpec spec;

            spec.facing = CAMERA_FACING_EXTERNAL;
            spec.orientation = 0;

            while ((ix + 1) < words.size()) {
                auto& opt = words[++ix];
ALOGD("parseConfig: opt = %s, ix = %d", opt.string(), ix);

                if (opt == "-device" && (ix + 1) < words.size()) {
                    auto& dev = words[++ix];
ALOGD("parseConfig: dev = %s", dev.string());

                    if (!dev.isEmpty()) {
                        spec.devices.push_back(dev);
                    }
                } else if (opt == "-res" && (ix + 1) < words.size()) {
                    auto& res = words[++ix];
                    int w, h;

                    if (sscanf(res.string(), "%dx%d", &w, &h) == 2) {
                        spec.defaultSize = SurfaceSize(w, h);
                    }
                } else if (opt == "-role" && (ix + 1) < words.size()) {
                    auto& role = words[++ix];

                    if (role == "front") {
                        spec.facing = CAMERA_FACING_FRONT;
                    } else if (role == "back") {
                        spec.facing = CAMERA_FACING_BACK;
                    }
                }
            }

            // Create the camera entry
            mCamera.push_back(mkRef<CameraHardware>(spec));

        } else {
            ALOGD("Unrecognized config line '%s'", line.string());
        }
    }

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
