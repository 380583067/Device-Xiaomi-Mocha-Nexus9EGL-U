/*
 * Copyright (C) 2017 The LineageOS Project
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

#include <stdlib.h>

#include <ui/GraphicBuffer.h>

/*
 * Bridge for the 2015 GraphicBuffer constructor used by the NVIDIA blobs.
 *
 * hwcomposer.tegra.so and libnvcap_video.so were built against
 *
 *   GraphicBuffer(uint32_t w, uint32_t h, PixelFormat format, uint32_t usage,
 *                 uint32_t stride, native_handle_t* handle, bool keepOwnership)
 *   -> _ZN7android13GraphicBufferC1EjjijjP13native_handleb
 *
 * but Android 14 libui only exports the 8-argument variant, which adds a layer
 * count (and takes a 32-bit usage):
 *
 *   GraphicBuffer(uint32_t w, uint32_t h, PixelFormat format, uint32_t layerCount,
 *                 uint32_t usage, uint32_t stride, native_handle_t* handle,
 *                 bool keepOwnership)
 *   -> _ZN7android13GraphicBufferC1EjjijjjP13native_handleb
 *
 * Both are Itanium C++ ABI member functions, i.e. the object pointer is the
 * first argument (r0) and it is *not* part of the mangled name. The blob
 * allocates the object itself and then calls the constructor on it, so r0 is
 * `this` and it has to be forwarded as `this`.
 *
 * This used to be declared without `this`, which shifted every argument by one
 * slot: the byte that the blob passed as `stride` (1536, the panel width)
 * became the `this` pointer of the 8-argument constructor, and
 * GraphicBuffer::GraphicBuffer() then wrote its members through address
 * 1536. That is the composer crash loop we chased for days --
 *
 *   #00 libui.so  GraphicBuffer::GraphicBuffer()+24   fault addr 0x608
 *   #01 libui.so  GraphicBuffer(j,j,i,j,j,j,P,b)+12
 *   #02 libshims_ui.so GraphicBuffer(j,j,i,j,j,P,b)+32
 *   #03 hwcomposer.tegra.so create_graphic_buffer+84
 *   #04 hwcomposer.tegra.so glc_set+330
 *
 * 0x608 is exactly stride + offsetof(GraphicBuffer, mBufferMapper)-ish, and
 * the call always came from hwc's GL composition path (glc_set), once per
 * frame, which is why the display, SurfaceFlinger and system_server kept
 * restarting.
 */

extern "C" void _ZN7android13GraphicBufferC1EjjijjjP13native_handleb(
        android::GraphicBuffer* thiz,
        uint32_t inWidth,
        uint32_t inHeight,
        int inFormat,
        uint32_t inLayerCount,
        uint32_t inUsage,
        uint32_t inStride,
        native_handle_t* inHandle,
        bool keepOwnership);

extern "C" void _ZN7android13GraphicBufferC1EjjijjP13native_handleb(
        android::GraphicBuffer* thiz,
        uint32_t inWidth,
        uint32_t inHeight,
        int inFormat,
        uint32_t inUsage,
        uint32_t inStride,
        native_handle_t* inHandle,
        bool keepOwnership)
{
    // The 2015 ABI has no layer count; every buffer the blobs build here is
    // single layered (they are render targets, not multi-layer textures).
    _ZN7android13GraphicBufferC1EjjijjjP13native_handleb(thiz, inWidth, inHeight, inFormat,
            static_cast<uint32_t>(1), inUsage, inStride, inHandle, keepOwnership);
}

/*
 * Second half of the same ABI gap: the object itself.
 *
 * The blobs allocate the GraphicBuffer with the size they were compiled
 * against and only then call the constructor on it:
 *
 *     gb = operator new(128);        // sizeof(GraphicBuffer) on Android 5/6
 *     gb->GraphicBuffer(w, h, ...);  // resolved to the shim above
 *
 * Android 14's GraphicBuffer outgrew that: the constructor is still writing
 * members at offset 136 (mTransportNumFds/Ints, mId, the mDeathCallbacks
 * vector), so roughly a dozen bytes land past the end of a 128-byte
 * allocation and corrupt the next heap chunk. With the `this` bug fixed the
 * crash becomes a slow heap-corruption one instead of an immediate NULL
 * dereference, which is why the slack has to be added as well.
 *
 * Only the legacy GraphicBuffer range is padded -- everything else in the
 * process keeps the stock allocator behaviour, so this cannot mask a real
 * overflow somewhere else. malloc/free instead of ::operator new is
 * deliberate: the blobs were built without exceptions and expect a NULL
 * return, and operator delete(void*) is free() anyway.
 */
extern "C" void* _Znwj(unsigned int size) {
    static constexpr unsigned int kLegacyGraphicBufferMin = 128;
    static constexpr unsigned int kLegacyGraphicBufferMax = 160;
    static constexpr unsigned int kGraphicBufferSlack = 64;

    if (size >= kLegacyGraphicBufferMin && size <= kLegacyGraphicBufferMax) {
        size += kGraphicBufferSlack;
    }
    return malloc(size == 0 ? 1 : size);
}
