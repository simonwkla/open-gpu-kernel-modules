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

#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/scatterlist.h>

#define UVM_GNIO_MAX_BUFS 256

struct uvm_gnio_buf_struct
{
    bool in_use;
    uvm_va_space_t *owner;
    uvm_gpu_t *gpu;
    uvm_gnio_buf_kind_t kind;
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

static uvm_gnio_buf_t *gnio_buf_alloc(uvm_va_space_t *va_space, NvU32 *handle_out)
{
    NvU32 i;

    for (i = 0; i < UVM_GNIO_MAX_BUFS; i++) {
        if (!g_gnio_bufs[i].in_use) {
            memset(&g_gnio_bufs[i], 0, sizeof(g_gnio_bufs[i]));
            g_gnio_bufs[i].in_use = true;
            g_gnio_bufs[i].owner = va_space;
            *handle_out = i;
            return &g_gnio_bufs[i];
        }
    }

    return NULL;
}

uvm_gnio_buf_t *uvm_gnio_buf_get(uvm_va_space_t *va_space, NvU32 handle)
{
    if (handle >= UVM_GNIO_MAX_BUFS || !g_gnio_bufs[handle].in_use ||
        g_gnio_bufs[handle].owner != va_space)
        return NULL;

    return &g_gnio_bufs[handle];
}

uvm_gpu_t *uvm_gnio_buf_gpu(uvm_gnio_buf_t *buf)
{
    return buf->gpu;
}

NvU64 uvm_gnio_buf_size(uvm_gnio_buf_t *buf)
{
    return buf->size;
}

NvU32 uvm_gnio_buf_kind(uvm_gnio_buf_t *buf)
{
    return buf->kind;
}

uvm_gpu_address_t uvm_gnio_buf_gpu_address(uvm_gnio_buf_t *buf)
{
    if (buf->kind == UVM_GNIO_BUF_EXPOSURE) {
        uvm_gpu_address_t address = uvm_gpu_address_copy(
            buf->gpu,
            uvm_gpu_phys_address(UVM_APERTURE_SYS, buf->dma_addr));

        if (g_uvm_global.conf_computing_enabled)
            address.is_unprotected = true;
        return address;
    }

    return uvm_mem_gpu_address_virtual_kernel(buf->mem, buf->gpu);
}

void *uvm_gnio_buf_cpu_addr(uvm_gnio_buf_t *buf)
{
    return buf->kind == UVM_GNIO_BUF_EXPOSURE ? buf->kaddr : NULL;
}

static NV_STATUS gnio_alloc_protected(uvm_gpu_t *gpu, NvU64 size, uvm_mem_t **mem_out)
{
    NV_STATUS status;
    uvm_mem_t *mem;

    status = uvm_mem_alloc_vidmem(size, gpu, &mem);
    if (status != NV_OK)
        return status;

    mem->is_gnio = true;
    status = uvm_mem_map_gpu_kernel(mem, gpu);
    if (status != NV_OK) {
        uvm_mem_free(mem);
        return status;
    }

    *mem_out = mem;
    return NV_OK;
}

NV_STATUS uvm_gnio_mem_alloc_protected(uvm_va_space_t *va_space, NvU64 size, NvU32 *handle_out)
{
    uvm_gpu_t *gpu;
    uvm_mem_t *mem;
    uvm_gnio_buf_t *buf;
    NV_STATUS status;

    if (size == 0)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_read(va_space);
    gpu = uvm_va_space_find_first_gpu(va_space);
    status = gpu ? gnio_alloc_protected(gpu, size, &mem) : NV_ERR_INVALID_STATE;
    if (status == NV_OK) {
        buf = gnio_buf_alloc(va_space, handle_out);
        if (buf == NULL) {
            uvm_mem_free(mem);
            status = NV_ERR_INSUFFICIENT_RESOURCES;
        }
        else {
            buf->gpu = gpu;
            buf->kind = UVM_GNIO_BUF_PROTECTED;
            buf->size = size;
            buf->mem = mem;
        }
    }
    uvm_va_space_up_read(va_space);
    return status;
}

NV_STATUS uvm_gnio_mem_import_exposure(uvm_va_space_t *va_space, int fd, NvU64 size, NvU32 *handle_out)
{
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
    struct iosys_map vmap;
    uvm_gnio_buf_t *buf;
    uvm_gpu_t *gpu;
    NvU64 dma_addr;
    NV_STATUS status;

    if (size == 0 || fd < 0)
        return NV_ERR_INVALID_ARGUMENT;

    dmabuf = dma_buf_get(fd);
    if (IS_ERR(dmabuf) || size > dmabuf->size)
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

    dma_addr = sg_dma_address(sgt->sgl);
    if (sgt->nents != 1 || size > sg_dma_len(sgt->sgl) ||
        dma_addr < gpu->parent->dma_addressable_start ||
        dma_addr > gpu->parent->dma_addressable_limit ||
        size - 1 > gpu->parent->dma_addressable_limit - dma_addr) {
        status = NV_ERR_INVALID_ADDRESS;
        goto err_unmap;
    }

    iosys_map_clear(&vmap);
    if (dma_buf_vmap_unlocked(dmabuf, &vmap) != 0 || vmap.is_iomem) {
        status = NV_ERR_NOT_SUPPORTED;
        goto err_unmap;
    }

    buf = gnio_buf_alloc(va_space, handle_out);
    if (buf == NULL) {
        status = NV_ERR_INSUFFICIENT_RESOURCES;
        goto err_vunmap;
    }

    buf->gpu = gpu;
    buf->kind = UVM_GNIO_BUF_EXPOSURE;
    buf->size = size;
    buf->dmabuf = dmabuf;
    buf->attach = attach;
    buf->sgt = sgt;
    buf->vmap = vmap;
    buf->kaddr = vmap.vaddr;
    buf->dma_addr = uvm_parent_gpu_dma_addr_to_gpu_addr(gpu->parent, dma_addr);

    uvm_va_space_up_read(va_space);
    return NV_OK;

err_vunmap:
    dma_buf_vunmap_unlocked(dmabuf, &vmap);
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
    if (buf->kind == UVM_GNIO_BUF_EXPOSURE) {
        dma_buf_vunmap_unlocked(buf->dmabuf, &buf->vmap);
        dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt, DMA_BIDIRECTIONAL);
        dma_buf_detach(buf->dmabuf, buf->attach);
        dma_buf_put(buf->dmabuf);
    }
    else {
        uvm_mem_free(buf->mem);
    }

    memset(buf, 0, sizeof(*buf));
}

NV_STATUS uvm_gnio_mem_free(uvm_va_space_t *va_space, NvU32 handle)
{
    uvm_gnio_buf_t *buf;

    uvm_va_space_down_write(va_space);
    buf = uvm_gnio_buf_get(va_space, handle);
    if (buf != NULL)
        gnio_buf_destroy(buf);
    uvm_va_space_up_write(va_space);

    return buf ? NV_OK : NV_ERR_INVALID_ARGUMENT;
}

NV_STATUS uvm_gnio_mem_map_protected(uvm_va_space_t *va_space, NvU32 handle, NvU64 user_va)
{
    uvm_mem_gpu_mapping_attrs_t attrs = {
        .protection = UVM_PROT_READ_WRITE_ATOMIC,
        .is_cacheable = true,
    };
    uvm_gnio_buf_t *buf;
    NV_STATUS status;

    uvm_va_space_down_write(va_space);
    buf = uvm_gnio_buf_get(va_space, handle);
    status = (!buf || buf->kind != UVM_GNIO_BUF_PROTECTED) ? NV_ERR_INVALID_ARGUMENT :
             uvm_mem_map_gpu_user(buf->mem, buf->gpu, va_space, (void *)user_va, &attrs);
    uvm_va_space_up_write(va_space);
    return status;
}

NV_STATUS uvm_gnio_mem_unmap_protected(uvm_va_space_t *va_space, NvU32 handle)
{
    uvm_gnio_buf_t *buf;

    uvm_va_space_down_write(va_space);
    buf = uvm_gnio_buf_get(va_space, handle);
    if (buf && buf->kind == UVM_GNIO_BUF_PROTECTED)
        uvm_mem_unmap_gpu_user(buf->mem, buf->gpu);
    uvm_va_space_up_write(va_space);

    return buf && buf->kind == UVM_GNIO_BUF_PROTECTED ? NV_OK : NV_ERR_INVALID_ARGUMENT;
}

void uvm_gnio_mem_free_all(uvm_va_space_t *va_space)
{
    NvU32 i;

    for (i = 0; i < UVM_GNIO_MAX_BUFS; i++) {
        if (g_gnio_bufs[i].in_use && g_gnio_bufs[i].owner == va_space)
            gnio_buf_destroy(&g_gnio_bufs[i]);
    }
}
