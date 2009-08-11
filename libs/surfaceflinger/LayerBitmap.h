/*
 * Copyright (C) 2007 The Android Open Source Project
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

#ifndef ANDROID_LAYER_BITMAP_H
#define ANDROID_LAYER_BITMAP_H

#include <stdint.h>
#include <sys/types.h>

#include <hardware/gralloc.h>

#include <utils/Atomic.h>

#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Surface.h>

#include <pixelflinger/pixelflinger.h>

#include <private/ui/SharedState.h>
#include <private/ui/SurfaceBuffer.h>

class copybit_image_t;
struct android_native_buffer_t;

namespace android {

// ---------------------------------------------------------------------------
class IMemory;
class LayerBitmap;

// ===========================================================================
// Buffer
// ===========================================================================

class NativeBuffer;

class Buffer : public SurfaceBuffer
{
public:
    enum {
        DONT_CLEAR  = 0x00000001,
        GPU         = 0x00000002,
        SECURE      = 0x00000004
    };

    // creates w * h buffer
    Buffer(uint32_t w, uint32_t h, PixelFormat format,
            uint32_t reqUsage, uint32_t flags = 0);

    // return status
    status_t initCheck() const;

    uint32_t getWidth() const           { return width; }
    uint32_t getHeight() const          { return height; }
    uint32_t getStride() const          { return stride; }
    uint32_t getUsage() const           { return usage; }
    PixelFormat getPixelFormat() const  { return format; }
    Rect getBounds() const              { return Rect(width, height); }
    
    status_t lock(GGLSurface* surface, uint32_t usage);
    
    android_native_buffer_t* getNativeBuffer() const;
    
private:
    friend class LightRefBase<Buffer>;
    Buffer(const Buffer& rhs);
    virtual ~Buffer();
    Buffer& operator = (const Buffer& rhs);
    const Buffer& operator = (const Buffer& rhs) const;

    status_t initSize(uint32_t w, uint32_t h, uint32_t reqUsage);

    ssize_t                 mInitCheck;
    uint32_t                mFlags;
    uint32_t                mVStride;
};

// ===========================================================================
// LayerBitmap
// ===========================================================================

class LayerBitmap
{
public:
    enum {
        DONT_CLEAR  = Buffer::DONT_CLEAR,
        GPU         = Buffer::GPU,
        SECURE      = Buffer::SECURE
    };
    LayerBitmap();
    ~LayerBitmap();

    status_t init(surface_info_t* info,
            uint32_t w, uint32_t h, PixelFormat format, uint32_t flags = 0);

    status_t setSize(uint32_t w, uint32_t h);

    sp<Buffer> allocate(uint32_t reqUsage);
    status_t free();
    
    sp<const Buffer>  getBuffer() const { return mBuffer; }
    sp<Buffer>        getBuffer()       { return mBuffer; }
    
    uint32_t getWidth() const           { return mWidth; }
    uint32_t getHeight() const          { return mHeight; }
    PixelFormat getPixelFormat() const  { return mBuffer->getPixelFormat(); }
    Rect getBounds() const              { return mBuffer->getBounds(); }
    
private:
    surface_info_t* mInfo;
    sp<Buffer>      mBuffer;
    uint32_t        mWidth;
    uint32_t        mHeight;
    PixelFormat     mFormat;
    uint32_t        mFlags;
    // protects setSize() and allocate()
    mutable Mutex   mLock;
};

}; // namespace android

#endif // ANDROID_LAYER_BITMAP_H
