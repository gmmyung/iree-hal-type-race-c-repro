# IREE Shared Instance / Context Concurrency Repro

This is a small C repro based on
[`iree-template-runtime-cmake`](https://github.com/iree-org/iree-template-runtime-cmake).

It stress tests this low-level runtime shape:

1. the process creates one `iree_vm_instance_t`
2. the process calls `iree_hal_module_register_all_types(instance)` once
3. each thread retains the shared VM instance
4. each thread creates its own HAL device, HAL module, bytecode module, VM
   context, function handle, VM lists, and buffer views
5. all threads repeatedly invoke the same exported VM function

No `iree_vm_context_t`, `iree_vm_function_t`, `iree_vm_list_t`, or
`iree_hal_device_t` is shared between threads.

## Build

```sh
git submodule update --init --recursive third_party/iree

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build --target shared_instance_context_repro
```

Compile the sample module with a matching IREE compiler:

```sh
iree-compile \
    --iree-hal-target-backends=llvm-cpu \
    simple_mul.mlir \
    -o build/simple_mul.vmfb
```

## Run

```sh
for i in $(seq 1 20); do
  echo "run=$i"
  ./build/shared_instance_context_repro \
      local-sync build/simple_mul.vmfb 1000 64 || exit $?
done
```

On my local machine this C repro currently passes even at higher stress levels:

```sh
./build/shared_instance_context_repro local-sync build/simple_mul.vmfb 1000 64
./build/shared_instance_context_repro local-sync build/simple_mul.vmfb 500 128
```

This is useful as a reference for checking whether failures seen from higher
level bindings are caused by IREE itself or by the binding layer.
