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

#ifndef __UVM_GNIO_ABI_H__
#define __UVM_GNIO_ABI_H__

#define UVM_GNIO_U64 unsigned long long
#define UVM_GNIO_U32 unsigned int
#define UVM_GNIO_S64 long long

#define UVM_GNIO_BASE              2048
#define UVM_GNIO_ALLOC            (UVM_GNIO_BASE + 0)
#define UVM_GNIO_FREE             (UVM_GNIO_BASE + 1)
#define UVM_GNIO_IMPORT_DMABUF    (UVM_GNIO_BASE + 2)
#define UVM_GNIO_COPY             (UVM_GNIO_BASE + 3)
#define UVM_GNIO_BENCH_LATENCY    (UVM_GNIO_BASE + 4)
#define UVM_GNIO_BENCH_BANDWIDTH  (UVM_GNIO_BASE + 5)
#define UVM_GNIO_CALIBRATE_PTIMER (UVM_GNIO_BASE + 6)
#define UVM_GNIO_MAP_USER         (UVM_GNIO_BASE + 7)
#define UVM_GNIO_UNMAP_USER       (UVM_GNIO_BASE + 8)
#define UVM_GNIO_LAST             (UVM_GNIO_BASE + 9)

#define UVM_GNIO_KIND_CPR_VIDMEM      0
#define UVM_GNIO_KIND_UNPROT_SYSMEM   1
#define UVM_GNIO_KIND_IMPORTED_DMABUF 2

#define UVM_GNIO_COPY_HTOD        0
#define UVM_GNIO_COPY_DTOH        1
#define UVM_GNIO_COPY_DTOD        2
#define UVM_GNIO_COPY_DTOH_SEALED 3
#define UVM_GNIO_COPY_HTOD_SEALED 4
// probes that will fail / should produce a fault under CC
#define UVM_GNIO_COPY_DTOH_PLAIN  5
#define UVM_GNIO_COPY_HTOD_PLAIN  6

#define UVM_GNIO_F_MEASURE_DISPATCH (1u << 0)

typedef struct
{
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 kind;
    UVM_GNIO_U32 handle_out;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_ALLOC_PARAMS;

typedef struct
{
    UVM_GNIO_U32 handle;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_FREE_PARAMS;

typedef struct
{
    UVM_GNIO_S64 dmabuf_fd;
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 handle_out;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_IMPORT_DMABUF_PARAMS;

typedef struct
{
    UVM_GNIO_U32 handle;
    UVM_GNIO_U32 rmStatus;
    UVM_GNIO_U64 user_va;
} UVM_GNIO_MAP_USER_PARAMS;

typedef struct
{
    UVM_GNIO_U32 handle;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_UNMAP_USER_PARAMS;

typedef struct
{
    UVM_GNIO_U32 dst_handle;
    UVM_GNIO_U32 src_handle;
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 kind;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_COPY_PARAMS;

// Sealed DtoH lays out the destination dmabuf as a self-describing blob:
//   [ ciphertext (size) | pad | auth tag | pad | per-page IV log ]
// The CE writes the ciphertext and tag (physical+SYS), the CPU logs the IVs into
// the same buffer. Shared by the kernel (which writes at these offsets) and the
// userspace allocator (which must size the dmabuf to match). The kernel asserts
// these constants against the live UVM types (BUILD_BUG_ON).
#define UVM_GNIO_PAGE_SIZE     4096u
#define UVM_GNIO_AUTH_TAG_SIZE 16u   // == UVM_CONF_COMPUTING_AUTH_TAG_SIZE
#define UVM_GNIO_IV_SIZE       13u   // == sizeof(UvmCslIv): u8 iv[12] + u8 fresh
#define UVM_GNIO_SEAL_ALIGN    16u

static inline UVM_GNIO_U64 uvm_gnio_align_up(UVM_GNIO_U64 v, UVM_GNIO_U64 a)
{
    return (v + (a - 1)) & ~(UVM_GNIO_U64)(a - 1);
}

static inline UVM_GNIO_U64 uvm_gnio_iv_count(UVM_GNIO_U64 size)
{
    return (size + UVM_GNIO_PAGE_SIZE - 1) / UVM_GNIO_PAGE_SIZE;
}

static inline UVM_GNIO_U64 uvm_gnio_seal_tag_bytes(UVM_GNIO_U64 size)
{
    return uvm_gnio_iv_count(size) * UVM_GNIO_AUTH_TAG_SIZE;
}

static inline UVM_GNIO_U64 uvm_gnio_seal_iv_bytes(UVM_GNIO_U64 size)
{
    return uvm_gnio_iv_count(size) * UVM_GNIO_IV_SIZE;
}

static inline UVM_GNIO_U64 uvm_gnio_seal_tag_off(UVM_GNIO_U64 size)
{
    return uvm_gnio_align_up(size, UVM_GNIO_SEAL_ALIGN);
}

static inline UVM_GNIO_U64 uvm_gnio_seal_iv_off(UVM_GNIO_U64 size)
{
    return uvm_gnio_align_up(uvm_gnio_seal_tag_off(size) + uvm_gnio_seal_tag_bytes(size),
                             UVM_GNIO_SEAL_ALIGN);
}

static inline UVM_GNIO_U64 uvm_gnio_sealed_dtoh_buf_bytes(UVM_GNIO_U64 size)
{
    return uvm_gnio_seal_iv_off(size) + uvm_gnio_seal_iv_bytes(size);
}

typedef struct
{
    UVM_GNIO_U32 src_handle;
    UVM_GNIO_U32 dst_handle;
    UVM_GNIO_U32 kind;
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 iters;
    UVM_GNIO_U32 warmup;
    UVM_GNIO_U32 flags;
    UVM_GNIO_U64 total_ns_user;
    UVM_GNIO_U64 ce_ns_user;
    UVM_GNIO_S64 dispatch_ns_out;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_BENCH_LATENCY_PARAMS;

typedef struct
{
    UVM_GNIO_U32 src_handle;
    UVM_GNIO_U32 dst_handle;
    UVM_GNIO_U32 kind;
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 batch;
    UVM_GNIO_U32 warmup_copies;
    UVM_GNIO_U32 flags;
    UVM_GNIO_U64 bytes_out;
    UVM_GNIO_U64 window_ns_out;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_BENCH_BANDWIDTH_PARAMS;

#endif // __UVM_GNIO_ABI_H__
