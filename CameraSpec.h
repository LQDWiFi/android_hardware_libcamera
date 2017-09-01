
#ifndef _CAMERA_SPEC_H
#define _CAMERA_SPEC_H

#include "Utils.h"
#include "SurfaceDesc.h"
#include <hardware/camera.h>

namespace android {
//======================================================================

/*  These are details for the search for video devices.  They
    come from a configuration file.
*/

class CameraSpec
{
public:
    StringVec       devices;            // devices to force
    StringVec       nodevices;          // devices to skip
    SurfaceSize     preferredSize;
    int             facing = CAMERA_FACING_EXTERNAL;
    int             orientation = 0;    // 0, 90, 180, 270

    int loadFromFile(const char* configFile);
};

//======================================================================
}; // namespace android
#endif // _CAMERA_SPEC_H
