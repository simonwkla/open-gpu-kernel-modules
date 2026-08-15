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

#ifndef __UVM_GNIO_H__
#define __UVM_GNIO_H__

#include "uvm_forward_decl.h"
#include "uvm_linux.h"
#include "uvm_hal_types.h"
#include "nvstatus.h"
#include "uvm_gnio_abi.h"

struct uvm_gnio_buf_struct;
typedef struct uvm_gnio_buf_struct uvm_gnio_buf_t;

typedef enum
{
    UVM_GNIO_BUF_PROTECTED,
    UVM_GNIO_BUF_EXPOSURE,
} uvm_gnio_buf_kind_t;

static inline bool uvm_gnio_is_gnio_cmd(unsigned cmd)
{
    return cmd >= UVM_GNIO_BASE && cmd < UVM_GNIO_LAST;
}

long uvm_gnio_ioctl(struct file *filp, unsigned cmd, unsigned long arg);

NV_STATUS uvm_gnio_mem_alloc_protected(uvm_va_space_t *va_space, NvU64 size, NvU32 *handle_out);
NV_STATUS uvm_gnio_mem_import_exposure(uvm_va_space_t *va_space,
                                       int dmabuf_fd,
                                       NvU64 size,
                                       NvU32 *handle_out);
NV_STATUS uvm_gnio_mem_free(uvm_va_space_t *va_space, NvU32 handle);
NV_STATUS uvm_gnio_mem_map_protected(uvm_va_space_t *va_space, NvU32 handle, NvU64 user_va);
NV_STATUS uvm_gnio_mem_unmap_protected(uvm_va_space_t *va_space, NvU32 handle);
void uvm_gnio_mem_free_all(uvm_va_space_t *va_space);

uvm_gnio_buf_t *uvm_gnio_buf_get(uvm_va_space_t *va_space, NvU32 handle);
uvm_gpu_t *uvm_gnio_buf_gpu(uvm_gnio_buf_t *buf);
NvU64 uvm_gnio_buf_size(uvm_gnio_buf_t *buf);
NvU32 uvm_gnio_buf_kind(uvm_gnio_buf_t *buf);
uvm_gpu_address_t uvm_gnio_buf_gpu_address(uvm_gnio_buf_t *buf);
void *uvm_gnio_buf_cpu_addr(uvm_gnio_buf_t *buf);

NV_STATUS uvm_gnio_submit(uvm_va_space_t *va_space, UVM_GNIO_SUBMIT_PARAMS *params);

#endif // __UVM_GNIO_H__
