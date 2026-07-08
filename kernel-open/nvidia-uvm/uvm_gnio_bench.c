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

    GNIO copy microbenchmarks. One generic latency/bandwidth driver brackets the
    copy under test with GPU PTIMER timestamps (ce_ns) and CPU wall-clock around
    push_end_and_wait (total_ns). The copy itself is selected per kind: CE encrypt
    (sealed DtoH), CE decrypt (sealed HtoD), or plain CE memcopy (DtoD and the
    non-CC DtoH/HtoD). Timestamps live in vidmem and are read out either by a
    secure egress + cpu_decrypt (CC) or a plain memcopy egress (non-CC).

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
#include "uvm_kvmalloc.h"
#include "uvm_linux.h"

#define GNIO_TS_SLOT_SIZE   16
#define GNIO_TS_TIME_OFFSET 8
#define GNIO_TS_SLOTS       2
#define GNIO_TS_BYTES       (GNIO_TS_SLOTS * GNIO_TS_SLOT_SIZE)

// Conservative per-copy pushbuffer headroom (one secure CE copy is ~64 B of
// methods; the slack also covers the closing timestamp release).
#define GNIO_PUSH_COPY_RESERVE 512

typedef enum {
    GNIO_EMIT_MEMCOPY,  // plain CE copy: DtoD (PROT2PROT), non-CC DtoH/HtoD
    GNIO_EMIT_ENCRYPT,  // secure CE copy CPR -> unprotected sysmem (sealed DtoH)
    GNIO_EMIT_DECRYPT,  // secure CE copy unprotected sysmem -> CPR (sealed HtoD)
} gnio_emit_t;

typedef struct {
    gnio_emit_t        emit;
    uvm_channel_type_t channel_type;
} gnio_op_t;

typedef struct {
    uvm_gpu_t *gpu;
    gnio_op_t  op;
    size_t     size;

    uvm_mem_t *ts_cpr;   // vidmem PTIMER release target
    uvm_mem_t *ts_sys;   // sysmem readout landing
    uvm_mem_t *ts_tag;   // auth tag for the secure ts egress (CC only)

    uvm_mem_t *copy_tag; // per-slot auth tags for the timed copy (sealed only)
    UvmCslIv  *iv_scratch;   // GPU-encryption IV log (encrypt only)
    void      *plain_scratch; // plaintext fed to cpu_encrypt (decrypt only)
} gnio_bench_ctx_t;

static uvm_gpu_address_t gnio_addr_offset(uvm_gpu_address_t base, NvU64 offset)
{
    base.address += offset;
    return base;
}

static NV_STATUS gnio_resolve_op(NvU32 kind, gnio_op_t *op)
{
    bool cc = g_uvm_global.conf_computing_enabled;

    switch (kind) {
        case UVM_GNIO_COPY_DTOH_SEALED:
            if (!cc)
                return NV_ERR_NOT_SUPPORTED;
            op->emit = GNIO_EMIT_ENCRYPT;
            op->channel_type = UVM_CHANNEL_TYPE_GPU_TO_CPU;
            return NV_OK;
        case UVM_GNIO_COPY_HTOD_SEALED:
            if (!cc)
                return NV_ERR_NOT_SUPPORTED;
            op->emit = GNIO_EMIT_DECRYPT;
            op->channel_type = UVM_CHANNEL_TYPE_CPU_TO_GPU;
            return NV_OK;
        case UVM_GNIO_COPY_DTOD:
            op->emit = GNIO_EMIT_MEMCOPY;
            op->channel_type = UVM_CHANNEL_TYPE_GPU_INTERNAL;
            return NV_OK;
        case UVM_GNIO_COPY_DTOH:
            if (cc)
                return NV_ERR_NOT_SUPPORTED;
            op->emit = GNIO_EMIT_MEMCOPY;
            op->channel_type = UVM_CHANNEL_TYPE_GPU_TO_CPU;
            return NV_OK;
        case UVM_GNIO_COPY_HTOD:
            if (cc)
                return NV_ERR_NOT_SUPPORTED;
            op->emit = GNIO_EMIT_MEMCOPY;
            op->channel_type = UVM_CHANNEL_TYPE_CPU_TO_GPU;
            return NV_OK;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
}

static bool gnio_op_sealed(const gnio_op_t *op)
{
    return op->emit == GNIO_EMIT_ENCRYPT || op->emit == GNIO_EMIT_DECRYPT;
}

static void gnio_ctx_destroy(gnio_bench_ctx_t *ctx)
{
    uvm_kvfree(ctx->plain_scratch);
    uvm_kvfree(ctx->iv_scratch);
    uvm_gnio_free_mem(ctx->copy_tag);
    uvm_gnio_free_mem(ctx->ts_tag);
    uvm_gnio_free_mem(ctx->ts_sys);
    uvm_gnio_free_mem(ctx->ts_cpr);
}

static NV_STATUS gnio_ctx_init(gnio_bench_ctx_t *ctx, uvm_gpu_t *gpu, gnio_op_t op,
                               size_t size, NvU32 nslots)
{
    NV_STATUS status;

    memset(ctx, 0, sizeof(*ctx));
    ctx->gpu = gpu;
    ctx->op = op;
    ctx->size = size;

    status = uvm_gnio_alloc_vidmem(gpu, GNIO_TS_BYTES, &ctx->ts_cpr);
    if (status != NV_OK)
        goto err;
    status = uvm_gnio_alloc_sysmem(gpu, GNIO_TS_BYTES, &ctx->ts_sys);
    if (status != NV_OK)
        goto err;

    if (g_uvm_global.conf_computing_enabled) {
        status = uvm_gnio_alloc_sysmem(gpu, UVM_CONF_COMPUTING_AUTH_TAG_SIZE, &ctx->ts_tag);
        if (status != NV_OK)
            goto err;
    }

    if (gnio_op_sealed(&op)) {
        status = uvm_gnio_alloc_sysmem(gpu, (NvU64)nslots * uvm_gnio_seal_tag_bytes(size), &ctx->copy_tag);
        if (status != NV_OK)
            goto err;
    }
    if (op.emit == GNIO_EMIT_ENCRYPT) {
        ctx->iv_scratch = uvm_kvmalloc(uvm_gnio_iv_count(size) * sizeof(*ctx->iv_scratch));
        if (ctx->iv_scratch == NULL) {
            status = NV_ERR_NO_MEMORY;
            goto err;
        }
    }
    if (op.emit == GNIO_EMIT_DECRYPT) {
        ctx->plain_scratch = uvm_kvmalloc(size);
        if (ctx->plain_scratch == NULL) {
            status = NV_ERR_NO_MEMORY;
            goto err;
        }
    }

    return NV_OK;

err:
    gnio_ctx_destroy(ctx);
    return status;
}

static void gnio_push_timestamp(gnio_bench_ctx_t *ctx, uvm_push_t *push, NvU32 slot)
{
    uvm_gpu_address_t ts = uvm_mem_gpu_address_virtual_kernel(ctx->ts_cpr, ctx->gpu);
    ctx->gpu->parent->ce_hal->semaphore_timestamp(push, ts.address + (NvU64)slot * GNIO_TS_SLOT_SIZE);
}

// The CE secure-copy engine derives the auth-tag address using the addressing
// mode of the ciphertext operand (dst for encrypt, src for decrypt). The tag
// pointer must match: physical+SYS when the cipher is a physical sysmem import
// (the coh staging buffer), virtual otherwise. A mismatch makes the GPU read the
// decrypt compare-tag from the wrong place and faults the secure channel.
static uvm_gpu_address_t gnio_tag_addr(gnio_bench_ctx_t *ctx, uvm_gpu_address_t cipher, size_t tag_off)
{
    if (cipher.is_virtual)
        return gnio_addr_offset(uvm_mem_gpu_address_virtual_kernel(ctx->copy_tag, ctx->gpu), tag_off);

    return uvm_mem_gpu_address_physical(ctx->copy_tag, ctx->gpu, tag_off, UVM_CONF_COMPUTING_AUTH_TAG_SIZE);
}

// Emit one copy of `size` bytes from src to dst. For decrypt, fresh ciphertext +
// auth tag are produced in `cipher_cpu` first; cpu_encrypt advances the channel
// CPU-encrypt IV in lockstep with the GPU decrypt IV, so each decrypt needs its
// own (cipher, tag) and slots can never be reused within one push.
static void gnio_emit(gnio_bench_ctx_t *ctx, uvm_push_t *push, uvm_gpu_address_t dst,
                      uvm_gpu_address_t src, void *cipher_cpu, size_t tag_off, size_t size)
{
    uvm_gpu_t *gpu = ctx->gpu;
    uvm_gpu_address_t tag;

    switch (ctx->op.emit) {
        case GNIO_EMIT_MEMCOPY:
            gpu->parent->ce_hal->memcopy(push, dst, src, size);
            return;
        case GNIO_EMIT_ENCRYPT:
            tag = gnio_tag_addr(ctx, dst, tag_off);
            uvm_conf_computing_log_gpu_encryption(push->channel, size, ctx->iv_scratch);
            gpu->parent->ce_hal->encrypt(push, dst, src, size, tag);
            return;
        case GNIO_EMIT_DECRYPT:
            tag = gnio_tag_addr(ctx, src, tag_off);
            uvm_conf_computing_cpu_encrypt(push->channel, cipher_cpu, ctx->plain_scratch, NULL, size,
                                           (NvU8 *)uvm_mem_get_cpu_addr_kernel(ctx->copy_tag) + tag_off);
            gpu->parent->ce_hal->decrypt(push, dst, src, size, tag);
            return;
    }
}

// Emit up to `count` copies into an in-progress push, returning the number
// actually emitted (capped by pushbuffer space). Plain/encrypt copies reuse a
// pool of `nslots` buffers round-robin; decrypt copies must use a fresh slot
// each, so the caller passes count <= nslots and reuse = false.
static NvU32 gnio_push_copies(gnio_bench_ctx_t *ctx, uvm_push_t *push, NvU32 count, bool reuse,
                              NvU32 nslots, uvm_gpu_address_t dst_base, uvm_gpu_address_t src_base,
                              void *cipher_base, size_t size)
{
    size_t tagb = uvm_gnio_seal_tag_bytes(size);
    NvU32 k;

    for (k = 0; k < count && uvm_push_has_space(push, GNIO_PUSH_COPY_RESERVE); k++) {
        NvU32 slot = reuse ? (k % nslots) : k;
        void *cipher = cipher_base ? (NvU8 *)cipher_base + (size_t)slot * size : NULL;

        gnio_emit(ctx, push,
                  gnio_addr_offset(dst_base, (NvU64)slot * size),
                  gnio_addr_offset(src_base, (NvU64)slot * size),
                  cipher, (size_t)slot * tagb, size);
    }
    return k;
}

// Read the two PTIMER slots out of the CPR timestamp buffer. Untimed; runs after
// the measured push completes.
static NV_STATUS gnio_read_timestamps(gnio_bench_ctx_t *ctx, NvU64 *t0, NvU64 *t1)
{
    uvm_gpu_t *gpu = ctx->gpu;
    uvm_push_t push;
    NV_STATUS status;
    NvU8 plain[GNIO_TS_BYTES];
    const NvU8 *out;

    status = uvm_push_begin(gpu->channel_manager, UVM_CHANNEL_TYPE_GPU_TO_CPU, &push, "gnio ts egress");
    if (status != NV_OK)
        return status;

    if (g_uvm_global.conf_computing_enabled) {
        UvmCslIv iv;
        NvU32 key_version;

        uvm_conf_computing_log_gpu_encryption(push.channel, GNIO_TS_BYTES, &iv);
        key_version = uvm_channel_pool_key_version(push.channel->pool);
        gpu->parent->ce_hal->encrypt(&push,
                                     uvm_mem_gpu_address_virtual_kernel(ctx->ts_sys, gpu),
                                     uvm_mem_gpu_address_virtual_kernel(ctx->ts_cpr, gpu),
                                     GNIO_TS_BYTES,
                                     uvm_mem_gpu_address_virtual_kernel(ctx->ts_tag, gpu));

        status = uvm_push_end_and_wait(&push);
        if (status != NV_OK)
            return status;

        status = uvm_conf_computing_cpu_decrypt(push.channel, plain,
                                                uvm_mem_get_cpu_addr_kernel(ctx->ts_sys),
                                                &iv, key_version, GNIO_TS_BYTES,
                                                uvm_mem_get_cpu_addr_kernel(ctx->ts_tag));
        if (status != NV_OK)
            return status;
        out = plain;
    } else {
        gpu->parent->ce_hal->memcopy(&push,
                                     uvm_mem_gpu_address_virtual_kernel(ctx->ts_sys, gpu),
                                     uvm_mem_gpu_address_virtual_kernel(ctx->ts_cpr, gpu),
                                     GNIO_TS_BYTES);

        status = uvm_push_end_and_wait(&push);
        if (status != NV_OK)
            return status;
        out = uvm_mem_get_cpu_addr_kernel(ctx->ts_sys);
    }

    memcpy(t0, out + GNIO_TS_TIME_OFFSET, sizeof(*t0));
    memcpy(t1, out + GNIO_TS_SLOT_SIZE + GNIO_TS_TIME_OFFSET, sizeof(*t1));
    return NV_OK;
}

NV_STATUS uvm_gnio_bench_latency(uvm_va_space_t *va_space, UVM_GNIO_BENCH_LATENCY_PARAMS *params)
{
    uvm_gnio_buf_t *dst = uvm_gnio_buf_get(va_space, params->dst_handle);
    uvm_gnio_buf_t *src = uvm_gnio_buf_get(va_space, params->src_handle);
    gnio_bench_ctx_t ctx;
    gnio_op_t op;
    uvm_gpu_address_t src_addr, dst_addr;
    void *cipher_cpu = NULL;
    NvU64 *total_ns = NULL, *ce_ns = NULL;
    size_t size = params->size;
    NvU32 total_iters = params->warmup + params->iters;
    NvU32 i;
    NV_STATUS status;

    if (dst == NULL || src == NULL || size == 0 || params->iters == 0)
        return NV_ERR_INVALID_ARGUMENT;

    status = gnio_resolve_op(params->kind, &op);
    if (status != NV_OK)
        return status;

    total_ns = uvm_kvmalloc(params->iters * sizeof(*total_ns));
    ce_ns = uvm_kvmalloc(params->iters * sizeof(*ce_ns));
    if (total_ns == NULL || ce_ns == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto out;
    }

    uvm_va_space_down_read(va_space);

    status = gnio_ctx_init(&ctx, uvm_gnio_buf_gpu(src), op, size, 1);
    if (status != NV_OK)
        goto out_unlock;

    src_addr = uvm_gnio_buf_gpu_address(src);
    dst_addr = uvm_gnio_buf_gpu_address(dst);
    if (op.emit == GNIO_EMIT_DECRYPT) {
        cipher_cpu = uvm_gnio_buf_cpu_addr(src);
        if (cipher_cpu == NULL) {
            status = NV_ERR_INVALID_ARGUMENT;
            goto out_ctx;
        }
    }

    for (i = 0; i < total_iters; i++) {
        uvm_push_t push;
        NvU64 cpu_t0, cpu_t1, g0 = 0, g1 = 0;

        status = uvm_push_begin(ctx.gpu->channel_manager, op.channel_type, &push, "gnio lat");
        if (status != NV_OK)
            goto out_ctx;

        gnio_push_timestamp(&ctx, &push, 0);
        gnio_emit(&ctx, &push, dst_addr, src_addr, cipher_cpu, 0, size);
        gnio_push_timestamp(&ctx, &push, 1);

        cpu_t0 = ktime_get_ns();
        status = uvm_push_end_and_wait(&push);
        cpu_t1 = ktime_get_ns();
        if (status != NV_OK)
            goto out_ctx;

        status = gnio_read_timestamps(&ctx, &g0, &g1);
        if (status != NV_OK)
            goto out_ctx;

        if (i >= params->warmup) {
            total_ns[i - params->warmup] = cpu_t1 - cpu_t0;
            ce_ns[i - params->warmup] = g1 - g0;
        }
    }

    status = NV_OK;

out_ctx:
    gnio_ctx_destroy(&ctx);
out_unlock:
    uvm_va_space_up_read(va_space);

    if (status == NV_OK) {
        if (copy_to_user((void __user *)params->total_ns_user, total_ns, params->iters * sizeof(*total_ns)) ||
            copy_to_user((void __user *)params->ce_ns_user, ce_ns, params->iters * sizeof(*ce_ns)))
            status = NV_ERR_INVALID_ADDRESS;
    }

out:
    uvm_kvfree(ce_ns);
    uvm_kvfree(total_ns);
    return status;
}

NV_STATUS uvm_gnio_bench_bandwidth(uvm_va_space_t *va_space, UVM_GNIO_BENCH_BANDWIDTH_PARAMS *params)
{
    uvm_gnio_buf_t *dst = uvm_gnio_buf_get(va_space, params->dst_handle);
    uvm_gnio_buf_t *src = uvm_gnio_buf_get(va_space, params->src_handle);
    gnio_bench_ctx_t ctx;
    gnio_op_t op;
    uvm_gpu_address_t src_addr, dst_addr;
    void *cipher_cpu = NULL;
    uvm_push_t push;
    size_t size = params->size;
    NvU32 batch = params->batch;
    NvU64 dst_slots, src_slots, nslots;
    bool reuse;
    NvU32 emit_count, done = 0;
    NvU64 t0 = 0, t1 = 0;
    NV_STATUS status;

    params->bytes_out = 0;
    params->window_ns_out = 0;

    if (dst == NULL || src == NULL || batch == 0 || size == 0)
        return NV_ERR_INVALID_ARGUMENT;

    status = gnio_resolve_op(params->kind, &op);
    if (status != NV_OK)
        return status;

    // The buffers hold a small pool of slots, not one per copy. Plain/encrypt
    // copies reuse them round-robin; decrypt copies cannot reuse a slot within a
    // push, so the fused count is clamped to the slot pool.
    dst_slots = uvm_gnio_buf_size(dst) / size;
    src_slots = uvm_gnio_buf_size(src) / size;
    nslots = min(dst_slots, src_slots);
    nslots = min(nslots, (NvU64)batch);
    if (nslots == 0)
        return NV_ERR_INVALID_ARGUMENT;

    reuse = op.emit != GNIO_EMIT_DECRYPT;
    emit_count = reuse ? batch : (NvU32)nslots;

    uvm_va_space_down_read(va_space);

    status = gnio_ctx_init(&ctx, uvm_gnio_buf_gpu(src), op, size, (NvU32)nslots);
    if (status != NV_OK)
        goto out_unlock;

    src_addr = uvm_gnio_buf_gpu_address(src);
    dst_addr = uvm_gnio_buf_gpu_address(dst);
    if (op.emit == GNIO_EMIT_DECRYPT) {
        cipher_cpu = uvm_gnio_buf_cpu_addr(src);
        if (cipher_cpu == NULL) {
            status = NV_ERR_INVALID_ARGUMENT;
            goto out_ctx;
        }
    }

    if (params->warmup_copies > 0) {
        NvU32 warm = reuse ? params->warmup_copies : (NvU32)nslots;
        uvm_push_t warmup;

        status = uvm_push_begin(ctx.gpu->channel_manager, op.channel_type, &warmup, "gnio bw warmup");
        if (status != NV_OK)
            goto out_ctx;

        gnio_push_copies(&ctx, &warmup, warm, reuse, (NvU32)nslots, dst_addr, src_addr, cipher_cpu, size);

        status = uvm_push_end_and_wait(&warmup);
        if (status != NV_OK)
            goto out_ctx;
    }

    status = uvm_push_begin(ctx.gpu->channel_manager, op.channel_type, &push, "gnio bw");
    if (status != NV_OK)
        goto out_ctx;

    gnio_push_timestamp(&ctx, &push, 0);
    done = gnio_push_copies(&ctx, &push, emit_count, reuse, (NvU32)nslots, dst_addr, src_addr, cipher_cpu, size);
    gnio_push_timestamp(&ctx, &push, 1);

    status = uvm_push_end_and_wait(&push);
    if (status != NV_OK)
        goto out_ctx;

    status = gnio_read_timestamps(&ctx, &t0, &t1);
    if (status != NV_OK)
        goto out_ctx;

    params->bytes_out = (NvU64)done * size;
    params->window_ns_out = t1 - t0;

out_ctx:
    gnio_ctx_destroy(&ctx);
out_unlock:
    uvm_va_space_up_read(va_space);
    return status;
}
