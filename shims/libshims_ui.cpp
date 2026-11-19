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
 * Note on the object size, since the obvious patch here is wrong.
 *
 * The blobs allocate the GraphicBuffer with `operator new(128)` (see
 * create_graphic_buffer) while Android 14's constructor still initialises a
 * member at offset 136. Padding the allocation by overriding _Znwj in this
 * library was tried and had to be reverted: the shim is preloaded into
 * hwcomposer.tegra.so, so the override covers *every* C++ allocation in that
 * process, and the composer then failed to initialise and exited with
 * status 1. init counts exits of an updatable component and, after four in
 * four minutes, sets sys.init.updatable_crashing, which apexd/apex turns into
 * a userspace reboot -- the device sat on the boot animation forever.
 *
 * With the override removed the composer runs for minutes without a single
 * tombstone, so the overflow either does not happen on this path or lands in
 * the allocator's slack. If random heap corruption ever shows up in the
 * composer again, this is the first thing to look at -- but the fix has to be
 * scoped to the blob (for example by handing it a larger block from a
 * wrapper), not applied to the whole process.
 */
