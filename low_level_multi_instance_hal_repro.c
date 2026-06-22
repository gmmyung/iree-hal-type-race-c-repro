// Minimal C repro for concurrent HAL VM type registration/use across multiple
// independent iree_vm_instance_t objects.
//
// Usage:
//   low_level_multi_instance_hal_repro local-sync simple_mul.vmfb [iterations]
//                                      [thread_count]
//
// Each thread owns its VM instance, HAL module, bytecode module, VM context,
// and function handle. No VM context/session is shared across threads.

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

typedef struct repro_session_t {
  iree_vm_instance_t* instance;
  iree_hal_device_t* device;
  iree_vm_module_t* hal_module;
  iree_vm_module_t* bytecode_module;
  iree_vm_context_t* context;
  iree_vm_function_t function;
} repro_session_t;

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

static void repro_session_deinitialize(repro_session_t* session) {
  if (session->context) iree_vm_context_release(session->context);
  if (session->bytecode_module)
    iree_vm_module_release(session->bytecode_module);
  if (session->hal_module) iree_vm_module_release(session->hal_module);
  if (session->device) iree_hal_device_release(session->device);
  if (session->instance) iree_vm_instance_release(session->instance);
  memset(session, 0, sizeof(*session));
}

static iree_status_t read_file(const char* path, iree_allocator_t allocator,
                               iree_const_byte_span_t* out_contents) {
  memset(out_contents, 0, sizeof(*out_contents));

  FILE* file = fopen(path, "rb");
  if (!file) {
    return iree_make_status(IREE_STATUS_NOT_FOUND, "failed to open %s", path);
  }

  uint8_t* data = NULL;
  iree_status_t status = iree_ok_status();
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

  *out_contents = iree_make_const_byte_span(data, (iree_host_size_t)file_size);
  return iree_ok_status();
}

static iree_status_t repro_session_initialize(const char* device_uri,
                                              const char* module_path,
                                              repro_session_t* out) {
  memset(out, 0, sizeof(*out));
  iree_allocator_t host_allocator = iree_allocator_system();

  iree_status_t status = iree_vm_instance_create(
      IREE_VM_TYPE_CAPACITY_DEFAULT, host_allocator, &out->instance);
  if (iree_status_is_ok(status)) {
    status = iree_hal_module_register_all_types(out->instance);
  }

  iree_hal_driver_registry_t* registry = iree_hal_driver_registry_default();
  if (iree_status_is_ok(status)) {
    status =
        iree_hal_create_device(registry, iree_make_cstring_view(device_uri),
                               host_allocator, &out->device);
  }

  if (iree_status_is_ok(status)) {
    iree_hal_device_group_t* device_group = NULL;
    status = iree_hal_device_group_create_from_device(
        out->device, host_allocator, &device_group);
    if (iree_status_is_ok(status)) {
      status = iree_hal_module_create(
          out->instance, iree_hal_module_device_policy_default(), device_group,
          IREE_HAL_MODULE_FLAG_SYNCHRONOUS,
          iree_hal_module_debug_sink_stdio(stderr), host_allocator,
          &out->hal_module);
    }
    iree_hal_device_group_release(device_group);
  }

  iree_const_byte_span_t module_data = iree_make_const_byte_span(NULL, 0);
  if (iree_status_is_ok(status)) {
    status = read_file(module_path, host_allocator, &module_data);
  }
  if (iree_status_is_ok(status)) {
    status = iree_vm_bytecode_module_create(
        out->instance, IREE_VM_BYTECODE_MODULE_FLAG_NONE, module_data,
        host_allocator, host_allocator, &out->bytecode_module);
    module_data.data = NULL;
  }
  if (module_data.data) {
    iree_allocator_free(host_allocator, (void*)module_data.data);
  }

  if (iree_status_is_ok(status)) {
    iree_vm_module_t* modules[2] = {out->hal_module, out->bytecode_module};
    status = iree_vm_context_create_with_modules(
        out->instance, IREE_VM_CONTEXT_FLAG_NONE, IREE_ARRAYSIZE(modules),
        modules, host_allocator, &out->context);
  }
  if (iree_status_is_ok(status)) {
    status = iree_vm_context_resolve_function(
        out->context, iree_make_cstring_view("module.simple_mul"),
        &out->function);
  }

  if (!iree_status_is_ok(status)) {
    repro_session_deinitialize(out);
  }
  return status;
}

static iree_status_t push_input(repro_session_t* session,
                                iree_vm_list_t* inputs, const float data[4]) {
  static const iree_hal_dim_t shape[1] = {4};
  iree_hal_buffer_view_t* view = NULL;
  iree_status_t status = iree_hal_buffer_view_allocate_buffer_copy(
      session->device, iree_hal_device_allocator(session->device),
      IREE_ARRAYSIZE(shape), shape, IREE_HAL_ELEMENT_TYPE_FLOAT_32,
      IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
      (iree_hal_buffer_params_t){
          .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
          .access = IREE_HAL_MEMORY_ACCESS_ALL,
          .usage = IREE_HAL_BUFFER_USAGE_DEFAULT,
      },
      iree_make_const_byte_span(data, sizeof(float) * 4), &view);

  if (iree_status_is_ok(status)) {
    // Avoid using the process-global iree_hal_buffer_view_move_ref helper for
    // test inputs. This keeps user-provided refs tied to the owning VM instance
    // and leaves the observed mismatch to the HAL native module internals.
    iree_vm_ref_type_t buffer_view_type = iree_vm_instance_lookup_type(
        session->instance, iree_make_cstring_view("hal.buffer_view"));
    iree_vm_ref_t ref = iree_vm_ref_null();
    status = iree_vm_ref_wrap_retain(view, buffer_view_type, &ref);
    if (iree_status_is_ok(status)) {
      status = iree_vm_list_push_ref_retain(inputs, &ref);
    }
    iree_vm_ref_release(&ref);
  }
  iree_hal_buffer_view_release(view);
  return status;
}

static iree_status_t run_simple_mul(repro_session_t* session, int iteration,
                                    const char* label) {
  iree_allocator_t host_allocator = iree_allocator_system();
  iree_vm_list_t* inputs = NULL;
  iree_vm_list_t* outputs = NULL;
  iree_status_t status = iree_vm_list_create(iree_vm_make_undefined_type_def(),
                                             2, host_allocator, &inputs);
  if (iree_status_is_ok(status)) {
    status = iree_vm_list_create(iree_vm_make_undefined_type_def(), 1,
                                 host_allocator, &outputs);
  }

  const float lhs[4] = {1.0f, 1.1f, 1.2f, 1.3f};
  const float rhs[4] = {10.0f, 100.0f, 1000.0f, 10000.0f};
  if (iree_status_is_ok(status)) status = push_input(session, inputs, lhs);
  if (iree_status_is_ok(status)) status = push_input(session, inputs, rhs);
  if (iree_status_is_ok(status)) {
    status = iree_vm_invoke(session->context, session->function,
                            IREE_VM_INVOCATION_FLAG_NONE, NULL, inputs, outputs,
                            host_allocator);
  }

  iree_hal_buffer_view_t* result = NULL;
  iree_vm_ref_t result_ref = iree_vm_ref_null();
  if (iree_status_is_ok(status)) {
    status = iree_vm_list_get_ref_retain(outputs, 0, &result_ref);
    if (iree_status_is_ok(status)) {
      status = iree_hal_buffer_view_check_deref(result_ref, &result);
    }
  }
  if (iree_status_is_ok(status)) {
    float values[4] = {0};
    status = iree_hal_device_transfer_d2h(
        session->device, iree_hal_buffer_view_buffer(result), 0, values,
        sizeof(values), IREE_HAL_TRANSFER_BUFFER_FLAG_DEFAULT,
        iree_infinite_timeout());
    if (iree_status_is_ok(status) &&
        (values[0] != 10.0f || values[1] != 110.0f || values[2] != 1200.0f ||
         values[3] != 13000.0f)) {
      status = iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT, "%s iteration %d produced %g %g %g %g",
          label, iteration, values[0], values[1], values[2], values[3]);
    }
  }
  iree_vm_ref_release(&result_ref);
  if (outputs) iree_vm_list_release(outputs);
  if (inputs) iree_vm_list_release(inputs);
  return status;
}

typedef struct thread_args_t {
  const char* device_uri;
  const char* module_path;
  int iterations;
  int index;
  barrier_t* init_barrier;
  barrier_t* invoke_barrier;
  iree_status_t status;
} thread_args_t;

static void* thread_main(void* raw_args) {
  thread_args_t* args = (thread_args_t*)raw_args;
  repro_session_t session;
  memset(&session, 0, sizeof(session));
  if (!barrier_wait(args->init_barrier)) {
    args->status = iree_make_status(IREE_STATUS_CANCELLED, "barrier cancelled");
    return NULL;
  }

  args->status =
      repro_session_initialize(args->device_uri, args->module_path, &session);
  if (!barrier_wait(args->invoke_barrier)) {
    repro_session_deinitialize(&session);
    return NULL;
  }

  char label[32];
  snprintf(label, sizeof(label), "T%d", args->index);
  for (int i = 0; i < args->iterations && iree_status_is_ok(args->status);
       ++i) {
    args->status = run_simple_mul(&session, i, label);
  }
  repro_session_deinitialize(&session);
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: low_level_multi_instance_hal_repro device module.vmfb "
            "[iterations] [thread_count]\n");
    return 1;
  }
  int iterations = argc >= 4 ? atoi(argv[3]) : 1000;
  if (iterations <= 0) iterations = 1;
  int thread_count = argc >= 5 ? atoi(argv[4]) : 1;
  if (thread_count <= 0) thread_count = 1;

  iree_status_t status = iree_hal_register_all_available_drivers(
      iree_hal_driver_registry_default());
  if (!iree_status_is_ok(status)) {
    iree_status_fprint(stderr, status);
    iree_status_ignore(status);
    return 1;
  }

  barrier_t init_barrier;
  barrier_t invoke_barrier;
  barrier_initialize(&init_barrier, thread_count);
  barrier_initialize(&invoke_barrier, thread_count);

  pthread_t* threads = calloc((size_t)thread_count, sizeof(*threads));
  thread_args_t* args = calloc((size_t)thread_count, sizeof(*args));
  if (!threads || !args) {
    fprintf(stderr, "failed to allocate thread state\n");
    free(args);
    free(threads);
    barrier_deinitialize(&invoke_barrier);
    barrier_deinitialize(&init_barrier);
    return 1;
  }

  int created_count = 0;
  for (int i = 0; i < thread_count; ++i) {
    args[i] = (thread_args_t){
        .device_uri = argv[1],
        .module_path = argv[2],
        .iterations = iterations,
        .index = i,
        .init_barrier = &init_barrier,
        .invoke_barrier = &invoke_barrier,
        .status = iree_ok_status(),
    };
    int rc = pthread_create(&threads[i], NULL, thread_main, &args[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create failed for thread %d: %s\n", i,
              strerror(rc));
      barrier_cancel(&init_barrier);
      barrier_cancel(&invoke_barrier);
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
  barrier_deinitialize(&invoke_barrier);
  barrier_deinitialize(&init_barrier);

  if (ret) return ret;
  fprintf(stdout, "completed %d low-level calls on each of %d threads\n",
          iterations, thread_count);
  return 0;
}
