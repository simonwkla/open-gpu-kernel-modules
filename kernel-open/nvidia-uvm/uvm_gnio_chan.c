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

// GNIO self-sufficient hot channel.
//
// A dedicated CE GPFIFO channel (GPFIFO + GP_PUT in CPR vidmem) whose whole ring is filled with one
// self-resetting pushbuffer. The channel runs to each ring slot's semaphore acquire and WAITS there --
// resident, never idle -- so a "fire" is just an SM store releasing the `go` semaphore (no doorbell, no
// CPU). Each fire re-arms `go` for the next slot, ticks `done` (so the SM can track completions), then
// advances the channel's own GP_PUT and rings its own doorbell -- so the channel keeps itself running
// with zero CPU involvement after the one-time arm. A real CE copy method goes between the acquire and
// the reset; the payload here is just the completion tick.

#include "uvm_gnio.h"
#include "uvm_global.h"
#include "uvm_gpu.h"
#include "uvm_channel.h"
#include "uvm_va_space.h"
#include "uvm_linux.h"
#include "uvm_push.h"
#include "uvm_hal.h"
#include "nv_uvm_interface.h"

#define UVM_GNIO_MAX_CHANS 16

typedef struct
{
    bool in_use;
    uvm_va_space_t *owner;
    uvm_gpu_t *gpu;
    uvmGpuTsgHandle tsg;
    uvmGpuChannelHandle handle;
    UvmGpuChannelInfo channel_info;
    NvU32 pb_handle;
    NvU32 pb_size;
} uvm_gnio_chan_t;

static uvm_gnio_chan_t g_gnio_chans[UVM_GNIO_MAX_CHANS];
static DEFINE_MUTEX(g_gnio_chans_lock);

static bool gnio_find_ce_engine_index(uvm_gpu_t *gpu, unsigned *index_out)
{
    uvm_channel_pool_t *pool;

    if (gpu->channel_manager == NULL)
        return false;

    uvm_for_each_pool_of_type(pool, gpu->channel_manager, UVM_CHANNEL_POOL_TYPE_CE) {
        *index_out = pool->engine_index;
        return true;
    }
    return false;
}

// Allocate a dedicated CE TSG + GPFIFO channel with its GPFIFO ring and GP_PUT in vidmem/CPR.
NV_STATUS uvm_gnio_chan_create(uvm_va_space_t *va_space, UVM_GNIO_CHAN_CREATE_PARAMS *params)
{
    NV_STATUS status;
    uvm_gpu_t *gpu;
    UvmGpuTsgAllocParams tsg_params;
    UvmGpuChannelAllocParams chan_params;
    uvmGpuTsgHandle tsg = NULL;
    unsigned ce_index = 0;
    int slot = -1;
    int i;

    uvm_va_space_down_read(va_space);

    gpu = uvm_va_space_find_first_gpu(va_space);
    if (gpu == NULL) {
        uvm_va_space_up_read(va_space);
        return NV_ERR_INVALID_STATE;
    }

    if (!gnio_find_ce_engine_index(gpu, &ce_index)) {
        uvm_va_space_up_read(va_space);
        return NV_ERR_INVALID_STATE;
    }

    mutex_lock(&g_gnio_chans_lock);
    for (i = 0; i < UVM_GNIO_MAX_CHANS; i++) {
        if (!g_gnio_chans[i].in_use) {
            g_gnio_chans[i].in_use = true;
            slot = i;
            break;
        }
    }
    mutex_unlock(&g_gnio_chans_lock);
    if (slot < 0) {
        uvm_va_space_up_read(va_space);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    memset(&tsg_params, 0, sizeof(tsg_params));
    tsg_params.engineType = UVM_GPU_CHANNEL_ENGINE_TYPE_CE;
    tsg_params.engineIndex = ce_index;

    status = uvm_rm_locked_call(nvUvmInterfaceTsgAllocate(gpu->rm_address_space, &tsg_params, &tsg));
    if (status != NV_OK)
        goto fail_slot;

    memset(&chan_params, 0, sizeof(chan_params));
    chan_params.numGpFifoEntries = 32;
    chan_params.gpFifoLoc = UVM_BUFFER_LOCATION_VID;
    chan_params.gpPutLoc = UVM_BUFFER_LOCATION_VID;

    status = uvm_rm_locked_call(nvUvmInterfaceChannelAllocate(tsg,
                                                             &chan_params,
                                                             &g_gnio_chans[slot].handle,
                                                             &g_gnio_chans[slot].channel_info));
    if (status != NV_OK)
        goto fail_tsg;

    g_gnio_chans[slot].owner = va_space;
    g_gnio_chans[slot].gpu = gpu;
    g_gnio_chans[slot].tsg = tsg;

    params->handle_out = (NvU32)slot;
    params->num_gpfifo_entries = g_gnio_chans[slot].channel_info.numGpFifoEntries;
    params->hw_channel_id = g_gnio_chans[slot].channel_info.hwChannelId;
    params->gpfifo_gpu_va = g_gnio_chans[slot].channel_info.gpFifoGpuVa;
    params->gpput_gpu_va = g_gnio_chans[slot].channel_info.gpPutGpuVa;
    params->doorbell_present = (g_gnio_chans[slot].channel_info.workSubmissionOffset != NULL) ? 1 : 0;

    uvm_va_space_up_read(va_space);
    return NV_OK;

fail_tsg:
    uvm_rm_locked_call_void(nvUvmInterfaceTsgDestroy(tsg));
fail_slot:
    g_gnio_chans[slot].in_use = false;
    uvm_va_space_up_read(va_space);
    return status;
}

static void gnio_chan_teardown(uvm_gnio_chan_t *ch)
{
    uvm_rm_locked_call_void(nvUvmInterfaceChannelDestroy(ch->handle));
    uvm_rm_locked_call_void(nvUvmInterfaceTsgDestroy(ch->tsg));
    ch->in_use = false;
    ch->owner = NULL;
}

NV_STATUS uvm_gnio_chan_destroy(uvm_va_space_t *va_space, NvU32 handle)
{
    uvm_gnio_chan_t *ch;

    if (handle >= UVM_GNIO_MAX_CHANS)
        return NV_ERR_INVALID_ARGUMENT;

    ch = &g_gnio_chans[handle];
    if (!ch->in_use || ch->owner != va_space)
        return NV_ERR_INVALID_ARGUMENT;

    gnio_chan_teardown(ch);
    return NV_OK;
}

// Author the self-sufficient hot pushbuffer into `params->methods` (the SM copies it into the CPR `pb`
// buffer). go/done live in one CPR `sems` buffer at go_off/done_off. See the pushbuffer layout comment
// in uvm_gnio_abi.h. A membar precedes the GP_PUT advance and the doorbell so the Host front-end
// observes them before the entry completes.
NV_STATUS uvm_gnio_chan_prep(uvm_va_space_t *va_space, UVM_GNIO_CHAN_PREP_PARAMS *params)
{
    uvm_gnio_chan_t *ch;
    uvm_gnio_buf_t *pb, *sems;
    uvm_gpu_t *gpu;
    uvm_push_t fake;
    NvU64 sems_va;
    NvU32 size;
    NV_STATUS status;
    const NvU32 go_off = 0, done_off = 256;

    if (params->chan_handle >= UVM_GNIO_MAX_CHANS)
        return NV_ERR_INVALID_ARGUMENT;
    ch = &g_gnio_chans[params->chan_handle];
    if (!ch->in_use || ch->owner != va_space)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_read(va_space);

    pb = uvm_gnio_buf_get(va_space, params->pb_handle);
    sems = uvm_gnio_buf_get(va_space, params->sems_handle);
    if (pb == NULL || sems == NULL) {
        uvm_va_space_up_read(va_space);
        return NV_ERR_INVALID_ARGUMENT;
    }
    gpu = ch->gpu;
    sems_va = uvm_gnio_buf_gpu_address(sems).address;

    status = uvm_push_begin_fake(gpu, &fake);
    if (status != NV_OK) {
        uvm_va_space_up_read(va_space);
        return status;
    }

    gpu->parent->host_hal->semaphore_acquire(&fake, sems_va + go_off, 1);          // wait for the SM's fire
    gpu->parent->ce_hal->semaphore_release(&fake, sems_va + go_off, 0);            // re-arm go for next slot
    gpu->parent->ce_hal->semaphore_reduction_inc(&fake, sems_va + done_off, 0x7fffffff);  // completion tick

    uvm_push_set_flag(&fake, UVM_PUSH_FLAG_NEXT_MEMBAR_GPU);                       // advance own GP_PUT
    gpu->parent->ce_hal->semaphore_reduction_inc(&fake, ch->channel_info.gpPutGpuVa,
                                                 ch->channel_info.numGpFifoEntries - 1);
    uvm_push_set_flag(&fake, UVM_PUSH_FLAG_NEXT_MEMBAR_GPU);                       // ring own doorbell
    gpu->parent->ce_hal->semaphore_release(&fake, ch->channel_info.workSubmissionOffsetGpuVa,
                                           ch->channel_info.workSubmissionToken);

    size = uvm_push_get_size(&fake);
    if (size > sizeof(params->methods))
        size = sizeof(params->methods);
    memcpy(params->methods, fake.begin, size);
    uvm_push_end_fake(&fake);

    ch->pb_handle = params->pb_handle;
    ch->pb_size = size;
    params->method_size = size;
    params->go_off = go_off;
    params->done_off = done_off;
    params->pb_gpu_va = uvm_gnio_buf_gpu_address(pb).address;
    params->sems_gpu_va = sems_va;

    uvm_va_space_up_read(va_space);
    return NV_OK;
}

// Fill the ring with [segment_base(pb), pb x(num_entries-1)], set GP_PUT=init_put, doorbell once.
NV_STATUS uvm_gnio_chan_arm(uvm_va_space_t *va_space, UVM_GNIO_CHAN_ARM_PARAMS *params)
{
    uvm_gnio_chan_t *ch;
    uvm_gnio_buf_t *pb;
    uvm_gpu_t *gpu;
    NvU64 pb_va, seg, ent;
    NvU32 num;
    NV_STATUS status = NV_OK;

    if (params->chan_handle >= UVM_GNIO_MAX_CHANS)
        return NV_ERR_INVALID_ARGUMENT;
    ch = &g_gnio_chans[params->chan_handle];
    if (!ch->in_use || ch->owner != va_space)
        return NV_ERR_INVALID_ARGUMENT;

    num = params->num_entries;
    if (num < 2 || num > ch->channel_info.numGpFifoEntries)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_va_space_down_read(va_space);

    pb = uvm_gnio_buf_get(va_space, ch->pb_handle);
    if (pb == NULL) {
        uvm_va_space_up_read(va_space);
        return NV_ERR_INVALID_STATE;
    }
    gpu = ch->gpu;
    pb_va = uvm_gnio_buf_gpu_address(pb).address;

    // Surface the encoded entries + primed GP_PUT so DRY can vet a pushbuffer address before the
    // wedge-prone doorbell (a bad address faults the CE).
    gpu->parent->host_hal->set_gpfifo_pushbuffer_segment_base(&seg, pb_va);
    gpu->parent->host_hal->set_gpfifo_entry(&ent, pb_va, ch->pb_size, UVM_GPFIFO_SYNC_PROCEED);
    params->pb_gpu_va = pb_va;
    params->seg_entry = seg;
    params->pb_entry = ent;
    params->put = params->init_put ? params->init_put : num;

    if (!params->dry)
        status = uvm_gnio_channel_arm_ring(gpu, &ch->channel_info, pb_va, ch->pb_size, num, params->init_put);

    uvm_va_space_up_read(va_space);
    return status;
}

void uvm_gnio_chan_destroy_all(uvm_va_space_t *va_space)
{
    int i;

    for (i = 0; i < UVM_GNIO_MAX_CHANS; i++) {
        if (g_gnio_chans[i].in_use && g_gnio_chans[i].owner == va_space)
            gnio_chan_teardown(&g_gnio_chans[i]);
    }
}
