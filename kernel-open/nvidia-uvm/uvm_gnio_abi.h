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
#define UVM_GNIO_S32 int

#define UVM_GNIO_BASE              2048
#define UVM_GNIO_ALLOC_PROTECTED  (UVM_GNIO_BASE + 0)
#define UVM_GNIO_FREE             (UVM_GNIO_BASE + 1)
#define UVM_GNIO_IMPORT_EXPOSURE  (UVM_GNIO_BASE + 2)
#define UVM_GNIO_MAP_PROTECTED    (UVM_GNIO_BASE + 3)
#define UVM_GNIO_UNMAP_PROTECTED  (UVM_GNIO_BASE + 4)
#define UVM_GNIO_SUBMIT           (UVM_GNIO_BASE + 5)
#define UVM_GNIO_LAST             (UVM_GNIO_BASE + 6)

#define UVM_GNIO_SUBMIT_ABI_VERSION 5u
#define UVM_GNIO_MAX_OPS          1024u
#define UVM_GNIO_MAX_TIMESTAMPS      8u

#define UVM_GNIO_OP_SEMAPHORE_ACQUIRE 1u
#define UVM_GNIO_OP_SEMAPHORE_RELEASE 2u
#define UVM_GNIO_OP_SEAL              3u
#define UVM_GNIO_OP_UNSEAL            4u
#define UVM_GNIO_OP_COPY              5u
#define UVM_GNIO_OP_TIMESTAMP         6u
#define UVM_GNIO_OP_ENCRYPT           7u
#define UVM_GNIO_OP_DECRYPT           8u

#define UVM_GNIO_AUTH_TAG_SIZE 16u
#define UVM_GNIO_IV_SIZE       13u
#define UVM_GNIO_PAGE_SIZE     4096u
#define UVM_GNIO_SEAL_ALIGN    16u
#define UVM_GNIO_RECORD_ALIGN  32u

typedef struct
{
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 handle_out;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_ALLOC_PROTECTED_PARAMS;

typedef struct
{
    UVM_GNIO_U32 handle;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_FREE_PARAMS;

// The exposure DMA-BUF must be CPU coherent and map as one contiguous DMA
// segment. Its caller owns synchronization with all non-GNIO users.
typedef struct
{
    UVM_GNIO_S32 dmabuf_fd;
    UVM_GNIO_U32 rmStatus;
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 handle_out;
    UVM_GNIO_U32 reserved;
} UVM_GNIO_IMPORT_EXPOSURE_PARAMS;

typedef struct
{
    UVM_GNIO_U32 handle;
    UVM_GNIO_U32 rmStatus;
    UVM_GNIO_U64 user_va;
} UVM_GNIO_MAP_PROTECTED_PARAMS;

typedef struct
{
    UVM_GNIO_U32 handle;
    UVM_GNIO_U32 rmStatus;
} UVM_GNIO_UNMAP_PROTECTED_PARAMS;

// One typed element in a managed UVM push. Fields not used by an operation are zero.
typedef struct
{
    UVM_GNIO_U32 type;
    UVM_GNIO_U32 source_handle;
    UVM_GNIO_U32 destination_handle;
    UVM_GNIO_U64 source_offset;
    UVM_GNIO_U64 destination_offset;
    UVM_GNIO_U64 size;
    UVM_GNIO_U32 value;
} UVM_GNIO_PUSH_OP;

// Synchronously processes the typed list. GPU operations are emitted into one
// managed UVM push; CPU ENCRYPT/DECRYPT operations run before/after that push.
typedef struct
{
    UVM_GNIO_U32 abi_version;
    UVM_GNIO_U32 op_count;
    UVM_GNIO_U32 confidential;
    UVM_GNIO_U32 rmStatus;
    UVM_GNIO_PUSH_OP ops[UVM_GNIO_MAX_OPS];
    UVM_GNIO_U64 total_ns;
    UVM_GNIO_U64 submit_ns;
    UVM_GNIO_U64 preprocess_ns;
    UVM_GNIO_U64 postprocess_ns;
} UVM_GNIO_SUBMIT_PARAMS;

static inline UVM_GNIO_U64 uvm_gnio_align_up(UVM_GNIO_U64 value, UVM_GNIO_U64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline UVM_GNIO_U64 uvm_gnio_record_count(UVM_GNIO_U64 size)
{
    return (size + UVM_GNIO_PAGE_SIZE - 1) / UVM_GNIO_PAGE_SIZE;
}

static inline UVM_GNIO_U64 uvm_gnio_record_tag_off(UVM_GNIO_U64 size)
{
    return uvm_gnio_align_up(size, UVM_GNIO_SEAL_ALIGN);
}

static inline UVM_GNIO_U64 uvm_gnio_record_iv_off(UVM_GNIO_U64 size)
{
    return uvm_gnio_align_up(uvm_gnio_record_tag_off(size) +
                             uvm_gnio_record_count(size) * UVM_GNIO_AUTH_TAG_SIZE,
                             UVM_GNIO_SEAL_ALIGN);
}

static inline UVM_GNIO_U64 uvm_gnio_record_bytes(UVM_GNIO_U64 size)
{
    return uvm_gnio_align_up(uvm_gnio_record_iv_off(size) +
                             uvm_gnio_record_count(size) * UVM_GNIO_IV_SIZE,
                             UVM_GNIO_RECORD_ALIGN);
}

#endif // __UVM_GNIO_ABI_H__
