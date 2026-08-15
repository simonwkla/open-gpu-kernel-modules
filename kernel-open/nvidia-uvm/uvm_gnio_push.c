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

#include "uvm_extern_decl.h"
#include "uvm_forward_decl.h"
#include "uvm_gnio.h"
#include "uvm_channel.h"
#include "uvm_conf_computing.h"
#include "uvm_global.h"
#include "uvm_gnio_abi.h"
#include "uvm_gpu.h"
#include "uvm_hal.h"
#include "uvm_mem.h"
#include "uvm_push.h"
#include "uvm_va_space.h"


typedef struct
{
    uvm_gpu_t *gpu;
    uvm_channel_type_t channel_type;
    NvU32 key_version;
} gnio_push_context_t;

static uvm_gpu_address_t gnio_address(uvm_gnio_buf_t *buffer, NvU64 offset)
{
    uvm_gpu_address_t address = uvm_gnio_buf_gpu_address(buffer);
    address.address += offset;
    return address;
}


static NV_STATUS gnio_channel_type(uvm_va_space_t *va_space,
                                   const UVM_GNIO_SUBMIT_PARAMS *params,
                                   uvm_gpu_t **gpu_out,
                                   uvm_channel_type_t *type_out)
{
    uvm_gpu_t *gpu = uvm_va_space_find_first_gpu(va_space);
    uvm_channel_type_t type = UVM_CHANNEL_TYPE_GPU_INTERNAL;
    NvU32 i;

    if (gpu == NULL)
        return NV_ERR_INVALID_STATE;

    for (i = 0; i < params->op_count; i++) {
        const UVM_GNIO_PUSH_OP *op = &params->ops[i];
        uvm_channel_type_t op_type = type;
        uvm_gnio_buf_t *src;
        uvm_gnio_buf_t *dst;

        if (op->type == UVM_GNIO_OP_SEAL || op->type == UVM_GNIO_OP_DECRYPT) {
            if (!g_uvm_global.conf_computing_enabled)
                return NV_ERR_NOT_SUPPORTED;
            op_type = UVM_CHANNEL_TYPE_GPU_TO_CPU;
        }

        else if (op->type == UVM_GNIO_OP_UNSEAL || op->type == UVM_GNIO_OP_ENCRYPT) {
            if (!g_uvm_global.conf_computing_enabled)
                return NV_ERR_NOT_SUPPORTED;
            op_type = UVM_CHANNEL_TYPE_CPU_TO_GPU;
        }
        else if (op->type == UVM_GNIO_OP_COPY) {
            src = uvm_gnio_buf_get(va_space, op->source_handle);
            dst = uvm_gnio_buf_get(va_space, op->destination_handle);
            if (uvm_gnio_buf_kind(src) == UVM_GNIO_BUF_PROTECTED && uvm_gnio_buf_kind(dst) == UVM_GNIO_BUF_EXPOSURE)
                op_type = UVM_CHANNEL_TYPE_GPU_TO_CPU;
            else if (uvm_gnio_buf_kind(src) == UVM_GNIO_BUF_EXPOSURE && uvm_gnio_buf_kind(dst) == UVM_GNIO_BUF_PROTECTED)
                op_type = UVM_CHANNEL_TYPE_CPU_TO_GPU;
            else
                op_type = UVM_CHANNEL_TYPE_GPU_INTERNAL;
        }
        else {
            continue;
        }

        if (type != UVM_CHANNEL_TYPE_GPU_INTERNAL && op_type != type)
            return NV_ERR_INVALID_ARGUMENT;
        type = op_type;
    }

    *gpu_out = gpu;
    *type_out = type;
    return NV_OK;
}



static void gnio_preprocess(uvm_va_space_t *va_space,
                            uvm_push_t *push,
                            const UVM_GNIO_PUSH_OP *op)
{
    uvm_gnio_buf_t *src = uvm_gnio_buf_get(va_space, op->source_handle);
    uvm_gnio_buf_t *dst = uvm_gnio_buf_get(va_space, op->destination_handle);

    switch (op->type) {
        case UVM_GNIO_OP_ENCRYPT:
        {
            NvU8 *plain = (NvU8 *)uvm_gnio_buf_cpu_addr(src) + op->source_offset;
            NvU8 *cipher = (NvU8 *)uvm_gnio_buf_cpu_addr(dst) + op->destination_offset;
            void *auth_tag = cipher + uvm_gnio_record_tag_off(op->size);

            uvm_conf_computing_cpu_encrypt(push->channel,
                                           cipher,
                                           plain,
                                           NULL,
                                           op->size,
                                           auth_tag);
            break;
        }
    }
}

static NV_STATUS gnio_postprocess(uvm_va_space_t *va_space,
                                  gnio_push_context_t *context,
                                  uvm_push_t *push,
                                  const UVM_GNIO_PUSH_OP *op)
{
    uvm_gnio_buf_t *src = uvm_gnio_buf_get(va_space, op->source_handle);
    uvm_gnio_buf_t *dst = uvm_gnio_buf_get(va_space, op->destination_handle);

    switch (op->type) {
        case UVM_GNIO_OP_DECRYPT:
        {
            NvU8 *plain = (NvU8 *)uvm_gnio_buf_cpu_addr(dst) + op->destination_offset;
            NvU8 *cipher = (NvU8 *)uvm_gnio_buf_cpu_addr(src) + op->source_offset;
            void *auth_tag = cipher + uvm_gnio_record_tag_off(op->size);
            UvmCslIv *iv = (UvmCslIv *)(cipher + uvm_gnio_record_iv_off(op->size));

            return uvm_conf_computing_cpu_decrypt(push->channel,
                                                  plain,
                                                  cipher,
                                                  iv,
                                                  context->key_version,
                                                  op->size,
                                                  auth_tag);
        }
    }

    return NV_OK;
}

static void gnio_emit(uvm_va_space_t *va_space,
                      gnio_push_context_t *context,
                      uvm_push_t *push,
                      const UVM_GNIO_PUSH_OP *op)
{
    uvm_gnio_buf_t *src = uvm_gnio_buf_get(va_space, op->source_handle);
    uvm_gnio_buf_t *dst = uvm_gnio_buf_get(va_space, op->destination_handle);

    switch (op->type) {
        case UVM_GNIO_OP_SEMAPHORE_ACQUIRE:
            context->gpu->parent->host_hal->semaphore_acquire(
                push,
                gnio_address(src, op->source_offset).address,
                op->value);
            break;

        case UVM_GNIO_OP_SEMAPHORE_RELEASE:
            context->gpu->parent->ce_hal->semaphore_release(
                push,
                gnio_address(src, op->source_offset).address,
                op->value);
            break;

        case UVM_GNIO_OP_SEAL:
        {
            NvU8 *cipher = (NvU8 *)uvm_gnio_buf_cpu_addr(dst) + op->destination_offset;
            UvmCslIv *iv = (UvmCslIv *)(cipher + uvm_gnio_record_iv_off(op->size));

            uvm_conf_computing_log_gpu_encryption(push->channel, op->size, iv);
            context->key_version = uvm_channel_pool_key_version(push->channel->pool);
            context->gpu->parent->ce_hal->encrypt(
                push,
                gnio_address(dst, op->destination_offset),
                gnio_address(src, op->source_offset),
                (NvU32)op->size,
                gnio_address(dst, op->destination_offset + uvm_gnio_record_tag_off(op->size)));
            break;
        }

        case UVM_GNIO_OP_UNSEAL:
            context->gpu->parent->ce_hal->decrypt(
                push,
                gnio_address(dst, op->destination_offset),
                gnio_address(src, op->source_offset),
                (NvU32)op->size,
                gnio_address(src, op->source_offset + uvm_gnio_record_tag_off(op->size)));
            break;

        case UVM_GNIO_OP_COPY:
            context->gpu->parent->ce_hal->memcopy(push,
                                                  gnio_address(dst, op->destination_offset),
                                                  gnio_address(src, op->source_offset),
                                                  op->size);
            break;

        case UVM_GNIO_OP_TIMESTAMP:
            context->gpu->parent->ce_hal->semaphore_timestamp(
                push,
                gnio_address(dst, op->destination_offset).address);
            break;
    }
}


NV_STATUS uvm_gnio_submit(uvm_va_space_t *va_space, UVM_GNIO_SUBMIT_PARAMS *params)
{
    gnio_push_context_t context = { 0 };
    uvm_push_t push;
    NvU64 total_start_ns;
    NvU64 submit_start_ns;
    NvU64 phase_start_ns;
    NvU32 i;
    NV_STATUS status;

    params->confidential = g_uvm_global.conf_computing_enabled;
    params->total_ns = 0;
    params->submit_ns = 0;
    params->preprocess_ns = 0;
    params->postprocess_ns = 0;

    // Only bounds required to parse the fixed-size ioctl safely. All operation
    // contents are trusted: this is a single-user research prototype.
    if (params->abi_version != UVM_GNIO_SUBMIT_ABI_VERSION ||
        params->op_count == 0 || params->op_count > UVM_GNIO_MAX_OPS)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_read(va_space);

    status = gnio_channel_type(va_space, params, &context.gpu, &context.channel_type);
    if (status != NV_OK)
        goto out;

    status = uvm_push_begin(context.gpu->channel_manager,
                            context.channel_type,
                            &push,
                            "gnio typed push");
    if (status != NV_OK)
        goto out;

    total_start_ns = ktime_get_ns();

    phase_start_ns = ktime_get_ns();
    for (i = 0; i < params->op_count; i++)
        gnio_preprocess(va_space, &push, &params->ops[i]);
    params->preprocess_ns = ktime_get_ns() - phase_start_ns;

    for (i = 0; i < params->op_count; i++)
        gnio_emit(va_space, &context, &push, &params->ops[i]);

    submit_start_ns = ktime_get_ns();
    status = uvm_push_end_and_wait(&push);
    params->submit_ns = ktime_get_ns() - submit_start_ns;

    if (status == NV_OK) {
        phase_start_ns = ktime_get_ns();
        for (i = 0; i < params->op_count; i++) {
            status = gnio_postprocess(va_space, &context, &push, &params->ops[i]);
            if (status != NV_OK)
                break;
        }
        params->postprocess_ns = ktime_get_ns() - phase_start_ns;
    }

    params->total_ns = ktime_get_ns() - total_start_ns;

out:
    uvm_va_space_up_read(va_space);
    return status;
}
