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

#ifndef HW_CAMERA_CAMERA_FACTORY_H
#define HW_CAMERA_CAMERA_FACTORY_H

#include <hardware/hardware.h>
#include <utils/Singleton.h>

#include "Utils.h"
#include "CameraHardware.h"

namespace android {
//======================================================================

/*  This simplified HAL only supports one camera using the uvcvideo driver.

    Instance of this class is also used as the entry point for the camera HAL API,
    including:
     - hw_module_methods_t::open entry point
     - camera_module_t::get_number_of_cameras entry point
     - camera_module_t::get_camera_info entry point
 
*/

class CameraFactory: public Singleton<CameraFactory> {
public:
    /* Constructs CameraFactory instance.
     * In this constructor the factory will create and initialize a list of
     * emulated cameras. All errors that occur on this constructor are reported
     * via mConstructedOK data member of this class.
     */
    CameraFactory();

    /* Destructs CameraFactory instance. */
    ~CameraFactory();

    /****************************************************************************
     * Camera HAL API handlers.
     ***************************************************************************/

    /* Opens (connects to) a camera device.
     * This method is called in response to hw_module_methods_t::open callback.
     */
    int cameraDeviceOpen(const hw_module_t* module,int camera_id, hw_device_t** device);

    /* Gets emulated camera information.
     * This method is called in response to camera_module_t::get_camera_info callback.
     */
    int getCameraInfo(int camera_id, struct camera_info *info);

    /* Returns the number of available cameras */
    int getCameraNum();

    /****************************************************************************
     * Camera HAL API callbacks.
     ***************************************************************************/

    /* camera_module_t::get_number_of_cameras callback entry point. */
    static int get_number_of_cameras(void);

    /* camera_module_t::get_camera_info callback entry point. */
    static int get_camera_info(int camera_id, struct camera_info *info);

    /* Contains device open entry point, as required by HAL API. */
    static struct hw_module_methods_t   mCameraModuleMethods;

private:
    friend class Singleton<CameraFactory>;

    /* hw_module_methods_t::open callback entry point. */
    static int device_open(const hw_module_t* module,
                           const char* name,
                           hw_device_t** device);

    /* Camera hardware */
    std::vector<Ref<CameraHardware>> mCamera;
};

//======================================================================
}; /* namespace android */

#endif  /* HW_EMULATOR_CAMERA_EMULATED_CAMERA_FACTORY_H */
