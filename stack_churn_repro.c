// Repro for concurrent low-level VM/HAL stack churn:
//
//   one shared VM instance
//   HAL types registered once before spawning threads
//   each thread repeatedly creates an independent low-level stack
//   each stack is invoked once and then destroyed
//
// Usage:
//   low_level_stack_churn_repro local-sync simple_mul.vmfb [iterations]
//                               [thread_count]

#include <iree/base/api.h>
#include <iree/hal/api.h>
#include <iree/hal/device_group.h>
#include <iree/hal/drivers/init.h>
#include <iree/modules/hal/module.h>
#include <iree/vm/api.h>
#include <iree/vm/bytecode/module.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const iree_hal_dim_t kElementCount = 4;

typedef struct barrier_t {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int total;
  int arrived;
  int generation;
  bool cancelled;
} barrier_t;

static void barrier_initialize(barrier_t* barrier, int total) {
  pthread_mutex_init(&barrier->mutex, NULL);
  pthread_cond_init(&barrier->cond, NULL);
  barrier->total = total;
  barrier->arrived = 0;
  barrier->generation = 0;
  barrier->cancelled = false;
}

static void barrier_deinitialize(barrier_t* barrier) {
  pthread_cond_destroy(&barrier->cond);
  pthread_mutex_destroy(&barrier->mutex);
}

static void barrier_cancel(barrier_t* barrier) {
  pthread_mutex_lock(&barrier->mutex);
  barrier->cancelled = true;
  ++barrier->generation;
  pthread_cond_broadcast(&barrier->cond);
  pthread_mutex_unlock(&barrier->mutex);
}

static bool barrier_wait(barrier_t* barrier) {
  pthread_mutex_lock(&barrier->mutex);
  int generation = barrier->generation;
  ++barrier->arrived;
  if (barrier->arrived == barrier->total) {
    barrier->arrived = 0;
    ++barrier->generation;
    pthread_cond_broadcast(&barrier->cond);
  } else {
    while (!barrier->cancelled && generation == barrier->generation) {
      pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
  }
  bool cancelled = barrier->cancelled;
  pthread_mutex_unlock(&barrier->mutex);
  return !cancelled;
}

typedef struct bytecode_file_t {
  iree_const_byte_span_t data;
  iree_allocator_t allocator;
} bytecode_file_t;

static void bytecode_file_deinitialize(bytecode_file_t* file) {
  if (file->data.data) {
    iree_allocator_free(file->allocator, (void*)file->data.data);
  }
  memset(file, 0, sizeof(*file));
}

static iree_status_t bytecode_file_initialize(const char* path,
                                              iree_allocator_t allocator,
                                              bytecode_file_t* out_file) {
  memset(out_file, 0, sizeof(*out_file));
  out_file->allocator = allocator;

  FILE* file = fopen(path, "rb");
  if (!file) {
    return iree_make_status(IREE_STATUS_NOT_FOUND, "failed to open %s", path);
  }

  iree_status_t status = iree_ok_status();
  uint8_t* data = NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    status = iree_make_status(IREE_STATUS_INTERNAL, "failed to seek %s", path);
  }
  long file_size = 0;
  if (iree_status_is_ok(status)) {
    file_size = ftell(file);
    if (file_size < 0) {
      status =
          iree_make_status(IREE_STATUS_INTERNAL, "failed to tell %s", path);
    }
  }
  if (iree_status_is_ok(status) && fseek(file, 0, SEEK_SET) != 0) {
    status =
        iree_make_status(IREE_STATUS_INTERNAL, "failed to rewind %s", path);
  }
  if (iree_status_is_ok(status)) {
    status = iree_allocator_malloc(allocator, (iree_host_size_t)file_size,
                                   (void**)&data);
  }
  if (iree_status_is_ok(status) &&
      fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
    status = iree_make_status(IREE_STATUS_INTERNAL, "failed to read %s", path);
  }
  fclose(file);

  if (!iree_status_is_ok(status)) {
    iree_allocator_free(allocator, data);
    return status;
  }

  out_file->data = iree_make_const_byte_span(data, (iree_host_size_t)file_size);
  return iree_ok_status();
}

typedef struct runtime_stack_t {
  iree_vm_instance_t* instance;
  iree_hal_driver_registry_t* registry;
  iree_hal_driver_t* driver;
  iree_hal_device_t* device;
  iree_vm_module_t* hal_module;
  iree_vm_module_t* bytecode_module;
  iree_vm_context_t* context;
  iree_vm_function_t function;
} runtime_stack_t;

static void stack_deinitialize(runtime_stack_t* stack) {
  if (stack->context) iree_vm_context_release(stack->context);
  if (stack->bytecode_module) iree_vm_module_release(stack->bytecode_module);
  if (stack->hal_module) iree_vm_module_release(stack->hal_module);
  if (stack->device) iree_hal_device_release(stack->device);
  if (stack->driver) iree_hal_driver_release(stack->driver);
  if (stack->registry) iree_hal_driver_registry_free(stack->registry);
  if (stack->instance) iree_vm_instance_release(stack->instance);
  memset(stack, 0, sizeof(*stack));
}

static iree_status_t stack_initialize(
    iree_vm_instance_t* shared_instance, iree_string_view_t device_uri,
    iree_const_byte_span_t bytecode_data, runtime_stack_t* out_stack) {
  memset(out_stack, 0, sizeof(*out_stack));
  iree_allocator_t host_allocator = iree_allocator_system();

  iree_vm_instance_retain(shared_instance);
  out_stack->instance = shared_instance;

  iree_status_t status =
      iree_hal_driver_registry_allocate(host_allocator, &out_stack->registry);
  if (iree_status_is_ok(status)) {
    status = iree_hal_register_all_available_drivers(out_stack->registry);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_driver_registry_try_create(
        out_stack->registry, device_uri, host_allocator, &out_stack->driver);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_driver_create_default_device(
        out_stack->driver, host_allocator, &out_stack->device);
  }
  if (iree_status_is_ok(status)) {
    iree_hal_device_group_t* device_group = NULL;
    status = iree_hal_device_group_create_from_device(
        out_stack->device, host_allocator, &device_group);
    if (iree_status_is_ok(status)) {
      status = iree_hal_module_create(
          out_stack->instance, iree_hal_module_device_policy_default(),
          device_group, IREE_HAL_MODULE_FLAG_SYNCHRONOUS,
          iree_hal_module_debug_sink_null(), host_allocator,
          &out_stack->hal_module);
    }
    iree_hal_device_group_release(device_group);
  }
  if (iree_status_is_ok(status)) {
    status = iree_vm_bytecode_module_create(
        out_stack->instance, IREE_VM_BYTECODE_MODULE_FLAG_NONE, bytecode_data,
        iree_allocator_null(), host_allocator, &out_stack->bytecode_module);
  }
  if (iree_status_is_ok(status)) {
    iree_vm_module_t* modules[2] = {out_stack->hal_module,
                                    out_stack->bytecode_module};
    status = iree_vm_context_create_with_modules(
        out_stack->instance, IREE_VM_CONTEXT_FLAG_NONE, IREE_ARRAYSIZE(modules),
        modules, host_allocator, &out_stack->context);
  }
  if (iree_status_is_ok(status)) {
    status = iree_vm_context_resolve_function(
        out_stack->context, iree_make_cstring_view("module.simple_mul"),
        &out_stack->function);
  }

  if (!iree_status_is_ok(status)) {
    stack_deinitialize(out_stack);
  }
  return status;
}

static iree_status_t push_f32_input(runtime_stack_t* stack,
                                    iree_vm_list_t* inputs, const float* values,
                                    iree_hal_dim_t element_count) {
  const iree_hal_dim_t shape[1] = {element_count};
  iree_hal_buffer_view_t* view = NULL;
  iree_status_t status = iree_hal_buffer_view_allocate_buffer_copy(
      stack->device, iree_hal_device_allocator(stack->device),
      IREE_ARRAYSIZE(shape), shape, IREE_HAL_ELEMENT_TYPE_FLOAT_32,
      IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
      (iree_hal_buffer_params_t){
          .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
          .access = IREE_HAL_MEMORY_ACCESS_ALL,
          .usage = IREE_HAL_BUFFER_USAGE_DEFAULT,
      },
      iree_make_const_byte_span(values, sizeof(float) * element_count), &view);

  iree_vm_ref_t ref = iree_vm_ref_null();
  if (iree_status_is_ok(status)) {
    ref = iree_hal_buffer_view_retain_ref(view);
    status = iree_vm_list_push_ref_move(inputs, &ref);
  }
  iree_hal_buffer_view_release(view);
  return status;
}

static iree_status_t invoke_once(runtime_stack_t* stack, int thread_index,
                                 int iteration) {
  iree_allocator_t host_allocator = iree_allocator_system();
  iree_vm_list_t* inputs = NULL;
  iree_vm_list_t* outputs = NULL;
  iree_status_t status = iree_vm_list_create(iree_vm_make_undefined_type_def(),
                                             2, host_allocator, &inputs);
  if (iree_status_is_ok(status)) {
    status = iree_vm_list_create(iree_vm_make_undefined_type_def(), 1,
                                 host_allocator, &outputs);
  }

  float* lhs = NULL;
  float* rhs = NULL;
  if (iree_status_is_ok(status)) {
    lhs = calloc((size_t)kElementCount, sizeof(*lhs));
    rhs = calloc((size_t)kElementCount, sizeof(*rhs));
    if (!lhs || !rhs) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "failed to allocate input buffers");
    }
  }
  if (iree_status_is_ok(status)) {
    for (iree_hal_dim_t i = 0; i < kElementCount; ++i) {
      lhs[i] = (float)i;
      rhs[i] = (float)i;
    }
  }
  if (iree_status_is_ok(status)) {
    status = push_f32_input(stack, inputs, lhs, kElementCount);
  }
  if (iree_status_is_ok(status)) {
    status = push_f32_input(stack, inputs, rhs, kElementCount);
  }
  if (iree_status_is_ok(status)) {
    status = iree_vm_invoke(stack->context, stack->function,
                            IREE_VM_INVOCATION_FLAG_NONE, NULL, inputs, outputs,
                            host_allocator);
  }

  iree_vm_ref_t result_ref = iree_vm_ref_null();
  iree_hal_buffer_view_t* result = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_vm_list_get_ref_retain(outputs, 0, &result_ref);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_buffer_view_check_deref(result_ref, &result);
  }
  if (iree_status_is_ok(status)) {
    float* result_values = calloc((size_t)kElementCount, sizeof(*result_values));
    if (!result_values) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "failed to allocate result buffer");
    }
    if (iree_status_is_ok(status)) {
      status = iree_hal_device_transfer_d2h(
          stack->device, iree_hal_buffer_view_buffer(result), 0, result_values,
          sizeof(float) * kElementCount, IREE_HAL_TRANSFER_BUFFER_FLAG_DEFAULT,
          iree_infinite_timeout());
    }
    for (iree_hal_dim_t i = 0; i < kElementCount && iree_status_is_ok(status);
         ++i) {
      float expected = (float)(i * i);
      if (result_values[i] != expected) {
        status = iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "thread %d iteration %d element %zu produced %g", thread_index,
            iteration, (size_t)i, result_values[i]);
      }
    }
    free(result_values);
  }

  free(rhs);
  free(lhs);
  iree_vm_ref_release(&result_ref);
  if (outputs) iree_vm_list_release(outputs);
  if (inputs) iree_vm_list_release(inputs);
  return status;
}

typedef struct thread_args_t {
  iree_vm_instance_t* shared_instance;
  iree_string_view_t device_uri;
  iree_const_byte_span_t bytecode_data;
  int iterations;
  int index;
  barrier_t* start_barrier;
  iree_status_t status;
} thread_args_t;

static void* thread_main(void* raw_args) {
  thread_args_t* args = (thread_args_t*)raw_args;
  if (!barrier_wait(args->start_barrier)) {
    args->status = iree_make_status(IREE_STATUS_CANCELLED, "barrier failed");
    return NULL;
  }

  for (int i = 0; i < args->iterations && iree_status_is_ok(args->status);
       ++i) {
    runtime_stack_t stack;
    args->status = stack_initialize(args->shared_instance, args->device_uri,
                                    args->bytecode_data, &stack);
    if (iree_status_is_ok(args->status)) {
      args->status = invoke_once(&stack, args->index, i);
    }
    stack_deinitialize(&stack);
  }
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: low_level_stack_churn_repro device module.vmfb "
            "[iterations] [thread_count]\n");
    return 1;
  }

  int iterations = argc >= 4 ? atoi(argv[3]) : 100;
  if (iterations <= 0) iterations = 1;
  int thread_count = argc >= 5 ? atoi(argv[4]) : 16;
  if (thread_count <= 0) thread_count = 1;

  iree_allocator_t host_allocator = iree_allocator_system();
  iree_vm_instance_t* shared_instance = NULL;
  iree_status_t status = iree_vm_instance_create(
      IREE_VM_TYPE_CAPACITY_DEFAULT, host_allocator, &shared_instance);
  if (iree_status_is_ok(status)) {
    status = iree_hal_module_register_all_types(shared_instance);
  }

  bytecode_file_t bytecode_file;
  memset(&bytecode_file, 0, sizeof(bytecode_file));
  if (iree_status_is_ok(status)) {
    status = bytecode_file_initialize(argv[2], host_allocator, &bytecode_file);
  }

  if (!iree_status_is_ok(status)) {
    iree_status_fprint(stderr, status);
    iree_status_ignore(status);
    if (shared_instance) iree_vm_instance_release(shared_instance);
    bytecode_file_deinitialize(&bytecode_file);
    return 1;
  }

  barrier_t start_barrier;
  barrier_initialize(&start_barrier, thread_count);

  pthread_t* threads = calloc((size_t)thread_count, sizeof(*threads));
  thread_args_t* args = calloc((size_t)thread_count, sizeof(*args));
  if (!threads || !args) {
    fprintf(stderr, "failed to allocate thread state\n");
    free(args);
    free(threads);
    barrier_deinitialize(&start_barrier);
    bytecode_file_deinitialize(&bytecode_file);
    iree_vm_instance_release(shared_instance);
    return 1;
  }

  int created_count = 0;
  for (int i = 0; i < thread_count; ++i) {
    args[i] = (thread_args_t){
        .shared_instance = shared_instance,
        .device_uri = iree_make_cstring_view(argv[1]),
        .bytecode_data = bytecode_file.data,
        .iterations = iterations,
        .index = i,
        .start_barrier = &start_barrier,
        .status = iree_ok_status(),
    };
    int rc = pthread_create(&threads[i], NULL, thread_main, &args[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create failed for thread %d: %s\n", i,
              strerror(rc));
      barrier_cancel(&start_barrier);
      break;
    }
    ++created_count;
  }

  int ret = created_count == thread_count ? 0 : 1;
  for (int i = 0; i < created_count; ++i) {
    pthread_join(threads[i], NULL);
    if (!iree_status_is_ok(args[i].status)) {
      fprintf(stderr, "thread %d failed:\n", i);
      iree_status_fprint(stderr, args[i].status);
      iree_status_ignore(args[i].status);
      ret = 1;
    }
  }

  free(args);
  free(threads);
  barrier_deinitialize(&start_barrier);
  bytecode_file_deinitialize(&bytecode_file);
  iree_vm_instance_release(shared_instance);

  if (ret) return ret;
  fprintf(stdout,
          "completed %d create/invoke/destroy iterations on each of %d "
          "threads with one shared VM instance\n",
          iterations, thread_count);
  return 0;
}
