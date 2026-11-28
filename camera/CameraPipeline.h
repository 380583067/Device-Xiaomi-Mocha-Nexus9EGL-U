#ifndef MOCHA_CAMERA_PIPELINE_H
#define MOCHA_CAMERA_PIPELINE_H

#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>



#include <system/graphics.h>

#include "isp/DemosaicNEON.h"
#include "isp/ColorConvNEON.h"

namespace mocha {

enum PipelineState {
    PIPELINE_CLOSED,
    PIPELINE_OPENED,
    PIPELINE_STREAMING
};

struct PipelineConfig {
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;
    uint8_t bayerPattern;
    uint8_t offset_x;
    uint8_t offset_y;
    bool flipV;
    bool enableISP;
    uint16_t blackLevel;
    float wbGain[4];
    float ccm[9];
    float gamma;
    bool enableAE;
    bool enableAWB;
    float targetLuma;
    float digitalGain;  // software brightness boost (multiplicative on RGB)
    /* [2026-11-26] Horizontal [1 2 1] anti-alias blend, 0..1. The OV5693 720p
     * mode subsamples the sensor window (x_inc=y_inc=2) which aliases high
     * frequencies into a 1-px diagonal checkerboard; a single horizontal pass
     * zeroes that frequency (see aaSmoothH in CameraPipeline.cpp).
     * 0 disables (rear camera, native-resolution readout, no aliasing). */
    float aaStrength;
    /* [2026-11-26] Fixed multipliers on the AWB gains at output time (the
     * measurement feedback is pre-gain, so these cannot hunt). Compensates the
     * OV5693's weak blue response: every measured front-camera scene came out
     * with B/G 0.90-0.94 after a correct white-point correction (window sky,
     * grey hoodie, wall), i.e. a constant yellow-green residue. 0/1.0 = no
     * boost (rear camera, IMX179 verified R/G=B/G=1.00). */
    float awbBoostR;
    float awbBoostB;
    /* [2026-11-28 front portrait video] Rotate the YUV (encoder) output 90 CCW
     * into a portrait buffer (outW = height, outH = width). Capture stays at
     * the landscape dims; only the YUV branch honours this. */
    bool outRotate90CCW;
    /* [2026-11-28 front video mirror] Emit the YUV stream mirrored so the
     * played-back video matches the (mirrored) selfie preview. Implemented as
     * a vertical flip of the landscape buffer: the app's 90 degree display
     * matrix turns that into a horizontal mirror on screen. */
    bool frontPreviewMirror;
    /* [2026-11-22] When the pipeline is rebuilt for a high-resolution still, carry
     * over the exposure/gain the previous instance already converged on: a fresh
     * instance starts at 800/60 and cannot converge in a dark scene from a few warm-up
     * frames, which made 8 MP photos dark. 0 means "use the defaults". */
    int initialExposure;
    int initialGain;
};

struct V4l2Buffer {
    void* start;
    size_t length;
    bool allocated;
};

class CameraPipeline {
public:
    CameraPipeline();
    ~CameraPipeline();

    int open(int cameraId);
    int close();

    int configure(const PipelineConfig& config);
    int startStreaming();
    int stopStreaming();

    int captureFrame(uint8_t* outputBuffer, uint32_t outputFormat);
    /* Capture at the pipeline resolution, then crop (digital zoom) and
     * scale into an RGBA output buffer of arbitrary size. When outW/outH
     * match the pipeline config and no crop is set this is a plain
     * RGB->RGBA conversion, same as captureFrame(RGBA). */
    int captureFrameRgba(uint8_t* outputBuffer, uint32_t outW, uint32_t outH);
    /* Digital zoom: crop window in pipeline-pixel coordinates (even-aligned,
     * clamped). Pass the full frame to disable. */
    void setCrop(int x, int y, int w, int h);
    void getCrop(int* x, int* y, int* w, int* h) const;

    PipelineState getState() const { return mState; }

    int setExposure(int exposure);
    int setGain(int gain);
    int getExposure();
    int getGain();

    int setFocus(int position);
    int getFocusPosition() const { return mFocusPosition; }
    int getAfState() const { return mAfState; }
    /* [2026-11-26 tap-to-focus regions] The sweep may be limited to a window in
     * pipeline pixels (0/0/0/0 = whole frame). The scan runs on a worker thread:
     * the old synchronous version froze the preview for the whole 2-3 s sweep on
     * the process_capture_request() thread, which looked like "tap does nothing". */
    void startAfScan(int rx = 0, int ry = 0, int rw = 0, int rh = 0);
    void cancelAf();

private:
    int initFocuser();
    void deinitFocuser();
    /* DQBUF one frame, run the ISP, write RGBA at the pipeline resolution */
    int captureFrameInternal(uint8_t* rgbaOutput);
    /* Legacy direct path: YUV written at pipeline resolution (dims must match) */
    int captureFrameDirect(uint8_t* outputBuffer, uint32_t outputFormat);
    int captureForAf();       // Capture a frame during AF sweep, returns sobel energy
    int sobelEnergy(const uint8_t* rgb, int w, int h) const;
    /* [2026-11-26 tap-to-focus regions] x0/y0/x1/y1 limit the measurement window
     * (caller clamps; x1<=x0 falls back to the full frame). */
    int sobelEnergyNw(const uint8_t* rgb, int w, int h,
                      int x0, int y0, int x1, int y1) const;

private:
    int processBayerToYuv(const uint8_t* bayerData, uint8_t* output, uint32_t outputFormat);
    void doAutoExposure(const uint8_t* rgbBuffer);
    void doAutoWhiteBalance(const uint8_t* rgbBuffer);
    /* [2026-11-26] Front-camera anti-alias: separable [1 2 1] on both axes,
     * uses mAaTmp scratch. See the comment above aaSmoothH in the .cpp. */
    void applyAntiAlias();

    int mFd;
    int mCameraId;
    PipelineState mState;
    bool mStreaming;

    std::unique_ptr<DemosaicNEON> mDemosaic;
    std::unique_ptr<ColorConvNEON> mColorConv;

    PipelineConfig mConfig;

    V4l2Buffer mBuffers[4];
    int mBufferCount;
    int mCurrentBuffer;

    uint8_t* mRgbBuffer;
    /* [2026-11-28 front portrait video] Scratch for the rotated portrait
     * RGB frame (same byte size as mRgbBuffer). */
    uint8_t* mRotBuffer;
    uint32_t mRgbBufferSize;
    uint8_t* mRgbaFull;       /* RGBA at pipeline resolution (full frame) */
    uint32_t mRgbaFullSize;
    uint8_t* mAaTmp;          /* [2026-11-26] anti-alias smooth buffer (w*h*3) */
    size_t mAaTmpSize;
    int mCropX, mCropY, mCropW, mCropH;

    int mCurrentExposure;
    int mCurrentGain;
    float mAwbGains[4];
    bool mHasAwbInit;
    float mDigitalGain;  // live copy of config.digitalGain, AE may boost it
    uint8_t mGammaLut[256];
    float mLastGamma;

    int mFocusPosition;
    int mAfState;

    /* [2026-11-26 async AF] Worker-thread sweep state. mAfAbort makes an existing
     * sweep stop early when a new region is requested, the session closes or the
     * object is destroyed; mAfThreadActive guards join() ownership. */
    void afScanLoop();
    std::thread mAfThread;
    std::atomic<bool> mAfThreadActive{false};
    std::atomic<bool> mAfAbort{false};
    int mAfRegX, mAfRegY, mAfRegW, mAfRegH;
};

} // namespace mocha

#endif // MOCHA_CAMERA_PIPELINE_H
