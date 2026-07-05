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
#include "uvm_va_space.h"
#include "uvm_linux.h"

#define UVM_GNIO_DISPATCH(params_type, handler)                              \
    do {                                                                     \
        params_type params;                                                  \
        if (copy_from_user(&params, (void __user *)arg, sizeof(params)))     \
            return -EFAULT;                                                  \
        params.rmStatus = (handler);                                         \
        if (copy_to_user((void __user *)arg, &params, sizeof(params)))       \
            return -EFAULT;                                                  \
        return 0;                                                            \
    } while (0)

long uvm_gnio_ioctl(struct file *filp, unsigned cmd, unsigned long arg)
{
    uvm_va_space_t *va_space = uvm_fd_va_space(filp);

    if (va_space == NULL)
        return -EINVAL;

    switch (cmd) {
        case UVM_GNIO_ALLOC:
            UVM_GNIO_DISPATCH(UVM_GNIO_ALLOC_PARAMS,
                uvm_gnio_mem_alloc(va_space, params.size, params.kind, &params.handle_out));

        case UVM_GNIO_FREE:
            UVM_GNIO_DISPATCH(UVM_GNIO_FREE_PARAMS,
                uvm_gnio_mem_free(va_space, params.handle));

        case UVM_GNIO_IMPORT_DMABUF:
            UVM_GNIO_DISPATCH(UVM_GNIO_IMPORT_DMABUF_PARAMS,
                uvm_gnio_mem_import_dmabuf(va_space, (int)params.dmabuf_fd, params.size, &params.handle_out));

        case UVM_GNIO_MAP_USER:
            UVM_GNIO_DISPATCH(UVM_GNIO_MAP_USER_PARAMS,
                uvm_gnio_mem_map_user(va_space, params.handle, params.user_va));

        case UVM_GNIO_UNMAP_USER:
            UVM_GNIO_DISPATCH(UVM_GNIO_UNMAP_USER_PARAMS,
                uvm_gnio_mem_unmap_user(va_space, params.handle));

        case UVM_GNIO_COPY: {
            UVM_GNIO_COPY_PARAMS params;
            if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
                return -EFAULT;
            params.rmStatus = uvm_gnio_copy(va_space, &params);
            if (copy_to_user((void __user *)arg, &params, sizeof(params)))
                return -EFAULT;
            return 0;
        }

        case UVM_GNIO_BENCH_LATENCY: {
            UVM_GNIO_BENCH_LATENCY_PARAMS params;
            if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
                return -EFAULT;
            params.rmStatus = uvm_gnio_bench_latency(va_space, &params);
            if (copy_to_user((void __user *)arg, &params, sizeof(params)))
                return -EFAULT;
            return 0;
        }

        case UVM_GNIO_BENCH_BANDWIDTH: {
            UVM_GNIO_BENCH_BANDWIDTH_PARAMS params;
            if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
                return -EFAULT;
            params.rmStatus = uvm_gnio_bench_bandwidth(va_space, &params);
            if (copy_to_user((void __user *)arg, &params, sizeof(params)))
                return -EFAULT;
            return 0;
        }

        default:
            return -ENOTTY;
    }
}
