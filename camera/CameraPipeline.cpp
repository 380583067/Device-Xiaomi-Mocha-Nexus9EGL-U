#include "CameraPipeline.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <system/graphics.h>
#include <cutils/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <cutils/properties.h>


#ifndef LOG_TAG
#define LOG_TAG "MochaCameraHAL"
#endif

namespace mocha {

CameraPipeline::CameraPipeline()
    : mFd(-1),
      mState(PIPELINE_CLOSED),
      mRgbBuffer(nullptr),
      mRgbBufferSize(0),
      mRgbaFull(nullptr),
      mRgbaFullSize(0),
      mCropX(0), mCropY(0), mCropW(0), mCropH(0),
      mStreaming(false),
      mBufferCount(0),
      mCurrentBuffer(0),
    mCurrentExposure(800),
    mCurrentGain(60),
      mHasAwbInit(false),
      mLastGamma(0.0f),
      mFocusPosition(0),
      mAfState(0),
      mAfRegX(0), mAfRegY(0), mAfRegW(0), mAfRegH(0) {
    for (int i = 0; i < 4; i++) {
        mBuffers[i].start = nullptr;
        mBuffers[i].length = 0;
        mBuffers[i].allocated = false;
    }
    mAwbGains[0] = 1.0f;
    mAwbGains[1] = 1.0f;
    mAwbGains[2] = 1.0f;
    mAwbGains[3] = 1.0f;
}

CameraPipeline::~CameraPipeline() {
    close();
}

int CameraPipeline::open(int cameraId) {
    ALOGI("CameraPipeline::open cameraId=%d", cameraId);

    if (mState != PIPELINE_CLOSED) {
        ALOGE("Pipeline already open");
        return -EBUSY;
    }

    mCameraId = cameraId;
    const char* devPath = (cameraId == 0) ? "/dev/video0" : "/dev/video1";

    mFd = ::open(devPath, O_RDWR | O_NONBLOCK);
    if (mFd < 0) {
        ALOGE("Failed to open V4L2 device: %s (error %d: %s)", devPath, errno, strerror(errno));
        return -ENODEV;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(mFd, VIDIOC_QUERYCAP, &cap) < 0) {
        ALOGE("VIDIOC_QUERYCAP failed: %s", strerror(errno));
        ::close(mFd);
        mFd = -1;
        return -ENODEV;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        ALOGE("Device does not support video capture or streaming");
        ::close(mFd);
        mFd = -1;
        return -ENODEV;
    }

    if (cameraId == 0) {
        initFocuser();
    }

    mState = PIPELINE_OPENED;
    return 0;
}

int CameraPipeline::close() {
    stopStreaming();

    /* [2026-11-26 async AF] A sweep may still be running on its worker thread and
     * about to touch mBuffers / mFd that close() is tearing down. Ask it to stop
     * and wait before freeing anything it can reach.
     * [2026-11-26 crash fix] Always gate on joinable(), never on the active
     * flag: the thread clears the flag slightly before it is actually reaped,
     * and assigning a new std::thread to a still-joinable object calls
     * std::terminate() (measured: second tap-to-focus crashed cameraserver). */
    if (mAfThread.joinable()) {
        mAfAbort = true;
        mAfThread.join();
    }
    mAfThreadActive = false;

    for (int i = 0; i < mBufferCount; i++) {
        if (mBuffers[i].start && mBuffers[i].allocated) {
            munmap(mBuffers[i].start, mBuffers[i].length);
            mBuffers[i].start = nullptr;
            mBuffers[i].allocated = false;
        }
    }
    mBufferCount = 0;

    if (mRgbBuffer) {
        delete[] mRgbBuffer;
        mRgbBuffer = nullptr;
        mRgbBufferSize = 0;
    }
    if (mRgbaFull) {
        delete[] mRgbaFull;
        mRgbaFull = nullptr;
        mRgbaFullSize = 0;
    }

    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }

    deinitFocuser();

    mState = PIPELINE_CLOSED;
    return 0;
}

int CameraPipeline::setExposure(int exposure) {
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_EXPOSURE;
    ctrl.value = exposure;
    /*
     * [exposure I2C failure fix 2026-11-16] mocha's camera I2C bus (tegra12-i2c.2)
     * occasionally times out writing to IMX179 (dmesg: -110/-121, writing 0x0202
     * fails and the chip ID read right after succeeds), so a single VIDIOC_S_CTRL
     * has a high failure rate. The kernel tegra-i2c driver only retries twice, so
     * retry three more times here (10 ms apart); otherwise the AE loop reports
     * errno=5 (EIO) every frame, exposure sticks at 800 and the picture is too dark.
     */
    int ret = -1;
    for (int retry = 0; retry < 3; retry++) {
        ret = ioctl(mFd, VIDIOC_S_CTRL, &ctrl);
        if (ret == 0) {
            mCurrentExposure = exposure;
            break;
        }
        ALOGE("setExposure(%d) attempt %d failed: ret=%d errno=%d (%s)",
              exposure, retry + 1, ret, errno, strerror(errno));
        usleep(10000);
    }
    return ret;
}

int CameraPipeline::setGain(int gain) {
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_GAIN;
    ctrl.value = gain;
    int ret = ioctl(mFd, VIDIOC_S_CTRL, &ctrl);
    if (ret == 0) {
        mCurrentGain = gain;
    } else {
        ALOGE("setGain(%d) failed: ret=%d errno=%d (%s)",
              gain, ret, errno, strerror(errno));
    }
    return ret;
}

int CameraPipeline::getExposure() {
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_EXPOSURE;
    if (ioctl(mFd, VIDIOC_G_CTRL, &ctrl) == 0) {
        mCurrentExposure = ctrl.value;
        return ctrl.value;
    }
    return mCurrentExposure;
}

int CameraPipeline::getGain() {
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_GAIN;
    if (ioctl(mFd, VIDIOC_G_CTRL, &ctrl) == 0) {
        mCurrentGain = ctrl.value;
        return ctrl.value;
    }
    return mCurrentGain;
}

int CameraPipeline::configure(const PipelineConfig& config) {
    if (mState == PIPELINE_STREAMING) {
        return -EBUSY;
    }

    mConfig = config;
    mDigitalGain = config.digitalGain;

    if (mFd < 0) {
        return -ENODEV;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config.width;
    fmt.fmt.pix.height = config.height;
    fmt.fmt.pix.pixelformat = config.pixelFormat;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    ALOGI("VIDIOC_S_FMT: requesting %dx%d fmt=0x%x", config.width, config.height, config.pixelFormat);

    if (ioctl(mFd, VIDIOC_S_FMT, &fmt) < 0) {
        ALOGE("VIDIOC_S_FMT failed: %s", strerror(errno));
        return -errno;
    }

    ALOGI("VIDIOC_S_FMT: got %dx%d fmt=0x%x sizeimage=%d bytesperline=%d",
          fmt.fmt.pix.width, fmt.fmt.pix.height,
          fmt.fmt.pix.pixelformat, fmt.fmt.pix.sizeimage,
          fmt.fmt.pix.bytesperline);


    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(mFd, VIDIOC_REQBUFS, &req) < 0) {
        ALOGE("VIDIOC_REQBUFS failed: %s", strerror(errno));
        return -errno;
    }

    if (req.count < 2) {
        return -ENOMEM;
    }

    mBufferCount = req.count;

    uint32_t bufSize = fmt.fmt.pix.sizeimage;
    for (int i = 0; i < mBufferCount; i++) {
        struct v4l2_buffer qbuf;
        memset(&qbuf, 0, sizeof(qbuf));
        qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qbuf.memory = V4L2_MEMORY_MMAP;
        qbuf.index = i;
        if (ioctl(mFd, VIDIOC_QUERYBUF, &qbuf) < 0) {
            for (int j = 0; j < i; j++) {
                if (mBuffers[j].start) munmap(mBuffers[j].start, mBuffers[j].length);
                mBuffers[j].start = nullptr;
                mBuffers[j].allocated = false;
            }
            mBufferCount = 0;
            return -errno;
        }
        mBuffers[i].length = qbuf.length;
        mBuffers[i].start = mmap(NULL, qbuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, qbuf.m.offset);
        if (mBuffers[i].start == MAP_FAILED) {
            mBuffers[i].start = nullptr;
            for (int j = 0; j < i; j++) {
                if (mBuffers[j].start) munmap(mBuffers[j].start, mBuffers[j].length);
                mBuffers[j].start = nullptr;
                mBuffers[j].allocated = false;
            }
            mBufferCount = 0;
            return -ENOMEM;
        }
        mBuffers[i].allocated = true;
    }

    if (config.enableISP) {
        DemosaicParams demosaicParams;
        demosaicParams.width = config.width;
        demosaicParams.height = config.height;
        demosaicParams.bayerPattern = config.bayerPattern;
        demosaicParams.offset_x = config.offset_x;
        demosaicParams.offset_y = config.offset_y;
        demosaicParams.blackLevel = config.blackLevel;

        mDemosaic = std::unique_ptr<DemosaicNEON>(new DemosaicNEON());
        int ret = mDemosaic->initialize(demosaicParams);
        if (ret != 0) return ret;

        mColorConv = std::unique_ptr<ColorConvNEON>(new ColorConvNEON());
        ret = mColorConv->initialize(config.width, config.height);
        if (ret != 0) return ret;

        if (mRgbBuffer) {
            delete[] mRgbBuffer;
            mRgbBuffer = nullptr;
        }
        mRgbBufferSize = config.width * config.height * 3;
        mRgbBuffer = new uint8_t[mRgbBufferSize];
        mRgbaFullSize = config.width * config.height * 4;
        mRgbaFull = new uint8_t[mRgbaFullSize];
        /* full frame by default */
        mCropX = 0; mCropY = 0;
        mCropW = config.width; mCropH = config.height;
    }

    mHasAwbInit = false;

    /* Rebuild gamma LUT if gamma changed */
    if (mLastGamma != config.gamma) {
        for (int i = 0; i < 256; i++)
            mGammaLut[i] = (uint8_t)(powf(i / 255.0f, config.gamma) * 255.0f + 0.5f);
        mLastGamma = config.gamma;
    }

    return 0;
}

int CameraPipeline::startStreaming() {
    if (mState != PIPELINE_OPENED) return -EINVAL;

    /* [2026-11-22 recording 0 frames] stopStreaming() munmaps every buffer after
     * STREAMOFF and zeroes mBufferCount, but this function used to QBUF only
     * mBufferCount buffers - which is 0 after a flush (starting a recording always
     * triggers camera_device_flush -> STREAMOFF -> stream restart). STREAMON then
     * left the VI buf_queue empty (kernel log: "0 buffers ... buf_queue=EMPTY") and
     * every later DQBUF came back empty, so the video track stayed at 0 frames for
     * the whole recording. If no buffers are queued, the configure() REQBUFS + mmap
     * sequence is repeated now. */
    if (mBufferCount == 0) {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = 2;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(mFd, VIDIOC_REQBUFS, &req) < 0) {
            ALOGE("VIDIOC_REQBUFS failed (restart): %s", strerror(errno));
            return -errno;
        }
        if (req.count < 2) {
            ALOGE("VIDIOC_REQBUFS gave only %d buffers (restart)", req.count);
            return -ENOMEM;
        }
        mBufferCount = req.count;

        for (int i = 0; i < mBufferCount; i++) {
            struct v4l2_buffer qbuf;
            memset(&qbuf, 0, sizeof(qbuf));
            qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            qbuf.memory = V4L2_MEMORY_MMAP;
            qbuf.index = i;

            if (ioctl(mFd, VIDIOC_QUERYBUF, &qbuf) < 0) {
                ALOGE("VIDIOC_QUERYBUF failed (restart): %s", strerror(errno));
                mBufferCount = 0;
                return -errno;
            }
            mBuffers[i].length = qbuf.length;
            mBuffers[i].start = mmap(NULL, qbuf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, mFd, qbuf.m.offset);
            if (mBuffers[i].start == MAP_FAILED) {
                mBuffers[i].start = nullptr;
                for (int j = 0; j < i; j++) {
                    if (mBuffers[j].start) munmap(mBuffers[j].start, mBuffers[j].length);
                    mBuffers[j].start = nullptr;
                    mBuffers[j].allocated = false;
                }
                mBufferCount = 0;
                ALOGE("mmap failed (restart): %s", strerror(errno));
                return -ENOMEM;
            }
            mBuffers[i].allocated = true;
        }
        ALOGI("Buffers re-allocated after stop: %d", mBufferCount);
    }

    /* Re-request buffers (idempotent: reuses existing if count matches) */
    for (int i = 0; i < mBufferCount; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(mFd, VIDIOC_QBUF, &buf) < 0) return -errno;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(mFd, VIDIOC_STREAMON, &type) < 0) return -errno;

    /* Set initial exposure/gain AFTER streaming starts (sensor must be powered) */
    if (mConfig.initialExposure > 0) mCurrentExposure = mConfig.initialExposure;
    if (mConfig.initialGain > 0) mCurrentGain = mConfig.initialGain;
    setExposure(mCurrentExposure);
    setGain(mCurrentGain);
    ALOGI("Initial exposure=%d gain=%d", mCurrentExposure, mCurrentGain);

    mStreaming = true;
    mState = PIPELINE_STREAMING;
    return 0;
}

int CameraPipeline::stopStreaming() {
    if (!mStreaming) return 0;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(mFd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < mBufferCount; i++) {
        if (mBuffers[i].start) {
            munmap(mBuffers[i].start, mBuffers[i].length);
            mBuffers[i].start = nullptr;
            mBuffers[i].allocated = false;
        }
    }
    mBufferCount = 0;

    mStreaming = false;
    mState = PIPELINE_OPENED;
    return 0;
}

void CameraPipeline::doAutoExposure(const uint8_t* rgbBuffer) {
    if (!rgbBuffer || !mConfig.enableAE) return;

    int w = mConfig.width;
    int h = mConfig.height;
    int step = 8;
    uint64_t sum = 0;
    int count = 0;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            int off = (y * w + x) * 3;
            uint8_t r = rgbBuffer[off], g = rgbBuffer[off+1], b = rgbBuffer[off+2];
            sum += (r * 77 + g * 150 + b * 29) >> 8;
            count++;
        }
    }
    if (count == 0) return;
    float avgLuma = (float)sum / count / 255.0f;
    float target = mConfig.targetLuma;

    /* [2026-11-22 flicker] Every frame used to multiply straight by target/avg with
     * no dead zone and no step limit, so an overshoot flipped the exposure back and
     * the picture visibly pulsed (measured by the user). It is proportional control
     * with a dead zone now:
     *   - within +/-12% nothing moves (rejects quantisation noise and step jitter);
     *   - each step covers only 60% of the gap and at most 1.33x/0.75x per frame, so
     *     it converges monotonically without overshoot. */
    if (avgLuma > target * 0.88f && avgLuma < target * 1.12f) {
        return;
    }
    float ratio = target / (avgLuma > 0.001f ? avgLuma : 0.001f);
    if (ratio > 1.0f) {
        ratio = 1.0f + (ratio - 1.0f) * 0.6f;
        if (ratio > 1.33f) ratio = 1.33f;
    } else {
        ratio = 1.0f - (1.0f - ratio) * 0.6f;
        if (ratio < 0.75f) ratio = 0.75f;
    }

    /* Sensor caps: imx179 allows exposure 1..2500 lines and gain 0..255
     * (v4l2_ctrl_new_std in imx179.c). The old caps (2000/120) saturated on
     * ordinary indoor scenes and left the frame badly underexposed even
     * though the sensor still had headroom. */
    /* [2026-11-22] The imx179 driver allows exposure 1..2500 and gain 0..255. Gain
     * used to be capped at 180, so dark scenes could only be pushed with software
     * gain (measured up to 3.91x, times 2.18x from AWB, which washed the picture
     * white). Prefer real sensor gain. */
    /* Measured: raising the gain cap to 255 makes the picture go black instead (the
     * driver registers are not a linear mapping; 0x40 is 1x). 180 is the highest
     * usable value verified on this device. */
    const int kExpMax = 2500;
    const int kGainMax = 180;

    int newExp = (int)(mCurrentExposure * ratio);
    int newGain = mCurrentGain;

    /* Prefer longer exposure over higher gain to reduce noise */
    if (newExp > 2500) {
        newGain = (int)(mCurrentGain * (newExp / 2500.0f));
        newExp = 2500;
    } else if (newGain > 100 && newExp < 2000) {
        /* If gain is high, increase exposure instead */
        newExp = (int)(newExp * (newGain / 100.0f));
        newGain = 100;
        if (newExp > kExpMax) newExp = kExpMax;
    }
    if (newExp < 10) {
        newExp = 10;
    }
    if (newExp > kExpMax) newExp = kExpMax;
    if (newGain > kGainMax) newGain = kGainMax;
    if (newGain < 1) newGain = 1;

    if (newExp != mCurrentExposure || newGain != mCurrentGain) {
        if (newGain != mCurrentGain) setGain(newGain);
        if (newExp != mCurrentExposure) setExposure(newExp);
        ALOGI("AE: luma=%.2f target=%.2f exp=%d(%d) gain=%d(%d)",
              avgLuma, target, newExp, mCurrentExposure, newGain, mCurrentGain);
    }

    /* Sensor saturated? Push the remaining brightness through software gain.
     * The prefer-exposure branch caps exposure at 2000 lines, so saturation
     * means exp==2000 && gain==kGainMax. Ramp mDigitalGain up when luma is
     * still well below target; back off as soon as luma recovers. Baseline
     * stays at config.digitalGain (1.5) to avoid permanent noise
     * amplification in well-lit scenes. */
    if (mCurrentExposure >= 2500 && mCurrentGain >= kGainMax) {
        float dg = mDigitalGain;
        if (avgLuma < target * 0.8f) {
            /* [2026-11-22] This used to raise without limit until need was met (measured
             * up to 3.91x), washing out the picture and amplifying noise. Software gain
             * only fills small gaps now, ceiling 1.5x. */
            float need = target / (avgLuma > 0.05f ? avgLuma : 0.05f);
            if (need > 1.25f) need = 1.25f;
            dg = dg * need;
        } else if (avgLuma > target * 1.1f) {
            dg = dg * 0.8f;
        }
        if (dg < mConfig.digitalGain) dg = mConfig.digitalGain;
        if (dg > 1.5f) dg = 1.5f;
        if (dg > mDigitalGain * 1.05f || dg < mDigitalGain * 0.95f) {
            ALOGI("AE saturation: luma=%.2f digitalGain %.2f -> %.2f",
                  avgLuma, mDigitalGain, dg);
            mDigitalGain = dg;
        }
    } else if (mDigitalGain > mConfig.digitalGain * 1.05f) {
        /* Exposure headroom returned (scene brightened) - restore baseline */
        mDigitalGain = mConfig.digitalGain;
    }
}

void CameraPipeline::doAutoWhiteBalance(const uint8_t* rgbBuffer) {
    if (!rgbBuffer || !mConfig.enableAWB) return;

    int w = mConfig.width;
    int h = mConfig.height;
    int step = 16;
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    int pixelCount = 0;

    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            int off = (y * w + x) * 3;
            sumR += rgbBuffer[off];
            sumG += rgbBuffer[off+1];
            sumB += rgbBuffer[off+2];
            pixelCount++;
        }
    }

    if (pixelCount < 100) return;
    float avgR = (float)sumR / pixelCount;
    float avgG = (float)sumG / pixelCount;
    float avgB = (float)sumB / pixelCount;

    /* [2026-11-23 dark-scene green cast] The global average is dominated by noise in
     * the dark: a white wall lit by a lamp covers only 11% of the pixels while 88% is
     * OV5693's green-dominated dark noise, so the computed gain only neutralises the
     * full-frame mean and the lit wall is never corrected. In dark scenes the gain
     * comes from the brightest decile instead (the real white reference: lit surfaces
     * and highlights); bright-scene global statistics are verified correct (back
     * camera R/G=1.00) and stay untouched. */
    bool dark = (avgG < 30.0f);
    if (dark) {
        /* Pass 1: luma histogram over the sample grid, to find the brightest-decile threshold. */
        uint32_t hist[256];
        memset(hist, 0, sizeof(hist));
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                int off = (y * w + x) * 3;
                int l = (rgbBuffer[off] * 299 + rgbBuffer[off+1] * 587 +
                         rgbBuffer[off+2] * 114) / 1000;
                hist[l]++;
            }
        }
        uint32_t total = 0;
        for (int i = 0; i < 256; i++) total += hist[i];
        uint32_t seen = 0, brightTarget = total / 10;
        int brightThr = 255;
        for (int i = 255; i >= 0; i--) {
            seen += hist[i];
            if (seen >= brightTarget) { brightThr = i; break; }
        }

        /* Pass 2: accumulate only the samples above the threshold. */
        uint64_t bR = 0, bG = 0, bB = 0;
        uint32_t bN = 0;
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                int off = (y * w + x) * 3;
                int l = (rgbBuffer[off] * 299 + rgbBuffer[off+1] * 587 +
                         rgbBuffer[off+2] * 114) / 1000;
                if (l >= brightThr) {
                    bR += rgbBuffer[off];
                    bG += rgbBuffer[off+1];
                    bB += rgbBuffer[off+2];
                    bN++;
                }
            }
        }

        /* The bright region needs enough samples and real signal (avgG>=8, below that
         * it is the noise floor) before the reference is switched; otherwise keep the
         * current gain and do not chase noise. */
        if (bN >= 100) {
            float bAvgR = (float)bR / bN;
            float bAvgG = (float)bG / bN;
            float bAvgB = (float)bB / bN;
            if (bAvgG >= 8.0f) {
                avgR = bAvgR;
                avgG = bAvgG;
                avgB = bAvgB;
                ALOGI("AWB: using bright decile (%u samples, thr=%d)", bN, brightThr);
            } else {
                ALOGI("AWB: dark scene, bright decile has no signal, keep gains");
                return;
            }
        }
        /* Not enough samples: fall back to the global average (previous behaviour). */
    }

    /* [2026-11-22 front camera green cast] All three channel means are tiny in the
     * dark and AWB used to be skipped below 5, so OV5693's native green cast (the
     * source of "the front camera is green") was never corrected in the dark. Lower it
     * to 2: 2/4 is about 0.5 in 8-bit terms and noise still cannot push the means into
     * a false balance. The 5 threshold only made sense for bright back-camera scenes;
     * the dark front camera has to take part too. */
    if (avgR < 2.0f || avgG < 2.0f || avgB < 2.0f) return;

    float rGain = avgG / avgR;
    float bGain = avgG / avgB;

    rGain = (rGain < 0.5f) ? 0.5f : (rGain > 3.0f) ? 3.0f : rGain;
    bGain = (bGain < 0.5f) ? 0.5f : (bGain > 3.0f) ? 3.0f : bGain;

    float alpha = 0.3f;
    if (!mHasAwbInit) {
        mAwbGains[0] = rGain;
        mAwbGains[2] = bGain;
        mHasAwbInit = true;
    } else {
        mAwbGains[0] = mAwbGains[0] * (1.0f - alpha) + rGain * alpha;
        mAwbGains[2] = mAwbGains[2] * (1.0f - alpha) + bGain * alpha;
    }
    mAwbGains[1] = 1.0f;
    mAwbGains[3] = 1.0f;

    ALOGI("AWB: R/G=%.2f B/G=%.2f gains R=%.2f B=%.2f", avgR/avgG, avgB/avgG, mAwbGains[0], mAwbGains[2]);
}

static void applyGamma(uint8_t* rgb, int width, int height, float gamma) {
    uint8_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = (uint8_t)(powf(i / 255.0f, gamma) * 255.0f + 0.5f);
    int total = width * height;
    for (int i = 0; i < total * 3; i++)
        rgb[i] = lut[rgb[i]];
}

static void flipVertical(uint8_t* buf, int width, int height, int bpp) {
    int rowSize = width * bpp;
    uint8_t* tmp = new uint8_t[rowSize];
    for (int y = 0; y < height / 2; y++) {
        int topOff = y * rowSize;
        int botOff = (height - 1 - y) * rowSize;
        memcpy(tmp, buf + topOff, rowSize);
        memcpy(buf + topOff, buf + botOff, rowSize);
        memcpy(buf + botOff, tmp, rowSize);
    }
    delete[] tmp;
}

int CameraPipeline::captureFrameInternal(uint8_t* rgbaOutput) {
    if (mState != PIPELINE_STREAMING) return -EINVAL;
    if (!rgbaOutput) return -EINVAL;

    struct pollfd pfd;
    pfd.fd = mFd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int poll_timeout_ms = 300;
    int max_retries = 6;
    int retry_count = 0;
    int pollRet = 0;

    do {
        pfd.revents = 0;
        pollRet = poll(&pfd, 1, poll_timeout_ms);
        if (pollRet < 0) return -errno;
        if (pollRet == 0) {
            retry_count++;
            if (retry_count >= max_retries) return -EAGAIN;
            continue;
        }
        if (!(pfd.revents & POLLIN)) return -EAGAIN;
        break;
    } while (retry_count < max_retries);

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    int ret = ioctl(mFd, VIDIOC_DQBUF, &buf);
    if (ret < 0) {
        if (errno == EAGAIN) return -EAGAIN;
        return -errno;
    }

    if (buf.index >= mBufferCount) return -EINVAL;

    uint8_t* frameBuffer = (uint8_t*)mBuffers[buf.index].start;
    uint32_t frameSize = buf.bytesused;

    if (mConfig.enableISP && mDemosaic && mColorConv) {
        /* Debug: raw frame dump (persist.mocha.camera.rawdump=1 -> dump first 3 frames) */
        static int sDumpCount = -1;
        if (sDumpCount < 0) {
            char prop[PROP_VALUE_MAX] = {0};
            __system_property_get("persist.mocha.camera.rawdump", prop);
            sDumpCount = (prop[0] == '1') ? 3 : 0;
        }
        if (sDumpCount > 0) {
            char path[128];
            snprintf(path, sizeof(path),
                     "/data/local/tmp/cam_raw_%dx%d_%d.bin",
                     mConfig.width, mConfig.height, 3 - sDumpCount);
            int dfd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (dfd >= 0) {
                size_t sz = (size_t)mConfig.width * mConfig.height * 2;
                ssize_t written = ::write(dfd, frameBuffer, sz < frameSize ? sz : frameSize);
                (void)written;
                ::close(dfd);
                ALOGI("rawdump: wrote %s (%u bytes)", path, frameSize);
            }
            sDumpCount--;
        }
        ret = processBayerToYuv(frameBuffer, rgbaOutput, HAL_PIXEL_FORMAT_RGBA_8888);
    } else {
        memcpy(rgbaOutput, frameBuffer, frameSize);
        ret = 0;
    }

    if (ioctl(mFd, VIDIOC_QBUF, &buf) < 0) return -errno;

    return ret;
}

int CameraPipeline::captureFrame(uint8_t* outputBuffer, uint32_t outputFormat) {
    /* Legacy API: RGBA at pipeline resolution, or YUV written directly by
     * processBayerToYuv when dims match (recording/scan pipelines are
     * configured at the encoder/analysis resolution). */
    if (outputFormat == HAL_PIXEL_FORMAT_YCBCR_420_888 ||
        outputFormat == HAL_PIXEL_FORMAT_YV12) {
        return captureFrameDirect(outputBuffer, outputFormat);
    }
    return captureFrameInternal(outputBuffer);
}

int CameraPipeline::captureFrameDirect(uint8_t* outputBuffer, uint32_t outputFormat) {
    if (mState != PIPELINE_STREAMING) return -EINVAL;
    if (!outputBuffer) return -EINVAL;

    struct pollfd pfd;
    pfd.fd = mFd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int poll_timeout_ms = 300;
    int max_retries = 6;
    int retry_count = 0;
    int pollRet = 0;

    do {
        pfd.revents = 0;
        pollRet = poll(&pfd, 1, poll_timeout_ms);
        if (pollRet < 0) return -errno;
        if (pollRet == 0) {
            retry_count++;
            if (retry_count >= max_retries) return -EAGAIN;
            continue;
        }
        if (!(pfd.revents & POLLIN)) return -EAGAIN;
        break;
    } while (retry_count < max_retries);

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    int ret = ioctl(mFd, VIDIOC_DQBUF, &buf);
    if (ret < 0) {
        if (errno == EAGAIN) return -EAGAIN;
        return -errno;
    }

    if (buf.index >= mBufferCount) return -EINVAL;

    uint8_t* frameBuffer = (uint8_t*)mBuffers[buf.index].start;
    uint32_t frameSize = buf.bytesused;

    ret = processBayerToYuv(frameBuffer, outputBuffer, outputFormat);

    if (ioctl(mFd, VIDIOC_QBUF, &buf) < 0) return -errno;

    return ret;
}

/* Nearest-neighbour crop+scale from a full-frame RGBA source. The crop
 * window (cx,cy,cw,ch) is expressed in source pixels; digital zoom is just
 * a smaller window scaled up to the output size. */
static void cropScaleRgba(const uint8_t* src, int sw, int sh,
                          uint8_t* dst, int dw, int dh,
                          int cx, int cy, int cw, int ch) {
    /* [2026-11-22 full-screen preview] When the crop window aspect differs from the
     * output, first tighten the window around its centre to the output ratio and then
     * scale nearest-neighbour, so 16:9 capture -> 4:3 preview crops the sides instead
     * of squeezing the picture horizontally. When the ratios already match (recording,
     * stills) nothing here changes and behaviour is exactly as before. */
    if (cw > 0 && ch > 0 && dw > 0 && dh > 0) {
        if ((int64_t)cw * dh > (int64_t)ch * dw) {
            int nw = (int)((int64_t)ch * dw / dh) & ~1;
            if (nw > 0 && nw <= cw) { cx += (cw - nw) / 2; cw = nw; }
        } else if ((int64_t)cw * dh < (int64_t)ch * dw) {
            int nh = (int)((int64_t)cw * dh / dw) & ~1;
            if (nh > 0 && nh <= ch) { cy += (ch - nh) / 2; ch = nh; }
        }
    }
    for (int y = 0; y < dh; y++) {
        int sy = cy + (int)((int64_t)y * ch / dh);
        if (sy >= sh) sy = sh - 1;
        const uint32_t* srow = reinterpret_cast<const uint32_t*>(
                src + (size_t)sy * sw * 4);
        uint32_t* drow = reinterpret_cast<uint32_t*>(
                dst + (size_t)y * dw * 4);
        for (int x = 0; x < dw; x++) {
            int sx = cx + (int)((int64_t)x * cw / dw);
            if (sx >= sw) sx = sw - 1;
            drow[x] = srow[sx];
        }
    }
}

void CameraPipeline::setCrop(int x, int y, int w, int h) {
    if (mState == PIPELINE_CLOSED) return;
    if (w <= 0 || h <= 0) return;
    int W = mConfig.width, H = mConfig.height;
    /* keep the window inside the frame, even offsets and even size so the
     * Bayer phase of the sampled region stays consistent */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w > W) w = W;
    if (h > H) h = H;
    if (x + w > W) x = W - w;
    if (y + h > H) y = H - h;
    x &= ~1; y &= ~1; w &= ~1; h &= ~1;
    if (w < 32) w = 32;
    if (h < 32) h = 32;
    mCropX = x; mCropY = y; mCropW = w; mCropH = h;
}

void CameraPipeline::getCrop(int* x, int* y, int* w, int* h) const {
    if (x) *x = mCropX;
    if (y) *y = mCropY;
    if (w) *w = mCropW;
    if (h) *h = mCropH;
}

int CameraPipeline::captureFrameRgba(uint8_t* outputBuffer,
                                     uint32_t outW, uint32_t outH) {
    if (mState != PIPELINE_STREAMING) return -EINVAL;
    if (!outputBuffer || outW == 0 || outH == 0) return -EINVAL;

    const bool fullFrame = (mCropW == (int)mConfig.width &&
                            mCropH == (int)mConfig.height &&
                            outW == mConfig.width && outH == mConfig.height);
    if (fullFrame) {
        /* [2026-11-22] This used to call processBayerToYuv(nullptr, ...) once before
         * capturing: a null pointer into demosaic, a wasted call that also wrote
         * garbage into the output buffer. The whole frame goes straight through
         * captureFrameInternal, which does demosaic/WB/gamma/flip itself. */
        return captureFrameInternal(outputBuffer);
    }
    /* general path: full-frame RGBA, then crop+scale */
    int ret = captureFrameInternal(mRgbaFull);
    if (ret != 0) return ret;
    cropScaleRgba(mRgbaFull, mConfig.width, mConfig.height,
                  outputBuffer, (int)outW, (int)outH,
                  mCropX, mCropY, mCropW, mCropH);
    return 0;
}

int CameraPipeline::processBayerToYuv(const uint8_t* bayerData, uint8_t* output, uint32_t outputFormat) {
    if (!bayerData || !output || !mDemosaic || !mColorConv) return -EINVAL;

    mDemosaic->process(bayerData, mRgbBuffer);

    /* AE update before applying gains */
    if (mConfig.enableAE)
        doAutoExposure(mRgbBuffer);

    /* AWB update before applying gains */
    if (mConfig.enableAWB)
        doAutoWhiteBalance(mRgbBuffer);

    float rG = (mConfig.enableAWB ? mAwbGains[0] : mConfig.wbGain[0]) * mDigitalGain;
    float gG = (mConfig.enableAWB ? mAwbGains[1] : mConfig.wbGain[1]) * mDigitalGain;
    float bG = (mConfig.enableAWB ? mAwbGains[2] : mConfig.wbGain[2]) * mDigitalGain;

    if (outputFormat == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        /* For YUV: apply WB gains in-place first, then gamma, then convert */
        {
            int total = mConfig.width * mConfig.height;
            if (rG != 1.0f || gG != 1.0f || bG != 1.0f) {
                for (int i = 0; i < total; i++) {
                    int off = i * 3;
                    int r = (int)(mRgbBuffer[off]   * rG);
                    int g = (int)(mRgbBuffer[off+1] * gG);
                    int b = (int)(mRgbBuffer[off+2] * bG);
                    mRgbBuffer[off]   = r > 255 ? 255 : (uint8_t)r;
                    mRgbBuffer[off+1] = g > 255 ? 255 : (uint8_t)g;
                    mRgbBuffer[off+2] = b > 255 ? 255 : (uint8_t)b;
                }
            }
        }
        applyGamma(mRgbBuffer, mConfig.width, mConfig.height, mConfig.gamma);
        uint8_t* yPlane = output;
        uint8_t* uvPlane = output + mConfig.width * mConfig.height;
        mColorConv->rgbToNv12(mRgbBuffer, yPlane, uvPlane);
    } else if (outputFormat == HAL_PIXEL_FORMAT_RGBA_8888 || outputFormat == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        /* Merged WB + gamma + RGB→RGBA + flip in one pass */
        mColorConv->rgbToRgbaWbGamma(mRgbBuffer, output,
                                     mConfig.width, mConfig.height,
                                     rG, gG, bG, mGammaLut, mConfig.flipV);
    } else {
        /* For other formats: apply WB and gamma, then raw copy */
        {
            int total = mConfig.width * mConfig.height;
            if (rG != 1.0f || gG != 1.0f || bG != 1.0f) {
                for (int i = 0; i < total; i++) {
                    int off = i * 3;
                    int r = (int)(mRgbBuffer[off]   * rG);
                    int g = (int)(mRgbBuffer[off+1] * gG);
                    int b = (int)(mRgbBuffer[off+2] * bG);
                    mRgbBuffer[off]   = r > 255 ? 255 : (uint8_t)r;
                    mRgbBuffer[off+1] = g > 255 ? 255 : (uint8_t)g;
                    mRgbBuffer[off+2] = b > 255 ? 255 : (uint8_t)b;
                }
            }
        }
        applyGamma(mRgbBuffer, mConfig.width, mConfig.height, mConfig.gamma);
        memcpy(output, mRgbBuffer, mRgbBufferSize);
    }

    return 0;
}

int CameraPipeline::initFocuser() {
    mFocusPosition = -1;
    ALOGI("Focuser: kernel controls AD5823 power/I2C; position 400 set at stream start");
    return 0;
}

void CameraPipeline::deinitFocuser() {
}

int CameraPipeline::setFocus(int position) {
    if (position < 0) position = 0;
    if (position > 1023) position = 1023;

    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
    ctrl.value = position;

    int ret = ioctl(mFd, VIDIOC_S_CTRL, &ctrl);
    if (ret == 0) {
        mFocusPosition = position;
        ALOGI("Focuser: set position %d via V4L2", position);
    } else {
        ALOGW("Focuser: ioctl V4L2_CID_FOCUS_ABSOLUTE=%d failed: %s",
              position, strerror(errno));
    }
    return ret;
}

/*
 * Horizontal Sobel energy on the green channel of an RGB buffer.
 * Uses 3×3 Sobel kernel for horizontal edges:
 *   Gx = | -1 0 +1 |
 *        | -2 0 +2 |
 *        | -1 0 +1 |
 * Only green channel (byte-offset 1) is used — fast approximation.
 */
int CameraPipeline::sobelEnergy(const uint8_t* rgb, int w, int h) const {
    if (!rgb || w < 3 || h < 3) return 0;
    int total = 0;
    int stride = w * 3;
    for (int y = 1; y < h - 1; y++) {
        const uint8_t* row = rgb + y * stride;
        for (int x = 1; x < w - 1; x++) {
            int g = abs((int)row[(x+1)*3+1] - (int)row[(x-1)*3+1]); // horizontal gradient
            total += g;
        }
    }
    return total;
}

/*
 * 8-pixel-wide horizontal sobel using green channel.
 * Processes 8 adjacent output pixels at a time, summing partial gradients.
 * Same algorithm as sobelEnergy() but loop-unrolled for 8x regions.
 * [2026-11-26 tap-to-focus regions] x0/y0/x1/y1 limit the measurement window
 * (caller clamps them; full frame = 1/1/w-1/h-1).
 */
int CameraPipeline::sobelEnergyNw(const uint8_t* rgb, int w, int h,
                                  int x0, int y0, int x1, int y1) const {
    if (!rgb || w < 3 || h < 3) return 0;
    if (x1 <= x0 || y1 <= y0) { x0 = 1; y0 = 1; x1 = w - 1; y1 = h - 1; }
    int total = 0;
    int stride = w * 3;
    int x;
    /* Process 8-wide chunks */
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = rgb + y * stride;
        x = x0;
        while (x + 8 < x1) {
            int g0 = abs((int)row[(x+1)*3+1] - (int)row[(x-1)*3+1]);
            int g1 = abs((int)row[(x+2)*3+1] - (int)row[(x+0)*3+1]);
            int g2 = abs((int)row[(x+3)*3+1] - (int)row[(x+1)*3+1]);
            int g3 = abs((int)row[(x+4)*3+1] - (int)row[(x+2)*3+1]);
            int g4 = abs((int)row[(x+5)*3+1] - (int)row[(x+3)*3+1]);
            int g5 = abs((int)row[(x+6)*3+1] - (int)row[(x+4)*3+1]);
            int g6 = abs((int)row[(x+7)*3+1] - (int)row[(x+5)*3+1]);
            int g7 = abs((int)row[(x+8)*3+1] - (int)row[(x+6)*3+1]);
            total += g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7;
            x += 8;
        }
        /* Remainder */
        for (; x < x1; x++) {
            total += abs((int)row[(x+1)*3+1] - (int)row[(x-1)*3+1]);
        }
    }
    return total;
}

/*
 * Capture one V4L2 frame and return Sobel energy from the green channel.
 * Used during AF sweep — captures a frame, demosaics to RGB, measures contrast.
 */
int CameraPipeline::captureForAf() {
    if (mState != PIPELINE_STREAMING) return -EINVAL;

    struct pollfd pfd;
    pfd.fd = mFd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pollRet = poll(&pfd, 1, 500);
    if (pollRet <= 0) return -EAGAIN;
    if (!(pfd.revents & POLLIN)) return -EAGAIN;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;

    int ret = ioctl(mFd, VIDIOC_DQBUF, &buf);
    if (ret < 0) return -errno;
    if (buf.index >= mBufferCount) return -EINVAL;

    uint8_t* frameBuffer = (uint8_t*)mBuffers[buf.index].start;
    int energy = 0;

    if (mConfig.enableISP && mDemosaic && mColorConv) {
        /* Demosaic to mRgbBuffer */
        mDemosaic->process(frameBuffer, mRgbBuffer);

        /* [2026-11-26 async AF] The sweep no longer runs AE/AWB/gamma per step:
         *  - AE keeps moving exposure/gain between sweep positions, which skews
         *    the energy curve (darker frame = smaller gradient = fake peak);
         *  - AWB and gamma are pure colour/curve ops, the Sobel energy only
         *    depends on the green-channel gradient;
         *  - skipping them cuts per-step cost so the whole sweep finishes in a
         *    fraction of the old time. The AE the main loop runs keeps the
         *    exposure where the preview converged, which is what the sweep
         *    measures under. */
        /* Measure Sobel energy on the green channel, limited to the AF region
         * when a tap-to-focus window is set (0/0/0/0 = whole frame). */
        int x0 = mAfRegX, y0 = mAfRegY;
        int x1 = x0 + mAfRegW, y1 = y0 + mAfRegH;
        if (x0 <= 0 && y0 <= 0 && mAfRegW <= 0 && mAfRegH <= 0) {
            x0 = 1; y0 = 1; x1 = mConfig.width - 1; y1 = mConfig.height - 1;
        }
        if (x0 < 1) x0 = 1;
        if (y0 < 1) y0 = 1;
        if (x1 > mConfig.width - 1) x1 = mConfig.width - 1;
        if (y1 > mConfig.height - 1) y1 = mConfig.height - 1;
        if (x1 - x0 < 8 || y1 - y0 < 8) { x0 = 1; y0 = 1; x1 = mConfig.width - 1; y1 = mConfig.height - 1; }
        energy = sobelEnergyNw(mRgbBuffer, mConfig.width, mConfig.height, x0, y0, x1, y1);
    }

    /* Return buffer to queue */
    buf.m.userptr = (unsigned long)mBuffers[buf.index].start;
    buf.length = mBuffers[buf.index].length;
    if (ioctl(mFd, VIDIOC_QBUF, &buf) < 0) return -errno;

    return energy;
}

void CameraPipeline::startAfScan(int rx, int ry, int rw, int rh) {
    if (mState != PIPELINE_STREAMING) {
        ALOGW("AF: cannot scan, not streaming");
        mAfState = 0;
        return;
    }

    /* [2026-11-26 async AF] A tap-to-focus while a sweep is running must restart
     * with the new region: stop the old thread first, then launch a fresh one.
     * startAfScan() stays non-blocking - the old synchronous version froze the
     * preview for the whole sweep because it ran on the request thread.
     * Gate on joinable(), not the active flag (see close(): the thread clears
     * the flag just before it is reaped, and re-assigning a joinable
     * std::thread is std::terminate()). */
    if (mAfThread.joinable()) {
        mAfAbort = true;
        mAfThread.join();
    }
    mAfThreadActive = false;

    /* Clamp the region to the pipeline frame; 0/0/0/0 or a degenerate window
     * scans the whole frame. */
    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 ||
        rx + rw > mConfig.width || ry + rh > mConfig.height ||
        rw < 16 || rh < 16) {
        rx = 0; ry = 0; rw = 0; rh = 0;
    }
    mAfRegX = rx; mAfRegY = ry; mAfRegW = rw; mAfRegH = rh;

    mAfAbort = false;
    mAfState = 1; // MOVING
    if (rw && rh)
        ALOGI("AF: starting contrast scan on region %dx%d@(%d,%d)", rw, rh, rx, ry);
    else
        ALOGI("AF: starting contrast-detection scan (full frame)");
    mAfThreadActive = true;
    mAfThread = std::thread(&CameraPipeline::afScanLoop, this);
}

void CameraPipeline::afScanLoop() {
    int bestEnergy = 0;
    int bestPos = 400;
    /* [2026-11-25 AF range] The AD5823 actuator has 1024 positions (0 = infinity,
     * 1023 = closest focus, see AD5823_ACTUATOR_RANGE). The sweep used to stop at
     * 640, so anything closer than ~40 cm never reached its focus peak and the
     * preview stayed blurry no matter how well the scan went. Cover the full
     * travel instead: 17 coarse steps of 64 plus a +/-64 fine pass around the
     * winner. */
    const int kPosMin = 0;
    const int kPosMax = 1023;
    const int coarseStep = 64;
    const int fineStep = 8;
    const int settleMs = 60;

    /* ---- Coarse sweep: full actuator range ---- */
    for (int pos = kPosMin; pos <= kPosMax && !mAfAbort.load(); pos += coarseStep) {
        setFocus(pos);
        usleep(settleMs * 1000);
        if (mState != PIPELINE_STREAMING) { mAfState = 0; mAfThreadActive = false; return; }
        int energy = captureForAf();
        if (energy < 0) {
            ALOGW("AF: capture error at pos %d: %d", pos, energy);
            continue;
        }
        ALOGI("AF: coarse pos=%d energy=%d", pos, energy);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestPos = pos;
        }
    }

    /* ---- Fine sweep around best ---- */
    int fineStart = bestPos - coarseStep;
    if (fineStart < kPosMin) fineStart = kPosMin;
    int fineEnd = bestPos + coarseStep;
    if (fineEnd > kPosMax) fineEnd = kPosMax;

    for (int pos = fineStart; pos <= fineEnd && !mAfAbort.load(); pos += fineStep) {
        if (pos == bestPos) continue; // already measured
        setFocus(pos);
        usleep(settleMs * 1000);
        if (mState != PIPELINE_STREAMING) { mAfState = 0; mAfThreadActive = false; return; }
        int energy = captureForAf();
        if (energy < 0) continue;
        ALOGI("AF: fine pos=%d energy=%d", pos, energy);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestPos = pos;
        }
    }

    if (mAfAbort.load()) {
        ALOGI("AF: sweep aborted");
        mAfThreadActive = false;
        return;
    }

    /* ---- Lock to best position ---- */
    setFocus(bestPos);
    usleep(settleMs * 1000);
    mAfState = 4; // FOCUSED_LOCKED
    if (mAfRegW && mAfRegH)
        ALOGI("AF: scan complete (region %dx%d@(%d,%d)), lock at position=%d energy=%d",
              mAfRegW, mAfRegH, mAfRegX, mAfRegY, bestPos, bestEnergy);
    else
        ALOGI("AF: scan complete, lock at position=%d energy=%d", bestPos, bestEnergy);
    mAfThreadActive = false;
}

void CameraPipeline::cancelAf() {
    /* [2026-11-26 async AF] Stop the worker sweep, but do NOT move the lens back
     * to infinity: apps that tap-to-focus send AF_TRIGGER_CANCEL right before
     * AF_TRIGGER_START (to abort any running scan), and parking the lens at 0 in
     * between made every tap start its scan from the far end, which looked slow
     * and wrong. Leaving the lens where it is costs nothing and the fresh sweep
     * re-positions it within one step. */
    if (mAfThread.joinable()) {
        mAfAbort = true;
        mAfThread.join();
    }
    mAfThreadActive = false;
    mAfState = 0; // INACTIVE
    ALOGI("AF: scan cancelled, lens left in place");
}

} // namespace mocha
