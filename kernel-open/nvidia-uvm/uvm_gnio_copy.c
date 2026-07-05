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

    GNIO single-shot copies for perftest and general use. Sealed DtoH leaves the
    CE ciphertext in the destination dmabuf without decrypting, so a NIC can DMA it
    straight out; the per-page IV and GCM auth tag are written into the same dmabuf
    right after the ciphertext (layout: uvm_gnio_sealed_dtoh_buf_bytes), making the
    buffer a self-describing blob with no separate sysmem scratch. Sealed HtoD
    reuses UVM's CPU-encrypt + CE-decrypt ingress util. DtoD is a plaintext
    CPR->CPR CE copy (PROT2PROT). Plaintext DtoH/HtoD cross the CPR boundary in the
    clear and are only valid when confidential computing is disabled.

*******************************************************************************/

#include "uvm_gnio.h"
#include "uvm_global.h"
#include "uvm_gpu.h"
#include "uvm_mem.h"
#include "uvm_channel.h"
#include "uvm_push.h"
#include "uvm_hal.h"
#include "uvm_conf_computing.h"
#include "uvm_va_space.h"
#include "uvm_linux.h"

static NV_STATUS gnio_copy_dtoh_sealed(uvm_gpu_t *gpu, uvm_gnio_buf_t *src, uvm_gnio_buf_t *dst,
                                       size_t size, UVM_GNIO_COPY_PARAMS *params)
{
    uvm_push_t push;
    uvm_gpu_address_t dst_addr, tag_addr;
    NvU8 *dst_cpu;
    UvmCslIv *iv;
    NvU64 tag_off, iv_off;
    NV_STATUS status;

    BUILD_BUG_ON(UVM_GNIO_PAGE_SIZE != PAGE_SIZE);
    BUILD_BUG_ON(UVM_GNIO_AUTH_TAG_SIZE != UVM_CONF_COMPUTING_AUTH_TAG_SIZE);
    BUILD_BUG_ON(UVM_GNIO_IV_SIZE != sizeof(UvmCslIv));

    // The destination dmabuf carries the ciphertext plus the tag/IV sidecar: the
    // CE writes ciphertext + tag into it (physical+SYS, like the ciphertext) and
    // the CPU logs the IVs into the same buffer. No separate sysmem tag scratch is
    // allocated, so there is no per-page PSC hypercall on the copy path.
    if (uvm_gnio_buf_size(dst) < uvm_gnio_sealed_dtoh_buf_bytes(size))
        return NV_ERR_INVALID_ARGUMENT;

    dst_cpu = uvm_gnio_buf_cpu_addr(dst);
    if (dst_cpu == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    tag_off = uvm_gnio_seal_tag_off(size);
    iv_off = uvm_gnio_seal_iv_off(size);

    dst_addr = uvm_gnio_buf_gpu_address(dst);
    tag_addr = dst_addr;
    tag_addr.address += tag_off;
    iv = (UvmCslIv *)(dst_cpu + iv_off);

    status = uvm_push_begin(gpu->channel_manager, UVM_CHANNEL_TYPE_GPU_TO_CPU, &push, "gnio dtoh sealed");
    if (status != NV_OK)
        return status;

    uvm_conf_computing_log_gpu_encryption(push.channel, size, iv);
    gpu->parent->ce_hal->encrypt(&push, dst_addr, uvm_gnio_buf_gpu_address(src), size, tag_addr);

    return uvm_push_end_and_wait(&push);
}

static NV_STATUS gnio_copy_htod_sealed(uvm_gpu_t *gpu, uvm_gnio_buf_t *src, uvm_gnio_buf_t *dst, size_t size)
{
    void *src_plain = uvm_gnio_buf_cpu_addr(src);

    if (src_plain == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    return uvm_conf_computing_util_memcopy_cpu_to_gpu(gpu,
                                                      uvm_gnio_buf_gpu_address(dst),
                                                      src_plain,
                                                      size,
                                                      NULL,
                                                      "gnio htod sealed");
}

static NV_STATUS gnio_copy_memcopy(uvm_gpu_t *gpu, uvm_gnio_buf_t *src, uvm_gnio_buf_t *dst,
                                   size_t size, uvm_channel_type_t type)
{
    uvm_push_t push;
    NV_STATUS status;

    status = uvm_push_begin(gpu->channel_manager, type, &push, "gnio memcopy");
    if (status != NV_OK)
        return status;

    gpu->parent->ce_hal->memcopy(&push, uvm_gnio_buf_gpu_address(dst), uvm_gnio_buf_gpu_address(src), size);

    return uvm_push_end_and_wait(&push);
}

NV_STATUS uvm_gnio_copy(uvm_va_space_t *va_space, UVM_GNIO_COPY_PARAMS *params)
{
    uvm_gnio_buf_t *dst = uvm_gnio_buf_get(va_space, params->dst_handle);
    uvm_gnio_buf_t *src = uvm_gnio_buf_get(va_space, params->src_handle);
    size_t size = params->size;
    uvm_gpu_t *gpu;
    NV_STATUS status;

    if (dst == NULL || src == NULL || size == 0)
        return NV_ERR_INVALID_ARGUMENT;

    gpu = uvm_gnio_buf_gpu(src);

    uvm_va_space_down_read(va_space);

    switch (params->kind) {
        case UVM_GNIO_COPY_DTOH_SEALED:
            status = gnio_copy_dtoh_sealed(gpu, src, dst, size, params);
            break;
        case UVM_GNIO_COPY_HTOD_SEALED:
            status = gnio_copy_htod_sealed(gpu, src, dst, size);
            break;
        case UVM_GNIO_COPY_DTOD:
            status = gnio_copy_memcopy(gpu, src, dst, size, UVM_CHANNEL_TYPE_GPU_INTERNAL);
            break;
        case UVM_GNIO_COPY_DTOH:
            status = g_uvm_global.conf_computing_enabled ? NV_ERR_NOT_SUPPORTED
                   : gnio_copy_memcopy(gpu, src, dst, size, UVM_CHANNEL_TYPE_GPU_TO_CPU);
            break;
        case UVM_GNIO_COPY_HTOD:
            status = g_uvm_global.conf_computing_enabled ? NV_ERR_NOT_SUPPORTED
                   : gnio_copy_memcopy(gpu, src, dst, size, UVM_CHANNEL_TYPE_CPU_TO_GPU);
            break;
        // force plain CE memcopy across boundary under CC -> should fault
        case UVM_GNIO_COPY_DTOH_PLAIN:
            status = gnio_copy_memcopy(gpu, src, dst, size, UVM_CHANNEL_TYPE_GPU_TO_CPU);
            break;
        case UVM_GNIO_COPY_HTOD_PLAIN:
            status = gnio_copy_memcopy(gpu, src, dst, size, UVM_CHANNEL_TYPE_CPU_TO_GPU);
            break;
        default:
            status = NV_ERR_INVALID_ARGUMENT;
            break;
    }

    uvm_va_space_up_read(va_space);
    return status;
}
