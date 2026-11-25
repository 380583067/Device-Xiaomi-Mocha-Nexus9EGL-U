#ifndef MOCHA_JPEG_ENCODER_H
#define MOCHA_JPEG_ENCODER_H

#include <cstdint>
#include <cstddef>

namespace mocha {

class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();

    int encodeRGBA(const uint8_t* rgba, int width, int height, int quality,
                   uint8_t* output, size_t outputSize, size_t* jpegSize);

    /* [2026-11-25] Degrees clockwise the viewer has to rotate the decoded image by
     * to see it upright (i.e. ANDROID_JPEG_ORIENTATION). Written as an EXIF
     * Orientation tag; 0 means "no tag". */
    void setOrientation(int degrees) { mOrientation = degrees; }

private:
    bool mInitialized;
    int mOrientation = 0;
};

} // namespace mocha

#endif // MOCHA_JPEG_ENCODER_H
