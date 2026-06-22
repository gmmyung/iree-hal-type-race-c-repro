# IREE HAL VM Ref Type Race Repro

This is a small C repro based on
[`iree-template-runtime-cmake`](https://github.com/iree-org/iree-template-runtime-cmake).

It stress tests concurrent creation and use of independent low-level IREE
runtime stacks:

1. each thread creates its own `iree_vm_instance_t`
2. each instance calls `iree_hal_module_register_all_types(instance)`
3. each thread creates its own HAL device, HAL module, bytecode module, VM
   context, and function handle
4. all threads invoke the same exported VM function repeatedly

No `iree_vm_context_t` or `iree_runtime_session_t` is shared across threads.

## Why This Looks Supported

The repro follows the low-level setup pattern used by
`samples/simple_embedding/simple_embedding.c`:

```c
iree_vm_instance_create(..., &instance);
iree_hal_module_register_all_types(instance);
iree_hal_module_create(..., &hal_module);
iree_vm_bytecode_module_create(..., &bytecode_module);
iree_vm_context_create_with_modules(..., &context);
iree_vm_invoke(...);
```

Relevant API comments in IREE `v3.11.0`:

- `iree_vm_instance_t` is documented as `Thread-safe`.
- `iree_runtime_instance_t` is documented as `Thread-safe` and describes
  separate instances as useful for multi-tenant isolation.
- `iree_runtime_session_t` and `iree_vm_context_t` are only
  thread-compatible, but this repro does not share either object between
  threads.

## Build

```sh
git submodule update --init --recursive third_party/iree

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build --target low_level_multi_instance_hal_repro
```

Compile the sample module with a matching IREE compiler:

```sh
iree-compile \
    --iree-hal-target-backends=llvm-cpu \
    simple_mul.mlir \
    -o build/simple_mul.vmfb
```

## Run

Low thread counts usually pass:

```sh
./build/low_level_multi_instance_hal_repro \
    local-sync build/simple_mul.vmfb 1000 8
```

Example output:

```text
completed 1000 low-level calls on each of 8 threads
```

Higher thread counts reproduce intermittent failures on my machine. This failed
on run 4 of the loop below:

```sh
for i in $(seq 1 20); do
  echo "run=$i"
  ./build/low_level_multi_instance_hal_repro \
      local-sync build/simple_mul.vmfb 1000 128 || exit $?
done
```

Observed failures:

```text
run=4
thread 78 failed:
iree/runtime/src/iree/vm/ref.h:223: INVALID_ARGUMENT; ref is null; while invoking native function hal.command_buffer.dispatch; while calling import;
[ 1] bytecode module.__init:184 -
[ 0] bytecode module@2:1012 -
thread 100 failed:
iree/runtime/src/iree/vm/ref.h:223: INVALID_ARGUMENT; ref type mismatch; while invoking native function hal.command_buffer.create; while calling import;
[ 1] bytecode module.__init:110 -
[ 0] bytecode module@2:1012 -
```

Other observed failures from the same repro:

```text
thread 59 failed:
iree/runtime/src/iree/vm/ref.h:223: INVALID_ARGUMENT; ref type mismatch; while invoking native function hal.device.query.i64; while calling import;
[ 0] bytecode module@2:268 -
```

```text
thread 17 failed:
iree/runtime/src/iree/vm/ref.h:223: INVALID_ARGUMENT; ref type mismatch; while invoking native function hal.executable.create; while calling import;
[ 0] bytecode module@2:990 -
```

The failure is intermittent. Running the command a few times may be needed.

## Question

Is this usage expected to be supported? If not, what is the intended threading
and instance ownership model for low-level VM + HAL setup?
