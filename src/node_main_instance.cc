#include "../deps/uv/src/uv-tape.h"
#include "node_main_instance.h"
#include <memory>
#if HAVE_OPENSSL
#include "crypto/crypto_util.h"
#endif  // HAVE_OPENSSL
#include "debug_utils-inl.h"
#include "node_builtins.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "node_options-inl.h"
#include "node_realm.h"
#include "node_sea.h"
#include "node_snapshot_builder.h"
#include "node_snapshotable.h"
#include "node_v8_platform-inl.h"
#include "util-inl.h"
#if defined(LEAK_SANITIZER)
#include <sanitizer/lsan_interface.h>
#endif

#if HAVE_INSPECTOR
#include "inspector/worker_inspector.h"  // ParentInspectorHandle
#endif

namespace node {

using v8::Context;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Locker;

NodeMainInstance::NodeMainInstance(const SnapshotData* snapshot_data,
                                   uv_loop_t* event_loop,
                                   MultiIsolatePlatform* platform,
                                   const std::vector<std::string>& args,
                                   const std::vector<std::string>& exec_args)
    : args_(args),
      exec_args_(exec_args),
      array_buffer_allocator_(ArrayBufferAllocator::Create()),
      isolate_(nullptr),
      platform_(platform),
      isolate_data_(),
      isolate_params_(std::make_unique<Isolate::CreateParams>()),
      snapshot_data_(snapshot_data) {
  isolate_params_->array_buffer_allocator = array_buffer_allocator_.get();

  isolate_ =
      NewIsolate(isolate_params_.get(), event_loop, platform, snapshot_data);
  CHECK_NOT_NULL(isolate_);

  // If the indexes are not nullptr, we are not deserializing
  isolate_data_.reset(
      CreateIsolateData(isolate_,
                        event_loop,
                        platform,
                        array_buffer_allocator_.get(),
                        snapshot_data->AsEmbedderWrapper().get()));

  isolate_data_->max_young_gen_size =
      isolate_params_->constraints.max_young_generation_size_in_bytes();
}

NodeMainInstance::~NodeMainInstance() {
  if (isolate_params_ == nullptr) {
    return;
  }

  {
#ifdef DEBUG
    // node::Environment has been disposed and no JavaScript Execution is
    // allowed at this point.
    // Create a scope to check that no JavaScript is executed in debug build
    // and proactively crash the process in the case JavaScript is being
    // executed.
    // Isolate::Dispose() must be invoked outside of this scope to avoid
    // use-after-free.
    Isolate::DisallowJavascriptExecutionScope disallow_js(
        isolate_, Isolate::DisallowJavascriptExecutionScope::CRASH_ON_FAILURE);
#endif
    // This should only be done on a main instance that owns its isolate.
    // IsolateData must be freed before UnregisterIsolate() is called.
    isolate_data_.reset();
  }
  platform_->DisposeIsolate(isolate_);
}

ExitCode NodeMainInstance::Run() {
  Locker locker(isolate_);
  Isolate::Scope isolate_scope(isolate_);
  HandleScope handle_scope(isolate_);

  ExitCode exit_code = ExitCode::kNoFailure;
  DeleteFnPtr<Environment, FreeEnvironment> env =
      CreateMainEnvironment(&exit_code);
  CHECK_NOT_NULL(env);

  Context::Scope context_scope(env->context());
  Run(&exit_code, env.get());
  return exit_code;
}

void NodeMainInstance::Run(ExitCode* exit_code, Environment* env) {
  if (*exit_code == ExitCode::kNoFailure) {
    // --tape-inspect is a static read: there is no program to run.
    if (!per_process::cli_options->tape_inspect.empty()) {
      uv_tape_inspect(per_process::cli_options->tape_inspect.c_str());
      *exit_code = ExitCode::kNoFailure;
      return;
    }
    // Arm the tape before the program runs. Startup IO (the module loader, the
    // compile cache) is paused separately; see NODEJS.md.
    if (!per_process::cli_options->tape_record.empty())
      uv_tape_record_to(per_process::cli_options->tape_record.c_str());
    else if (!per_process::cli_options->tape_replay.empty())
      uv_tape_replay_from(per_process::cli_options->tape_replay.c_str());

    if (!sea::MaybeLoadSingleExecutableApplication(env)) {
      LoadEnvironment(env, StartExecutionCallbackWithModule{});
    }

    *exit_code =
        SpinEventLoopInternal(env).FromMaybe(ExitCode::kGenericUserError);

    // Seal the tape once the loop has drained and the program is done.
    if (UV_TAPE_ACTIVE())
      uv_tape_finish(static_cast<int>(*exit_code));
  }

#if defined(LEAK_SANITIZER)
  __lsan_do_leak_check();
#endif
}

DeleteFnPtr<Environment, FreeEnvironment>
NodeMainInstance::CreateMainEnvironment(ExitCode* exit_code) {
  *exit_code = ExitCode::kNoFailure;  // Reset the exit code to 0

  HandleScope handle_scope(isolate_);

  // TODO(addaleax): This should load a real per-Isolate option, currently
  // this is still effectively per-process.
  if (isolate_data_->options()->track_heap_objects) {
    isolate_->GetHeapProfiler()->StartTrackingHeapObjects(true);
  }

  Local<Context> context;
  DeleteFnPtr<Environment, FreeEnvironment> env;

  if (snapshot_data_ != nullptr) {
    env.reset(CreateEnvironment(isolate_data_.get(),
                                Local<Context>(),  // read from snapshot
                                args_,
                                exec_args_));
#if HAVE_OPENSSL
    crypto::InitCryptoOnce(isolate_);
#endif  // HAVE_OPENSSL
  } else {
    context = NewContext(isolate_);
    CHECK(!context.IsEmpty());
    Context::Scope context_scope(context);
    env.reset(
        CreateEnvironment(isolate_data_.get(), context, args_, exec_args_));
  }

  return env;
}

}  // namespace node
