#include "JpegEncoder.h"
#include <stdio.h>
#include <cutils/log.h>
#include <jpeglib.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#ifndef LOG_TAG
#define LOG_TAG "MochaCameraHAL"
#endif

namespace mocha {

JpegEncoder::JpegEncoder() : mInitialized(false) {}

JpegEncoder::~JpegEncoder() {}

/* [2026-11-25 photo orientation] Build an EXIF APP1 segment carrying only the
 * Orientation tag. Camera2 apps do not rotate stills themselves - they pass
 * ANDROID_JPEG_ORIENTATION and expect the HAL to make the file come out upright.
 * This HAL encoded the raw sensor frame with no metadata at all, so every photo was
 * saved sideways and the user had to turn it by hand.
 *
 * EXIF Orientation is not "rotate the data by N"; it describes the transform a
 * decoder has to apply, so the degrees-clockwise-to-upright value maps to:
 *   90 -> 6 (rotate 90 CW), 180 -> 3 (rotate 180), 270 -> 8 (rotate 270 CW).
 * The tag itself lives in a little-endian TIFF header with one IFD0 entry. */
static int exifOrientationTag(int degrees)
{
    int d = ((degrees % 360) + 360) % 360;
    switch (d) {
        case 90:  return 6;
        case 180: return 3;
        case 270: return 8;
        default:  return 0;   /* 0 -> tag 1 (normal), nothing to write */
    }
}

static void writeExifOrientation(j_compress_ptr cinfo, int degrees)
{
    const int tag = exifOrientationTag(degrees);
    if (tag == 0) return;

    /* "Exif\0\0" + TIFF header (II, 42, IFD0 at offset 8) + one 12-byte IFD entry
     * (tag 0x0112 Orientation, type 3 SHORT, count 1, value) + next-IFD offset 0. */
    unsigned char app1[6 + 8 + 2 + 12 + 4] = {
        'E','x','i','f',0,0,
        'I','I', 0x2A,0x00, 0x08,0x00,0x00,0x00,     /* TIFF header, IFD0 @8 */
        0x01,0x00,                                    /* one entry */
        0x12,0x01,                                    /* 0x0112 Orientation */
        0x03,0x00,                                    /* type SHORT */
        0x01,0x00,0x00,0x00,                          /* count 1 */
        (unsigned char)(tag & 0xFF), 0x00, 0x00,0x00, /* value + pad */
        0x00,0x00,0x00,0x00,                          /* no next IFD */
    };
    jpeg_write_marker(cinfo, JPEG_APP0 + 1, app1, (unsigned int)sizeof(app1));
}

/* libjpeg destination manager that writes to a fixed-size buffer */
struct jpeg_dest_mgr {
    struct jpeg_destination_mgr pub;
    uint8_t* buffer;
    size_t bufferSize;
    bool overflow;
};

static void dest_init_destination(j_compress_ptr cinfo) {
    jpeg_dest_mgr* dest = (jpeg_dest_mgr*)cinfo->dest;
    dest->pub.next_output_byte = dest->buffer;
    dest->pub.free_in_buffer = dest->bufferSize;
    dest->overflow = false;
}

static boolean dest_empty_output_buffer(j_compress_ptr cinfo) {
    jpeg_dest_mgr* dest = (jpeg_dest_mgr*)cinfo->dest;
    dest->overflow = true;
    return FALSE;
}

static void dest_term_destination(j_compress_ptr cinfo) { (void)cinfo; }

int JpegEncoder::encodeRGBA(const uint8_t* rgba, int width, int height, int quality,
                            uint8_t* output, size_t outputSize, size_t* jpegSize) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    struct jpeg_dest_mgr dest;
    dest.buffer = output;
    dest.bufferSize = outputSize;
    dest.pub.init_destination = dest_init_destination;
    dest.pub.empty_output_buffer = dest_empty_output_buffer;
    dest.pub.term_destination = dest_term_destination;
    cinfo.dest = &dest.pub;

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    /* Must come after start_compress so the marker lands right behind SOI, and
     * before the first scanline. */
    writeExifOrientation(&cinfo, mOrientation);

    row_stride = width * 4;

    uint8_t* rgbRow = (uint8_t*)malloc(width * 3);
    if (!rgbRow) {
        jpeg_destroy_compress(&cinfo);
        return -ENOMEM;
    }

    while (cinfo.next_scanline < (JDIMENSION)height) {
        const uint8_t* src = rgba + cinfo.next_scanline * row_stride;
        for (int x = 0; x < width; x++) {
            rgbRow[x * 3 + 0] = src[x * 4 + 0];
            rgbRow[x * 3 + 1] = src[x * 4 + 1];
            rgbRow[x * 3 + 2] = src[x * 4 + 2];
        }
        row_pointer[0] = rgbRow;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
        if (dest.overflow) break;
    }

    free(rgbRow);
    jpeg_finish_compress(&cinfo);

    *jpegSize = outputSize - dest.pub.free_in_buffer;

    if (dest.overflow) {
        ALOGE("JPEG output buffer too small: %zu bytes used, %zu available",
              *jpegSize, outputSize);
        jpeg_destroy_compress(&cinfo);
        return -ENOSPC;
    }

    ALOGI("JPEG encoded %dx%d quality=%d -> %zu bytes", width, height, quality, *jpegSize);

    jpeg_destroy_compress(&cinfo);
    return 0;
}

} // namespace mocha
