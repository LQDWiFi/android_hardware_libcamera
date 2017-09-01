/*
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 */

#ifndef _V4L2CAMERA_H
#define _V4L2CAMERA_H

#define NB_BUFFER 4

#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <hardware/camera.h>
#include <utils/SortedVector.h>
#include <utils/Timers.h>               // for nsecs_t

#include "Utils.h"
#include "uvc_compat.h"
#include "SurfaceDesc.h"

namespace android {
//======================================================================

static const int MaxPreviewWidth = 1920;    // a hack for certain cameras


struct CameraSpec
{
    StringVec       devices;            // devices to force
    StringVec       nodevices;          // devices to skip
    SurfaceSize     preferredSize;
    int             facing = CAMERA_FACING_EXTERNAL;
    int             orientation = 0;    // 0, 90, 180, 270
};


struct vdIn {
    struct v4l2_capability cap;
    struct v4l2_format format;              // Capture format being used
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers rb;
    struct v4l2_streamparm params;          // v4l2 stream parameters struct
    struct v4l2_jpegcompression jpegcomp;   // v4l2 jpeg compression settings

    void *mem[NB_BUFFER];
    bool isStreaming;

    void* tmpBuffer;

    int outWidth;                           // Requested Output width
    int outHeight;                          // Requested Output height
    int outFrameSize;                       // The expected output framesize (in YUYV)
    int capBytesPerPixel;                   // Capture bytes per pixel
    int capCropOffset;                      // The offset in bytes to add to the captured buffer to get to the first pixel

};


//======================================================================

class V4L2Camera {

public:
    V4L2Camera();
    ~V4L2Camera();

    int  Open(const CameraSpec& spec);
    void Close();

    int  Init(int width, int height, int fps);
    void Uninit();

    int StartStreaming ();
    int StopStreaming ();

    /*  @return NO_ERROR  - a frame has been copied
                TIMED_OUT - no data is available
                UNKNOWN_ERROR - some I/O error
    */
    status_t GrabRawFrame (void *frameBuffer, int maxSize, nsecs_t timeout);

    void getSize(int& width, int& height) const;
    int  getFps() const;

    SortedVector<SurfaceSize> getAvailableSizes() const;
    SortedVector<int> getAvailableFps() const;
    const SurfaceDesc& getBestPreviewFmt() const;
    const SurfaceDesc& getBestPictureFmt() const;

private:
    bool tryDevices(const CameraSpec& spec);
    bool tryOneDevice(const std::string& device);
    bool EnumFrameIntervals(int pixfmt, int width, int height);
    bool EnumFrameSizes(int pixfmt);
    bool EnumFrameFormats(const SurfaceSize& preferred);
    status_t dequeueBuf(nsecs_t timeout);
    status_t enqueueBuf();

    int saveYUYVtoJPEG(uint8_t* src, uint8_t* dst, int maxsize, int width, int height, int quality);

private:
    std::string  lastDevice;
    bool         haveEnumerated;
    struct vdIn* videoIn;
    int          vfd;

    SortedVector<SurfaceDesc> m_AllFmts;        // Available video modes
    SurfaceDesc m_BestPreviewFmt;               // Best preview mode. maximum fps with biggest frame
    SurfaceDesc m_BestPictureFmt;               // Best picture format. maximum size

};

//======================================================================
}; // namespace android

#endif
