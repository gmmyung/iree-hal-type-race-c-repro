# IREE Concurrent Low-Level VM/HAL Stack Churn Repro

This is a small C repro based on
[`iree-template-runtime-cmake`](https://github.com/iree-org/iree-template-runtime-cmake).

It stress tests this low-level runtime shape:

1. create one process-wide `iree_vm_instance_t`
2. call `iree_hal_module_register_all_types(instance)` once
3. spawn worker threads
4. in every worker iteration, create a fresh HAL driver/device/module,
   bytecode module, VM context, function handle, VM lists, and buffer views
5. invoke once
6. destroy the whole stack

No `iree_vm_context_t`, `iree_vm_function_t`, `iree_vm_list_t`, or
`iree_hal_device_t` is shared between threads. HAL types are registered once on
the shared VM instance before spawning worker threads.

This reproduces intermittent aborts while independent per-thread stacks are
being created, invoked, and destroyed concurrently.

## Build

```sh
git submodule update --init --recursive third_party/iree

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build --target low_level_stack_churn_repro
```

Compile the sample module with a matching IREE compiler:

```sh
iree-compile \
    --iree-hal-target-backends=llvm-cpu \
    simple_mul.mlir \
    -o build/simple_mul.vmfb
```

The CMake config intentionally enables both the embedded ELF and VMVX executable
loaders. The repro command below uses the LLVM CPU/embedded ELF VMFB generated
from `simple_mul.mlir`.

Single-thread stack churn passes:

```sh
./build/low_level_stack_churn_repro local-sync build/simple_mul.vmfb 1000 1
```

The same stack churn usually fails locally with higher thread counts:

```sh
./build/low_level_stack_churn_repro local-sync build/simple_mul.vmfb 100 16
```

Observed failure mode:

```text
iree/runtime/src/iree/vm/ref.h:223: INVALID_ARGUMENT; ref type mismatch;
while invoking native function hal.device.query.i64; while calling import;

thread #N, stop reason = signal SIGABRT
iree_abort
iree_vm_buffer_deinitialize
iree_vm_bytecode_module_destroy
stack_deinitialize
stack_initialize
thread_main
```

In the captured run, other worker threads were concurrently inside
`iree_vm_context_create_with_modules` and `iree_vm_invoke`, but each thread had
its own VM context and HAL device.
