/*******************************************************************************
    Copyright (c) 2024 TUM-DSE / gpu-network-io

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

*******************************************************************************/

#include "uvm_gnio.h"
#include "uvm_global.h"
#include "uvm_gpu.h"
#include "uvm_mem.h"
#include "uvm_va_space.h"
#include "uvm_linux.h"

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/iosys-map.h>

// Process-global ceiling shared across ALL va_spaces (single-tenant research probe). Handles are
// (slot_index | gen << 16): gen bumps every time a slot is freed so a stale handle to a recycled
// slot is rejected instead of silently aliasing the new buffer.
#define UVM_GNIO_MAX_BUFS 256
#define UVM_GNIO_HANDLE_INDEX(h) ((h) & 0xFFFFu)
#define UVM_GNIO_HANDLE_GEN(h)   ((h) >> 16)

struct uvm_gnio_buf_struct
{
    bool in_use;
    NvU32 gen;
    uvm_va_space_t *owner;
    uvm_gpu_t *gpu;
    NvU32 kind;
    NvU64 size;
    uvm_mem_t *mem;
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
    struct iosys_map vmap;
    void *kaddr;
    NvU64 dma_addr;
};

static uvm_gnio_buf_t g_gnio_bufs[UVM_GNIO_MAX_BUFS];
static DEFINE_MUTEX(g_gnio_bufs_lock);

static uvm_gnio_buf_t *gnio_buf_reserve_slot(uvm_va_space_t *va_space, NvU32 *handle_out)
{
    NvU32 i;

    mutex_lock(&g_gnio_bufs_lock);
    for (i = 0; i < UVM_GNIO_MAX_BUFS; i++) {
        if (!g_gnio_bufs[i].in_use) {
            NvU32 gen = g_gnio_bufs[i].gen;   // survives the zeroing below
            memset(&g_gnio_bufs[i], 0, sizeof(g_gnio_bufs[i]));
            g_gnio_bufs[i].gen = gen;
            g_gnio_bufs[i].in_use = true;
            g_gnio_bufs[i].owner = va_space;
            *handle_out = i | ((gen & 0xFFFFu) << 16);
            mutex_unlock(&g_gnio_bufs_lock);
            return &g_gnio_bufs[i];
        }
    }
    mutex_unlock(&g_gnio_bufs_lock);
    return NULL;
}

static void gnio_buf_release_slot(uvm_gnio_buf_t *buf)
{
    mutex_lock(&g_gnio_bufs_lock);
    buf->in_use = false;
    buf->gen++;   // invalidate any outstanding handle to this slot
    mutex_unlock(&g_gnio_bufs_lock);
}

uvm_gnio_buf_t *uvm_gnio_buf_get(uvm_va_space_t *va_space, NvU32 handle)
{
    NvU32 index = UVM_GNIO_HANDLE_INDEX(handle);

    if (index >= UVM_GNIO_MAX_BUFS)
        return NULL;
    if (!g_gnio_bufs[index].in_use || g_gnio_bufs[index].owner != va_space)
        return NULL;
    if ((g_gnio_bufs[index].gen & 0xFFFFu) != UVM_GNIO_HANDLE_GEN(handle))
        return NULL;
    return &g_gnio_bufs[index];
}

uvm_gpu_t *uvm_gnio_buf_gpu(uvm_gnio_buf_t *buf)
{
    return buf ? buf->gpu : NULL;
}

NvU64 uvm_gnio_buf_size(uvm_gnio_buf_t *buf)
{
    return buf ? buf->size : 0;
}

uvm_gpu_address_t uvm_gnio_buf_gpu_address(uvm_gnio_buf_t *buf)
{
    if (buf == NULL)
        return uvm_gpu_address_virtual(0);

    if (buf->kind == UVM_GNIO_KIND_IMPORTED_DMABUF) {
        uvm_gpu_address_t addr = uvm_gpu_address_copy(buf->gpu,
            uvm_gpu_phys_address(UVM_APERTURE_SYS, buf->dma_addr));
        if (g_uvm_global.conf_computing_enabled)
            addr.is_unprotected = true;
        return addr;
    }

    if (buf->mem != NULL)
        return uvm_mem_gpu_address_virtual_kernel(buf->mem, buf->gpu);
    return uvm_gpu_address_virtual(0);
}

void *uvm_gnio_buf_cpu_addr(uvm_gnio_buf_t *buf)
{
    if (buf == NULL)
        return NULL;
    if (buf->kind == UVM_GNIO_KIND_IMPORTED_DMABUF)
        return buf->kaddr;
    if (buf->mem == NULL || buf->kind != UVM_GNIO_KIND_UNPROT_SYSMEM)
        return NULL;
    return uvm_mem_get_cpu_addr_kernel(buf->mem);
}

NV_STATUS uvm_gnio_alloc_vidmem(uvm_gpu_t *gpu, NvU64 size, uvm_mem_t **mem_out)
{
    NV_STATUS status;
    uvm_mem_t *mem;

    status = uvm_mem_alloc_vidmem(size, gpu, &mem);
    if (status != NV_OK)
        return status;

    // Permit user-space (CUDA) GPU mapping of these CPR pages; see vidmem_can_be_mapped.
    mem->is_gnio = true;

    status = uvm_mem_map_gpu_kernel(mem, gpu);
    if (status != NV_OK) {
        uvm_mem_free(mem);
        return status;
    }

    *mem_out = mem;
    return NV_OK;
}

NV_STATUS uvm_gnio_alloc_sysmem(uvm_gpu_t *gpu, NvU64 size, uvm_mem_t **mem_out)
{
    NV_STATUS status;
    uvm_mem_t *mem;

    status = uvm_mem_alloc_sysmem_dma_and_map_cpu_kernel(size, gpu, current->mm, &mem);
    if (status != NV_OK)
        return status;

    status = uvm_mem_map_gpu_kernel(mem, gpu);
    if (status != NV_OK) {
        uvm_mem_free(mem);
        return status;
    }

    *mem_out = mem;
    return NV_OK;
}

void uvm_gnio_free_mem(uvm_mem_t *mem)
{
    if (mem != NULL)
        uvm_mem_free(mem);
}

NV_STATUS uvm_gnio_mem_alloc(uvm_va_space_t *va_space, NvU64 size, NvU32 kind, NvU32 *handle_out)
{
    NV_STATUS status;
    uvm_gpu_t *gpu;
    uvm_mem_t *mem = NULL;
    uvm_gnio_buf_t *buf;

    if (size == 0)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_read(va_space);

    gpu = uvm_va_space_find_first_gpu(va_space);
    if (gpu == NULL) {
        uvm_va_space_up_read(va_space);
        return NV_ERR_INVALID_STATE;
    }

    switch (kind) {
        case UVM_GNIO_KIND_CPR_VIDMEM:
            status = uvm_gnio_alloc_vidmem(gpu, size, &mem);
            break;
        case UVM_GNIO_KIND_UNPROT_SYSMEM:
            status = uvm_gnio_alloc_sysmem(gpu, size, &mem);
            break;
        default:
            status = NV_ERR_INVALID_ARGUMENT;
            break;
    }

    uvm_va_space_up_read(va_space);

    if (status != NV_OK)
        return status;

    buf = gnio_buf_reserve_slot(va_space, handle_out);
    if (buf == NULL) {
        uvm_mem_free(mem);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    buf->gpu = gpu;
    buf->kind = kind;
    buf->size = size;
    buf->mem = mem;

    return NV_OK;
}

NV_STATUS uvm_gnio_mem_import_dmabuf(uvm_va_space_t *va_space, int dmabuf_fd, NvU64 size, NvU32 *handle_out)
{
    NV_STATUS status;
    uvm_gpu_t *gpu;
    uvm_gnio_buf_t *buf;
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;

    if (size == 0 || dmabuf_fd < 0)
        return NV_ERR_INVALID_ARGUMENT;

    dmabuf = dma_buf_get(dmabuf_fd);
    if (IS_ERR(dmabuf))
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_read(va_space);

    gpu = uvm_va_space_find_first_gpu(va_space);
    if (gpu == NULL) {
        status = NV_ERR_INVALID_STATE;
        goto err_put;
    }

    attach = dma_buf_attach(dmabuf, &gpu->parent->pci_dev->dev);
    if (IS_ERR(attach)) {
        status = NV_ERR_OPERATING_SYSTEM;
        goto err_put;
    }

    sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
    if (IS_ERR(sgt)) {
        status = NV_ERR_OPERATING_SYSTEM;
        goto err_detach;
    }

    // The bench/copy paths address the buffer as one flat sysmem region, so the
    // coherent allocation must map as a single contiguous DMA segment.
    if (sgt->nents != 1) {
        status = NV_ERR_NOT_SUPPORTED;
        goto err_unmap;
    }

    buf = gnio_buf_reserve_slot(va_space, handle_out);
    if (buf == NULL) {
        status = NV_ERR_INSUFFICIENT_RESOURCES;
        goto err_unmap;
    }

    buf->gpu = gpu;
    buf->kind = UVM_GNIO_KIND_IMPORTED_DMABUF;
    buf->size = size;
    buf->mem = NULL;
    buf->dmabuf = dmabuf;
    buf->attach = attach;
    buf->sgt = sgt;
    buf->dma_addr = sg_dma_address(sgt->sgl);

    // Both sealed directions touch this buffer from the CPU: HtoD reads its
    // plaintext for cpu_encrypt staging, and DtoH logs the per-page IV into it.
    // Map a CPU view (dma_heap_coh is coherent sysmem, so this always succeeds).
    iosys_map_clear(&buf->vmap);
    buf->kaddr = NULL;
    if (dma_buf_vmap_unlocked(dmabuf, &buf->vmap) == 0 && !buf->vmap.is_iomem)
        buf->kaddr = buf->vmap.vaddr;

    uvm_va_space_up_read(va_space);
    return NV_OK;

err_unmap:
    dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
err_detach:
    dma_buf_detach(dmabuf, attach);
err_put:
    uvm_va_space_up_read(va_space);
    dma_buf_put(dmabuf);
    return status;
}

static void gnio_buf_destroy(uvm_gnio_buf_t *buf)
{
    if (buf->kind == UVM_GNIO_KIND_IMPORTED_DMABUF) {
        if (buf->kaddr != NULL) {
            dma_buf_vunmap_unlocked(buf->dmabuf, &buf->vmap);
            buf->kaddr = NULL;
        }
        if (buf->sgt != NULL)
            dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt, DMA_BIDIRECTIONAL);
        if (buf->attach != NULL)
            dma_buf_detach(buf->dmabuf, buf->attach);
        if (buf->dmabuf != NULL)
            dma_buf_put(buf->dmabuf);
        buf->sgt = NULL;
        buf->attach = NULL;
        buf->dmabuf = NULL;
        return;
    }

    if (buf->mem != NULL) {
        uvm_mem_free(buf->mem);
        buf->mem = NULL;
    }
}

NV_STATUS uvm_gnio_mem_free(uvm_va_space_t *va_space, NvU32 handle)
{
    uvm_gnio_buf_t *buf = uvm_gnio_buf_get(va_space, handle);

    if (buf == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    gnio_buf_destroy(buf);
    gnio_buf_release_slot(buf);
    return NV_OK;
}

// Map a GNIO buffer into the caller's user (CUDA) GPU VA space at user_va, so a CUDA
// kernel can write it. The buffer keeps its existing kernel-VA mapping for the CE, so the
// same physical CPR pages are reachable by both the SM (user VA) and the engine (kernel VA).
NV_STATUS uvm_gnio_mem_map_user(uvm_va_space_t *va_space, NvU32 handle, NvU64 user_va)
{
    uvm_gnio_buf_t *buf;
    NV_STATUS status;
    uvm_mem_gpu_mapping_attrs_t attrs = {
        .protection = UVM_PROT_READ_WRITE_ATOMIC,
        .is_cacheable = true,
    };

    uvm_va_space_down_write(va_space);

    buf = uvm_gnio_buf_get(va_space, handle);
    if (buf == NULL || buf->mem == NULL) {
        uvm_va_space_up_write(va_space);
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = uvm_mem_map_gpu_user(buf->mem, buf->gpu, va_space, (void *)user_va, &attrs);

    uvm_va_space_up_write(va_space);
    return status;
}

NV_STATUS uvm_gnio_mem_unmap_user(uvm_va_space_t *va_space, NvU32 handle)
{
    uvm_gnio_buf_t *buf;

    uvm_va_space_down_write(va_space);

    buf = uvm_gnio_buf_get(va_space, handle);
    if (buf == NULL || buf->mem == NULL) {
        uvm_va_space_up_write(va_space);
        return NV_ERR_INVALID_ARGUMENT;
    }

    uvm_mem_unmap_gpu_user(buf->mem, buf->gpu);

    uvm_va_space_up_write(va_space);
    return NV_OK;
}

void uvm_gnio_mem_free_all(uvm_va_space_t *va_space)
{
    NvU32 i;

    for (i = 0; i < UVM_GNIO_MAX_BUFS; i++) {
        uvm_gnio_buf_t *buf = &g_gnio_bufs[i];
        if (buf->in_use && buf->owner == va_space) {
            gnio_buf_destroy(buf);
            gnio_buf_release_slot(buf);
        }
    }
}
