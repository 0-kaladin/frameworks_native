/* 
**
** Copyright 2009, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing, software 
** distributed under the License is distributed on an "AS IS" BASIS, 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#include <sys/mman.h>
#include <cutils/ashmem.h>
#include <cutils/log.h>

#include <utils/Singleton.h>
#include <utils/String8.h>

#include "BufferAllocator.h"


namespace android {
// ---------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE( BufferAllocator )

Mutex BufferAllocator::sLock;
KeyedVector<buffer_handle_t, BufferAllocator::alloc_rec_t> BufferAllocator::sAllocList;

BufferAllocator::BufferAllocator()
    : mAllocDev(0)
{
    hw_module_t const* module;
    int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
    LOGE_IF(err, "FATAL: can't find the %s module", GRALLOC_HARDWARE_MODULE_ID);
    if (err == 0) {
        gralloc_open(module, &mAllocDev);
    }
}

BufferAllocator::~BufferAllocator()
{
    gralloc_close(mAllocDev);
}

void BufferAllocator::dump(String8& result) const
{
    Mutex::Autolock _l(sLock);
    KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
    size_t total = 0;
    const size_t SIZE = 512;
    char buffer[SIZE];
    snprintf(buffer, SIZE, "Allocated buffers:\n");
    result.append(buffer);
    const size_t c = list.size();
    for (size_t i=0 ; i<c ; i++) {
        const alloc_rec_t& rec(list.valueAt(i));
        snprintf(buffer, SIZE, "%10p: %7.2f KiB | %4u x %4u | %2d | 0x%08x\n",
            list.keyAt(i), rec.size/1024.0f, 
            rec.w, rec.h, rec.format, rec.usage);
        result.append(buffer);
        total += rec.size;
    }
    snprintf(buffer, SIZE, "Total allocated: %.2f KB\n", total/1024.0f);
    result.append(buffer);
}

status_t BufferAllocator::alloc(uint32_t w, uint32_t h, PixelFormat format,
        int usage, buffer_handle_t* handle, int32_t* stride)
{
    Mutex::Autolock _l(mLock);
    
    // we have a h/w allocator and h/w buffer is requested
    status_t err = mAllocDev->alloc(mAllocDev,
            w, h, format, usage, handle, stride);
    LOGW_IF(err, "alloc(%u, %u, %d, %08x, ...) failed %d (%s)",
            w, h, format, usage, err, strerror(-err));
    
    if (err == NO_ERROR) {
        Mutex::Autolock _l(sLock);
        KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
        alloc_rec_t rec;
        rec.w = w;
        rec.h = h;
        rec.format = format;
        rec.usage = usage;
        rec.vaddr = 0;
        rec.size = h * stride[0] * bytesPerPixel(format);
        list.add(*handle, rec);
    }

    return err;
}

status_t BufferAllocator::free(buffer_handle_t handle)
{
    Mutex::Autolock _l(mLock);

    status_t err = mAllocDev->free(mAllocDev, handle);
    LOGW_IF(err, "free(...) failed %d (%s)", err, strerror(-err));
    
    if (err == NO_ERROR) {
        Mutex::Autolock _l(sLock);
        KeyedVector<buffer_handle_t, alloc_rec_t>& list(sAllocList);
        list.removeItem(handle);
    }

    return err;
}

// ---------------------------------------------------------------------------
}; // namespace android
