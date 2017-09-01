#include "CameraSpec.h"

#define LOG_TAG "CameraSpec"

#include <utils/Errors.h>
#include <cutils/log.h>
#include <cutils/properties.h>

namespace android {
//======================================================================


/*  Parse a simple configuration file

    The camera device nodes that will be scanned include all of the /dev/video* devices
    and those mentioned in device lines but excluding those in nodevice lines.

    nodevice PATH
    device PATH
    resolution 1920x1080      : the default resolution to use
    role [front|back|other]   : defaults to other for the USB camera
*/
int CameraSpec::loadFromFile(const char* configFile)
{
    ALOGD("loadFromFile: configFile = %s", configFile);

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
            ALOGD("loadFromFile: device = %s", dev.c_str());
            spec.devices.push_back(dev);
        } else if (cmd == "nodevice" && words.size() == 2) {
            auto& dev = words[1];
            ALOGD("loadFromFile: nodevice = %s", dev.c_str());
            spec.nodevices.push_back(dev);
        } else if (cmd == "resolution" && words.size() == 2) {
            auto& res = words[1];
            int w, h;

            ALOGD("loadFromFile: resolution = %s", res.c_str());

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

    return NO_ERROR;
}

//======================================================================
}; /* namespace android */
