// Copyright (c) Facebook, Inc. and its affiliates.
// Licensed under the Apache License, Version 2.0 (the "License")
//
// Copyright 2023-2024 The Parca Authors

#include "pyperf.h"

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "hash.h"
#include "shared.h"
#include "tls.h"

//
//   ╔═════════════════════════════════════════════════════════════════════════╗
//   ║ Constants and Configuration                                             ║
//   ╚═════════════════════════════════════════════════════════════════════════╝
//
const volatile bool verbose = false;

//
//   ╔═════════════════════════════════════════════════════════════════════════╗
//   ║  BPF Maps                                                               ║
//   ╚═════════════════════════════════════════════════════════════════════════╝
//

struct {
    __uint(type, BPF_MAP_TYPE_PROG_ARRAY);
    __uint(max_entries, 3);
    __type(key, u32);
    __type(value, u32);
} programs SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, pid_t);
    __type(value, InterpreterInfo);
} pid_to_interpreter_info SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 12);
    __type(key, u32);
    __type(value, PythonVersionOffsets);
} version_specific_offsets SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 12);  // arbitrary
    __type(key, u32);
    __type(value, LibcOffsets);
} musl_offsets SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 12);  // arbitrary
    __type(key, u32);
    __type(value, LibcOffsets);
} glibc_offsets SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, State);
} global_state SEC(".maps");

//
//   ╔═════════════════════════════════════════════════════════════════════════╗
//   ║ Generic Helpers and Macros                                              ║
//   ╚═════════════════════════════════════════════════════════════════════════╝
//

#define GET_STATE()                                           \
    State *state = bpf_map_lookup_elem(&global_state, &zero); \
    if (state == NULL) {                                      \
        return 0;                                             \
    }

#define GET_OFFSETS()                                                                                                          \
    PythonVersionOffsets *offsets = bpf_map_lookup_elem(&version_specific_offsets, &state->interpreter_info.py_version_index); \
    if (offsets == NULL) {                                                                                                     \
        return 0;                                                                                                              \
    }

#define LOG(fmt, ...)                                  \
    ({                                                 \
        if (verbose) {                                 \
            bpf_printk("pyperf: " fmt, ##__VA_ARGS__); \
        }                                              \
    })

// Context for ERROR_SAMPLE.
static const int BPF_PROGRAM = PYTHON_UNWINDER_PROGRAM_ID;

// tls_read reads from the TLS associated with the provided key depending on the libc implementation.
static inline __attribute__((__always_inline__)) int tls_read(void *tls_base, InterpreterInfo *interpreter_info, void **out) {
    LibcOffsets *libc_offsets;
    void *tls_addr = NULL;
    int key = interpreter_info->tls_key;
    switch (interpreter_info->libc_implementation) {
        case LIBC_IMPLEMENTATION_GLIBC:
            // Read the offset from the corresponding map.
            libc_offsets = bpf_map_lookup_elem(&glibc_offsets, &interpreter_info->libc_offset_index);
            if (libc_offsets == NULL) {
                LOG("[error] libc_offsets for glibc is NULL");
                return -1;
            }
#if __TARGET_ARCH_x86

            tls_addr = tls_base + libc_offsets->pthread_block + (key * libc_offsets->pthread_key_data_size) + libc_offsets->pthread_key_data;
#elif __TARGET_ARCH_arm64
            tls_addr = tls_base - libc_offsets->pthread_size + libc_offsets->pthread_block + (key * libc_offsets->pthread_key_data_size) +
                       libc_offsets->pthread_key_data;
#else
#error "Unsupported platform"
#endif
            break;
        case LIBC_IMPLEMENTATION_MUSL:
            // Read the offset from the corresponding map.
            libc_offsets = bpf_map_lookup_elem(&musl_offsets, &interpreter_info->libc_offset_index);
            if (libc_offsets == NULL) {
                LOG("[error] libc_offsets for musl is NULL");
                return -1;
            }
#if __TARGET_ARCH_x86
            if (bpf_probe_read_user(&tls_addr, sizeof(tls_addr), tls_base + libc_offsets->pthread_block)) {
                return -1;
            }
            tls_addr = tls_addr + key * libc_offsets->pthread_key_data_size;
#elif __TARGET_ARCH_arm64
            if (bpf_probe_read_user(&tls_addr, sizeof(tls_addr), tls_base - libc_offsets->pthread_size + libc_offsets->pthread_block)) {
                return -1;
            }
            tls_addr = tls_addr + key * libc_offsets->pthread_key_data_size;
#else
#error "Unsupported platform"
#endif
            break;
        default:
            LOG("[error] unknown libc_implementation %d", interpreter_info->libc_implementation);
            return -1;
    }

    LOG("tls_read key %d from address 0x%lx", key, (unsigned long)tls_addr);
    if (bpf_probe_read(out, sizeof(*out), tls_addr)) {
        LOG("failed to read 0x%lx from TLS", (unsigned long)tls_addr);
        return -1;
    }
    return 0;
}

//
//   ╔═════════════════════════════════════════════════════════════════════════╗
//   ║ BPF Programs                                                            ║
//   ╚═════════════════════════════════════════════════════════════════════════╝
//
SEC("perf_event")
int unwind_python_stack(struct bpf_perf_event_data *ctx) {
    u64 zero = 0;
    unwind_state_t *unwind_state = bpf_map_lookup_elem(&heap, &zero);
    if (unwind_state == NULL) {
        LOG("[error] unwind_state is NULL, should not happen");
        return 1;
    }

    error_t *err_ctx = bpf_map_lookup_elem(&err_symbol, &zero);
    if (err_ctx == NULL) {
        LOG("[error] err_ctx is NULL!");
        return 1;
    }

    u64 pid_tgid = bpf_get_current_pid_tgid();
    pid_t pid = pid_tgid >> 32;
    pid_t tid = pid_tgid;
    if (pid == 0) {
        return 0;
    }

    InterpreterInfo *interpreter_info = bpf_map_lookup_elem(&pid_to_interpreter_info, &pid);
    if (!interpreter_info) {
        LOG("[error] interpreter_info is NULL, not a Python process or unknown Python version");
        ERROR_MSG(err_ctx, "interpreter_info was NULL");
        goto error;
    }

    LOG("[start]");
    LOG("[event] pid=%d tid=%d", pid, tid);

    GET_STATE();

    // Reset state.
    bpf_large_memzero((void *)state, sizeof(State));
    state->interpreter_info = *interpreter_info;

    state->sample.tid = tid;
    state->sample.pid = pid;
    state->sample.stack_status = STACK_COMPLETE;

    // TODO(kakkoyun): Implement stack bound checks.
    // state->stack.expected_size = (base_stack - cfp) / control_frame_t_sizeof;

    // Fetch thread state.

    if (interpreter_info->thread_state_addr != 0) {
        LOG("interpreter_info->thread_state_addr 0x%llx", interpreter_info->thread_state_addr);
        int err = bpf_probe_read_user(&state->thread_state, sizeof(state->thread_state), (void *)(long)interpreter_info->thread_state_addr);
        if (err != 0) {
            LOG("[error] failed to read interpreter_info->thread_state_addr with %d", err);
            ERROR_MSG(err_ctx, "failed read of thread_state_addr");
            goto submit_without_unwinding;
        }
        LOG("thread_state 0x%llx", state->thread_state);
    }

    if (interpreter_info->use_tls) {
        struct task_struct *task = (struct task_struct *)bpf_get_current_task();
        long unsigned int tls_base = read_tls_base(task);
        LOG("tls_base 0x%llx", (void *)tls_base);

        // TODO(kakkoyun): Read TLS key in here instead of user-space.
        // int key;
        // if (bpf_probe_read(&key, sizeof(key), interpreter_info->tls_key_addr)) {
        //   LOG("[error] failed to read TLS key from 0x%lx", (unsigned long)interpreter_info->tls_key_addr);
        //   goto submit_without_unwinding;
        // }
        if (tls_read((void *)tls_base, interpreter_info, &state->thread_state)) {
            LOG("[error] failed to read thread state from TLS 0x%lx", (unsigned long)interpreter_info->tls_key);
            ERROR_MSG(err_ctx, "failed read of TLS");
            goto submit_without_unwinding;
        }

        if (state->thread_state == 0) {
            LOG("[error] thread_state was NULL");
            ERROR_MSG(err_ctx, "thread_state was NULL");
            goto submit_without_unwinding;
        }
        LOG("thread_state 0x%llx", state->thread_state);
    }

    GET_OFFSETS();

    // Fetch the thread id.

    LOG("offsets->py_thread_state.thread_id %d", offsets->py_thread_state.thread_id);
    pthread_t pthread_id;
    if (bpf_probe_read_user(&pthread_id, sizeof(pthread_id), state->thread_state + offsets->py_thread_state.thread_id)) {
        LOG("[error] failed to read thread_state->thread_id");
        ERROR_MSG(err_ctx, "failed read of thread_state->thread_id");
        goto submit_without_unwinding;
    }

    LOG("pthread_id %lu", pthread_id);
    state->current_pthread = pthread_id;

    // Get pointer to top frame from PyThreadState.

    if (offsets->py_thread_state.frame > -1) {
        LOG("offsets->py_thread_state.frame %d", offsets->py_thread_state.frame);
        if (bpf_probe_read_user(&state->frame_ptr, sizeof(void *), state->thread_state + offsets->py_thread_state.frame)) {
            LOG("[error] failed to read thread_state->frame");
            ERROR_MSG(err_ctx, "failed read of thread_state->frame");
            goto submit_without_unwinding;
        }
    } else {
        LOG("offsets->py_thread_state.cframe %d", offsets->py_thread_state.cframe);
        void *cframe;
        if (bpf_probe_read_user(&cframe, sizeof(cframe), (void *)(state->thread_state + offsets->py_thread_state.cframe))) {
            LOG("[error] failed to read thread_state->cframe");
            ERROR_MSG(err_ctx, "failed read of thread_state->cframe");
            goto submit_without_unwinding;
        }
        if (cframe == 0) {
            LOG("[error] cframe was NULL");
            ERROR_MSG(err_ctx, "cframe was NULL");
            goto submit_without_unwinding;
        }
        LOG("cframe 0x%llx", cframe);

        LOG("offsets->py_cframe.current_frame %d", offsets->py_cframe.current_frame);
        bpf_probe_read_user(&state->frame_ptr, sizeof(state->frame_ptr), (void *)(cframe + offsets->py_cframe.current_frame));
    }
    if (state->frame_ptr == 0) {
        LOG("[error] frame_ptr was NULL");
        ERROR_MSG(err_ctx, "frame_ptr was NULL");
        goto submit_without_unwinding;
    }

    LOG("frame_ptr 0x%llx", state->frame_ptr);
    bpf_tail_call(ctx, &programs, PYPERF_STACK_WALKING_PROGRAM_IDX);

submit_without_unwinding:
    aggregate_stacks();
    LOG("[stop] submit_without_unwinding");
    return 0;

error:
    ERROR_SAMPLE(unwind_state, err_ctx);
    return 1;
}

static inline __attribute__((__always_inline__)) u32 read_symbol(PythonVersionOffsets *offsets, void *cur_frame, void *code_ptr, symbol_t *symbol) {
    // Figure out if we want to parse class name, basically checking the name of
    // the first argument.
    // If it's 'self', we get the type and it's name, if it's cls, we just get
    // the name. This is not perfect but there is no better way to figure this
    // out from the code object.
    // Everything we do in this function is best effort, we don't want to fail
    // the program if we can't read something.

    // GDB: ((PyTupleObject*)$frame->f_code->co_varnames)->ob_item[0]
    void *args_ptr;
    bpf_probe_read_user(&args_ptr, sizeof(void *), code_ptr + offsets->py_code_object.co_varnames);
    bpf_probe_read_user(&args_ptr, sizeof(void *), args_ptr + offsets->py_tuple_object.ob_item);
    bpf_probe_read_user_str(&symbol->method_name, sizeof(symbol->method_name), args_ptr + offsets->py_string.data);

    // Compare strings as ints to save instructions.
    char self_str[4] = {'s', 'e', 'l', 'f'};
    char cls_str[4] = {'c', 'l', 's', '\0'};
    bool first_self = *(s32 *)symbol->method_name == *(s32 *)self_str;
    bool first_cls = *(s32 *)symbol->method_name == *(s32 *)cls_str;

    // GDB: $frame->f_localsplus[0]->ob_type->tp_name.
    if (first_self || first_cls) {
        void *ptr;
        bpf_probe_read_user(&ptr, sizeof(void *), cur_frame + offsets->py_frame_object.f_localsplus);
        if (first_self) {
            // We are working with an instance, first we need to get type.
            bpf_probe_read_user(&ptr, sizeof(void *), ptr + offsets->py_object.ob_type);
        }
        bpf_probe_read_user(&ptr, sizeof(void *), ptr + offsets->py_type_object.tp_name);
        bpf_probe_read_user_str(&symbol->class_name, sizeof(symbol->class_name), ptr);
    }

    void *pystr_ptr;

    // GDB: $frame->f_code->co_filename
    bpf_probe_read_user(&pystr_ptr, sizeof(void *), code_ptr + offsets->py_code_object.co_filename);
    bpf_probe_read_user_str(&symbol->path, sizeof(symbol->path), pystr_ptr + offsets->py_string.data);

    // GDB: $frame->f_code->co_name
    bpf_probe_read_user(&pystr_ptr, sizeof(void *), code_ptr + offsets->py_code_object.co_name);
    bpf_probe_read_user_str(&symbol->method_name, sizeof(symbol->method_name), pystr_ptr + offsets->py_string.data);

    u32 lineno;
    // GDB: $frame->f_code->co_firstlineno
    bpf_probe_read_user(&lineno, sizeof(u32), code_ptr + offsets->py_code_object.co_firstlineno);
    return lineno;
}

static inline __attribute__((__always_inline__)) void reset_symbol(symbol_t *sym) {
    __builtin_memset((void *)sym, 0, sizeof(symbol_t));

    sym->class_name[0] = '\0';
    sym->method_name[0] = '\0';
    sym->path[0] = '\0';
}

SEC("perf_event")
int walk_python_stack(struct bpf_perf_event_data *ctx) {
    u64 zero = 0;
    GET_STATE();
    GET_OFFSETS();

    LOG("=====================================================\n");
    LOG("[start] walk_python_stack");
    state->stack_walker_prog_call_count++;
    Sample *sample = &state->sample;

    int frame_count = 0;
#pragma unroll
    for (int i = 0; i < PYTHON_STACK_FRAMES_PER_PROG; i++) {
        void *curr_frame_ptr = state->frame_ptr;
        if (!curr_frame_ptr) {
            break;
        }

        // https: // github.com/python/cpython/blob/de2a73dc4649b110351fce789de0abb14c460b97/Python/traceback.c#L980
        if (offsets->py_interpreter_frame.owner != -1) {
            int owner = 0;
            bpf_probe_read_user(&owner, sizeof(owner), (void *)(curr_frame_ptr + offsets->py_interpreter_frame.owner));
            if (owner == FRAME_OWNED_BY_CSTACK) {
                bpf_probe_read_user(&curr_frame_ptr, sizeof(curr_frame_ptr), curr_frame_ptr + offsets->py_frame_object.f_back);
            }
            if (!curr_frame_ptr) {
                break;
            }
        }

        // Read the code pointer. PyFrameObject.f_code
        void *curr_code_ptr;
        int err = bpf_probe_read_user(&curr_code_ptr, sizeof(curr_code_ptr), curr_frame_ptr + offsets->py_frame_object.f_code);
        if (err != 0) {
            LOG("[error] failed to read frame_ptr->f_code with %d", err);
            break;
        }
        if (!curr_code_ptr) {
            LOG("[error] cur_code_ptr was NULL");
            break;
        }

        LOG("## frame %d", frame_count);
        LOG("\tcur_frame_ptr 0x%llx", curr_frame_ptr);
        LOG("\tcur_code_ptr 0x%llx", curr_code_ptr);

        symbol_t sym = (symbol_t){0};
        reset_symbol(&sym);

        // Read symbol information from the code object if possible.
        u64 lineno = read_symbol(offsets, curr_frame_ptr, curr_code_ptr, &sym);

        LOG("\tsym.path %s", sym.path);
        LOG("\tsym.class_name %s", sym.class_name);
        LOG("\tsym.method_name %s", sym.method_name);
        LOG("\tsym.lineno %d", lineno);

        u64 symbol_id = get_symbol_id(&sym);
        u64 cur_len = sample->stack.len;
        if (cur_len >= 0 && cur_len < MAX_STACK_DEPTH) {
            LOG("\tstack->frames[%llu] = %llu", cur_len, symbol_id);
            sample->stack.addresses[cur_len] = (lineno << 32) | symbol_id;
            sample->stack.len++;
        }
        frame_count++;

        bpf_probe_read_user(&state->frame_ptr, sizeof(state->frame_ptr), curr_frame_ptr + offsets->py_frame_object.f_back);
        if (!state->frame_ptr) {
            // There aren't any frames to read. We are done.
            goto complete;
        }
    }
    LOG("[iteration] frame_count %d", frame_count);

    LOG("state->stack_walker_prog_call_count %d", state->stack_walker_prog_call_count);
    if (state->stack_walker_prog_call_count < PYTHON_STACK_PROG_CNT) {
        LOG("[continue] walk_python_stack");
        bpf_tail_call(ctx, &programs, PYPERF_STACK_WALKING_PROGRAM_IDX);
        goto submit;
    }

    // TODO(kakkoyun): Implement stack bound checks.
    // state->stack.stack_status = cfp > state->base_stack ? STACK_COMPLETE : STACK_INCOMPLETE;
    // if (state->stack.frames.len != state->stack.expected_size) {
    //     LOG("[error] stack size %d, expected %d", state->stack.frames.len, state->stack.expected_size);
    // }

    LOG("[error] walk_python_stack TRUNCATED");
    LOG("[truncated] walk_python_stack, stack_len=%d", sample->stack.len);
    state->sample.stack_status = STACK_TRUNCATED;
    goto submit;

complete:
    LOG("[complete] walk_python_stack, stack_len=%d", sample->stack.len);
    state->sample.stack_status = STACK_COMPLETE;
submit:
    LOG("[stop] walk_python_stack");

    // Hash stack.
    u64 stack_hash = hash_stack(&state->sample.stack, 0);
    LOG("[debug] stack hash: %d", stack_hash);

    // Insert stack.
    int err = bpf_map_update_elem(&stack_traces, &stack_hash, &state->sample.stack, BPF_ANY);
    if (err != 0) {
        LOG("[error] failed to insert stack_traces with %d", err);
    }

    unwind_state_t *unwind_state = bpf_map_lookup_elem(&heap, &zero);
    if (unwind_state != NULL) {
        unwind_state->stack_key.interpreter_stack_id = stack_hash;
    }

    // We are done.
    aggregate_stacks();
    LOG("[stop] submit");
    return 0;
}

//
//   ╔═════════════════════════════════════════════════════════════════════════╗
//   ║ Metadata                                                                ║
//   ╚═════════════════════════════════════════════════════════════════════════╝
//
#define KBUILD_MODNAME "py-perf"
volatile const char bpf_metadata_name[] SEC(".rodata") = "py-perf";
unsigned int VERSION SEC("version") = 1;
char LICENSE[] SEC("license") = "Dual MIT/GPL";
