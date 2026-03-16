// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "snapshot.h"

#include <workerd/jsg/setup.h>

// v8.h includes all V8 headers (context, isolate, module, script, snapshot, etc.)
#include <v8.h>

#include <kj/debug.h>
#include <kj/map.h>
#include <kj/string.h>
#include <kj/time.h>

#include <cstring>

namespace workerd::server {

namespace {

// An arbitrary tag value used for our embedder data slot. Must be non-zero so V8 treats
// it as a tagged pointer (not a raw pointer) in the embedder data array.
constexpr v8::EmbedderDataTypeTag kResolveStateTag = static_cast<v8::EmbedderDataTypeTag>(0x1);
constexpr int kResolveStateSlot = 1;

// Compiles a single ES module source into a v8::Module without evaluating it.
// Returns an empty MaybeLocal on failure (exception will be pending on the isolate).
v8::MaybeLocal<v8::Module> compileModule(
    v8::Isolate* isolate, kj::StringPtr specifier, kj::ArrayPtr<const char> source) {
  v8::EscapableHandleScope scope(isolate);

  auto nameStr = v8::String::NewFromUtf8(
      isolate, specifier.cStr(), v8::NewStringType::kNormal, static_cast<int>(specifier.size()))
                     .ToLocalChecked();
  auto sourceStr = v8::String::NewFromUtf8(
      isolate, source.begin(), v8::NewStringType::kNormal, static_cast<int>(source.size()))
                       .ToLocalChecked();

  v8::ScriptOrigin origin(nameStr,
      /*resource_line_offset=*/0,
      /*resource_column_offset=*/0,
      /*resource_is_shared_cross_origin=*/false,
      /*script_id=*/-1,
      /*source_map_url=*/v8::Local<v8::Value>(),
      /*resource_is_opaque=*/false,
      /*is_wasm=*/false,
      /*is_module=*/true);

  v8::ScriptCompiler::Source src(sourceStr, origin);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(isolate, &src).ToLocal(&module)) {
    return v8::MaybeLocal<v8::Module>();
  }
  return scope.Escape(module);
}

// V8 callback for resolving module imports during Module::InstantiateModule.
v8::MaybeLocal<v8::Module> resolveModuleCallback(v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> /*import_attributes*/,
    v8::Local<v8::Module> /*referrer*/) {
  // V8 enters the isolate before calling this callback, so GetCurrent() is safe.
  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  auto* compiledModules = static_cast<kj::HashMap<kj::String, v8::Global<v8::Module>>*>(
      context->GetAlignedPointerFromEmbedderData(isolate, kResolveStateSlot, kResolveStateTag));
  KJ_ASSERT(compiledModules != nullptr, "compiledModules is null in resolveModuleCallback");

  v8::String::Utf8Value specifierStr(isolate, specifier);
  kj::StringPtr specifierKj(*specifierStr);

  return KJ_UNWRAP_OR(compiledModules->find(specifierKj), {
    auto msg = kj::str("Module not found: ", specifierKj);
    isolate->ThrowError(v8::String::NewFromUtf8(isolate, msg.cStr()).ToLocalChecked());
    return v8::MaybeLocal<v8::Module>();
  }).Get(isolate);
}

// Serializes the V8 heap captured in `creator` into a kj::Array.
// The SnapshotCreator must have had SetDefaultContext() called before this.
kj::Array<kj::byte> serializeSnapshot(v8::SnapshotCreator& creator) {
  // Serialize the heap. kKeep retains compiled bytecode in the blob.
  v8::StartupData blob = creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);

  KJ_REQUIRE(
      blob.data != nullptr && blob.raw_size > 0, "V8 SnapshotCreator returned an empty blob");

  // Copy the blob into a kj::Array. V8 owns blob.data and we must free it ourselves
  // after copying (V8 allocates with new[]).
  auto result = kj::heapArray<kj::byte>(blob.raw_size);
  memcpy(result.begin(), blob.data, blob.raw_size);
  delete[] blob.data;

  return result;
}

}  // namespace

kj::Array<kj::byte> createWorkerSnapshot(
    jsg::V8System& /*v8System*/, const WorkerSource::ModulesSource& source) {
  // Use the non-deprecated SnapshotCreator constructor that takes CreateParams.
  // We pass default CreateParams (no external references) because the snapshot context
  // contains no C++ callbacks — only plain JavaScript objects and V8 built-ins.
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared = std::shared_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());

  v8::SnapshotCreator creator(params);
  v8::Isolate* isolate = creator.GetIsolate();

  auto& clock = kj::systemPreciseMonotonicClock();
  auto tStart = clock.now();
  kj::TimePoint tCompiled = tStart;
  kj::TimePoint tEvaluated = tStart;

  {
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);

    // Create a plain context with only standard V8 built-ins (no JSG TypeWrapper).
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope contextScope(context);

    // Compile all ES modules and register them in the resolve map, then store a
    // pointer to the map in the context's embedder data so the resolve callback can reach it.
    kj::HashMap<kj::String, v8::Global<v8::Module>> compiledModules;
    context->SetAlignedPointerInEmbedderData(kResolveStateSlot, &compiledModules, kResolveStateTag);

    // Phase 1: compile all ES modules and register them in the resolve map.
    for (auto& mod: source.modules) {
      KJ_IF_SOME(esModule, mod.content.tryGet<WorkerSource::EsModule>()) {
        kj::StringPtr specifier = mod.name;
        kj::ArrayPtr<const char> body = esModule.body;

        v8::Local<v8::Module> module;
        if (!compileModule(isolate, specifier, body).ToLocal(&module)) {
          KJ_FAIL_REQUIRE("Failed to compile module during snapshot creation", specifier);
        }
        compiledModules.insert(kj::str(specifier), v8::Global<v8::Module>(isolate, module));
      }
    }

    tCompiled = clock.now();

    // Phase 2: instantiate and evaluate the main module (and its transitive imports).
    kj::StringPtr mainModuleName = source.mainModule;
    v8::Local<v8::Module> mainModule = KJ_UNWRAP_OR(compiledModules.find(mainModuleName), {
      KJ_FAIL_REQUIRE("Main module not found in modules list", mainModuleName);
    }).Get(isolate);

    // Instantiate: resolves imports recursively via resolveModuleCallback.
    if (mainModule->InstantiateModule(context, resolveModuleCallback).IsNothing()) {
      KJ_FAIL_REQUIRE("Failed to instantiate main module during snapshot creation", mainModuleName);
    }

    // Evaluate: run the top-level code.
    v8::Local<v8::Value> evalResult;
    if (!mainModule->Evaluate(context).ToLocal(&evalResult)) {
      KJ_FAIL_REQUIRE("Failed to evaluate main module during snapshot creation", mainModuleName);
    }

    // Drain microtasks so any top-level awaits complete before we snapshot.
    isolate->PerformMicrotaskCheckpoint();

    tEvaluated = clock.now();

    // Clear the raw pointer from embedder data so it does not end up in the snapshot.
    context->SetAlignedPointerInEmbedderData(kResolveStateSlot, nullptr, kResolveStateTag);

    // Register this context as the default context (index 0) for the snapshot.
    creator.SetDefaultContext(context);
  }

  auto tSerializeStart = clock.now();
  auto blob = serializeSnapshot(creator);
  auto tEnd = clock.now();

  KJ_LOG(WARNING, "snapshot creation timing (ms): esm", "compile_ms",
      (tCompiled - tStart) / kj::MILLISECONDS, "evaluate_ms",
      (tEvaluated - tCompiled) / kj::MILLISECONDS, "serialize_ms",
      (tEnd - tSerializeStart) / kj::MILLISECONDS, "blob_bytes", blob.size());

  return blob;
}

kj::Array<kj::byte> createWorkerSnapshot(
    jsg::V8System& /*v8System*/, const WorkerSource::ScriptSource& source) {
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared = std::shared_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());

  v8::SnapshotCreator creator(params);
  v8::Isolate* isolate = creator.GetIsolate();

  auto& clock = kj::systemPreciseMonotonicClock();
  auto tStart = clock.now();
  kj::TimePoint tCompiled = tStart;
  kj::TimePoint tEvaluated = tStart;

  {
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);

    // Context 0 (default): clean built-in context. Required by SnapshotCreator; used as
    // the template for contexts created at runtime via Context::New().
    {
      v8::Local<v8::Context> emptyCtx = v8::Context::New(isolate);
      creator.SetDefaultContext(emptyCtx);
    }

    // Context 1 (added, index 0 for Context::FromSnapshot): the script execution context.
    // It contains the mock addEventListener and the handler functions captured from the
    // top-level script evaluation.
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope contextScope(context);

      // Install mock addEventListener that records calls into __snapshot_listeners__.
      // Using eval-style injection so we stay in pure JS and avoid needing V8 function
      // templates (which would require external_references for serialization).
      static constexpr kj::StringPtr kSetupScript = R"js(
        globalThis.__snapshot_listeners__ = [];
        globalThis.addEventListener = function(type, handler) {
          globalThis.__snapshot_listeners__.push({ type: type, handler: handler });
        };
        )js"_kj;

      auto setupStr = v8::String::NewFromUtf8(isolate, kSetupScript.cStr(),
          v8::NewStringType::kNormal, static_cast<int>(kSetupScript.size()))
                          .ToLocalChecked();
      v8::Script::Compile(context, setupStr).ToLocalChecked()->Run(context).ToLocalChecked();

      // Compile and run the worker's top-level script.
      auto mainSrc = v8::String::NewFromUtf8(isolate, source.mainScript.cStr(),
          v8::NewStringType::kNormal, static_cast<int>(source.mainScript.size()))
                         .ToLocalChecked();
      auto scriptNameStr = v8::String::NewFromUtf8(isolate, source.mainScriptName.cStr(),
          v8::NewStringType::kNormal, static_cast<int>(source.mainScriptName.size()))
                               .ToLocalChecked();
      v8::ScriptOrigin origin(scriptNameStr);
      v8::Local<v8::Script> script;
      if (!v8::Script::Compile(context, mainSrc, &origin).ToLocal(&script)) {
        KJ_FAIL_REQUIRE(
            "Failed to compile worker script during snapshot creation", source.mainScriptName);
      }
      tCompiled = clock.now();

      if (script->Run(context).IsEmpty()) {
        KJ_FAIL_REQUIRE(
            "Failed to execute worker script during snapshot creation", source.mainScriptName);
      }

      // Flush microtasks so any top-level awaits settle before snapshotting.
      isolate->PerformMicrotaskCheckpoint();
      tEvaluated = clock.now();

      // Register as context index 0 for Context::FromSnapshot at runtime.
      creator.AddContext(context);
    }
  }

  auto tSerializeStart = clock.now();
  auto blob = serializeSnapshot(creator);
  auto tEnd = clock.now();

  KJ_LOG(WARNING, "snapshot creation timing (ms): script", "compile_ms",
      (tCompiled - tStart) / kj::MILLISECONDS, "evaluate_ms",
      (tEvaluated - tCompiled) / kj::MILLISECONDS, "serialize_ms",
      (tEnd - tSerializeStart) / kj::MILLISECONDS, "blob_bytes", blob.size());

  return blob;
}

}  // namespace workerd::server
