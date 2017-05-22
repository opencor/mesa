/*
 * Copyright © 2008-2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

/**
 * @file brw_bufmgr.h
 *
 * Public definitions of Intel-specific bufmgr functions.
 */

#ifndef INTEL_BUFMGR_H
#define INTEL_BUFMGR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "util/list.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct gen_device_info;
struct brw_context;

struct brw_bo {
   /**
    * Size in bytes of the buffer object.
    *
    * The size may be larger than the size originally requested for the
    * allocation, such as being aligned to page size.
    */
   uint64_t size;

   /**
    * Alignment requirement for object
    *
    * Used for GTT mapping & pinning the object.
    */
   uint64_t align;

   /**
    * Virtual address for accessing the buffer data.  Only valid while
    * mapped.
    */
#ifdef __cplusplus
   void *virt;
#else
   void *virtual;
#endif

   /** Buffer manager context associated with this buffer object */
   struct brw_bufmgr *bufmgr;

   /** The GEM handle for this buffer object. */
   uint32_t gem_handle;

   /**
    * Last seen card virtual address (offset from the beginning of the
    * aperture) for the object.  This should be used to fill relocation
    * entries when calling brw_bo_emit_reloc()
    */
   uint64_t offset64;

   /**
    * Boolean of whether the GPU is definitely not accessing the buffer.
    *
    * This is only valid when reusable, since non-reusable
    * buffers are those that have been shared with other
    * processes, so we don't know their state.
    */
   bool idle;

   int refcount;
   const char *name;

   /**
    * Kenel-assigned global name for this object
    *
    * List contains both flink named and prime fd'd objects
    */
   unsigned int global_name;

   /**
    * Current tiling mode
    */
   uint32_t tiling_mode;
   uint32_t swizzle_mode;
   uint32_t stride;

   time_t free_time;

   /** Mapped address for the buffer, saved across map/unmap cycles */
   void *mem_virtual;
   /** GTT virtual address for the buffer, saved across map/unmap cycles */
   void *gtt_virtual;
   /** WC CPU address for the buffer, saved across map/unmap cycles */
   void *wc_virtual;
   int map_count;

   /** BO cache list */
   struct list_head head;

   /**
    * Boolean of whether this buffer can be re-used
    */
   bool reusable;
};

#define BO_ALLOC_FOR_RENDER (1<<0)

/**
 * Allocate a buffer object.
 *
 * Buffer objects are not necessarily initially mapped into CPU virtual
 * address space or graphics device aperture.  They must be mapped
 * using bo_map() or brw_bo_map_gtt() to be used by the CPU.
 */
struct brw_bo *brw_bo_alloc(struct brw_bufmgr *bufmgr, const char *name,
                            uint64_t size, uint64_t alignment);

/**
 * Allocate a tiled buffer object.
 *
 * Alignment for tiled objects is set automatically; the 'flags'
 * argument provides a hint about how the object will be used initially.
 *
 * Valid tiling formats are:
 *  I915_TILING_NONE
 *  I915_TILING_X
 *  I915_TILING_Y
 *
 * Note the tiling format may be rejected; callers should check the
 * 'tiling_mode' field on return, as well as the pitch value, which
 * may have been rounded up to accommodate for tiling restrictions.
 */
struct brw_bo *brw_bo_alloc_tiled(struct brw_bufmgr *bufmgr,
                                  const char *name,
                                  int x, int y, int cpp,
                                  uint32_t tiling_mode,
                                  uint32_t *pitch,
                                  unsigned flags);

/** Takes a reference on a buffer object */
void brw_bo_reference(struct brw_bo *bo);

/**
 * Releases a reference on a buffer object, freeing the data if
 * no references remain.
 */
void brw_bo_unreference(struct brw_bo *bo);

/**
 * Maps the buffer into userspace.
 *
 * This function will block waiting for any existing execution on the
 * buffer to complete, first.  The resulting mapping is available at
 * buf->virtual.
 */
int brw_bo_map(struct brw_context *brw, struct brw_bo *bo, int write_enable);

/**
 * Reduces the refcount on the userspace mapping of the buffer
 * object.
 */
int brw_bo_unmap(struct brw_bo *bo);

/** Write data into an object. */
int brw_bo_subdata(struct brw_bo *bo, uint64_t offset,
                   uint64_t size, const void *data);
/** Read data from an object. */
int brw_bo_get_subdata(struct brw_bo *bo, uint64_t offset,
                       uint64_t size, void *data);
/**
 * Waits for rendering to an object by the GPU to have completed.
 *
 * This is not required for any access to the BO by bo_map,
 * bo_subdata, etc.  It is merely a way for the driver to implement
 * glFinish.
 */
void brw_bo_wait_rendering(struct brw_context *brw, struct brw_bo *bo);

/**
 * Tears down the buffer manager instance.
 */
void brw_bufmgr_destroy(struct brw_bufmgr *bufmgr);

/**
 * Get the current tiling (and resulting swizzling) mode for the bo.
 *
 * \param buf Buffer to get tiling mode for
 * \param tiling_mode returned tiling mode
 * \param swizzle_mode returned swizzling mode
 */
int brw_bo_get_tiling(struct brw_bo *bo, uint32_t *tiling_mode,
                      uint32_t *swizzle_mode);

/**
 * Create a visible name for a buffer which can be used by other apps
 *
 * \param buf Buffer to create a name for
 * \param name Returned name
 */
int brw_bo_flink(struct brw_bo *bo, uint32_t *name);

/**
 * Returns 1 if mapping the buffer for write could cause the process
 * to block, due to the object being active in the GPU.
 */
int brw_bo_busy(struct brw_bo *bo);

/**
 * Specify the volatility of the buffer.
 * \param bo Buffer to create a name for
 * \param madv The purgeable status
 *
 * Use I915_MADV_DONTNEED to mark the buffer as purgeable, and it will be
 * reclaimed under memory pressure. If you subsequently require the buffer,
 * then you must pass I915_MADV_WILLNEED to mark the buffer as required.
 *
 * Returns 1 if the buffer was retained, or 0 if it was discarded whilst
 * marked as I915_MADV_DONTNEED.
 */
int brw_bo_madvise(struct brw_bo *bo, int madv);

/* drm_bacon_bufmgr_gem.c */
struct brw_bufmgr *brw_bufmgr_init(struct gen_device_info *devinfo,
                                   int fd, int batch_size);
struct brw_bo *brw_bo_gem_create_from_name(struct brw_bufmgr *bufmgr,
                                           const char *name,
                                           unsigned int handle);
void brw_bufmgr_enable_reuse(struct brw_bufmgr *bufmgr);
int brw_bo_map_unsynchronized(struct brw_context *brw, struct brw_bo *bo);
int brw_bo_map_gtt(struct brw_context *brw, struct brw_bo *bo);

void *brw_bo_map__cpu(struct brw_bo *bo);
void *brw_bo_map__gtt(struct brw_bo *bo);
void *brw_bo_map__wc(struct brw_bo *bo);

int brw_bo_wait(struct brw_bo *bo, int64_t timeout_ns);

uint32_t brw_create_hw_context(struct brw_bufmgr *bufmgr);
void brw_destroy_hw_context(struct brw_bufmgr *bufmgr, uint32_t ctx_id);

int brw_bo_gem_export_to_prime(struct brw_bo *bo, int *prime_fd);
struct brw_bo *brw_bo_gem_create_from_prime(struct brw_bufmgr *bufmgr,
                                            int prime_fd, int size);

int brw_reg_read(struct brw_bufmgr *bufmgr, uint32_t offset,
                 uint64_t *result);

/** @{ */

#if defined(__cplusplus)
}
#endif
#endif /* INTEL_BUFMGR_H */
