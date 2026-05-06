// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// PoC: validate that an in-process V8 snapshot save→load round-trip works at the JSG layer.
// One save isolate produces a blob; a fresh load isolate consumes it and runs JS that depends
// on state populated in the save isolate's default context.

#include "jsg-test.h"

#include <v8-snapshot.h>

#include <kj/test.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct SnapCtx: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(SnapCtx) {}
};
JSG_DECLARE_ISOLATE_TYPE(SnapIsolate, SnapCtx);

KJ_TEST("save and load V8 snapshot round-trip in same process") {
  kj::Array<kj::byte> blobBytes;
  kj::Array<intptr_t> refs;

  // === SAVE phase ===
  {
    SnapIsolate saveIsolate(v8System, v8::IsolateGroup::GetDefault(), nullptr,
        kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
        /*instantiateTypeWrapper=*/true, IsolateMode::SAVE_SNAPSHOT);

    saveIsolate.runInLockScope([&](SnapIsolate::Lock& lock) {
      jsg::Lock& js = lock;
      js.withinHandleScope([&] {
        auto jsContext = lock.newContext<SnapCtx>();
        auto context = jsContext.getHandle(lock.v8Isolate);
        v8::Context::Scope cs(context);

        // Populate user JS state in the to-be-saved default context. CreateBlob with
        // FunctionCodeHandling::kClear discards compiled bytecode but keeps source,
        // so the function will recompile lazily on first call in the load isolate.
        {
          auto src = jsg::v8Str(lock.v8Isolate,
              "globalThis.add = (a, b) => a + b;"
              "globalThis.greeting = 'hello';"_kj);
          auto script = check(v8::Script::Compile(context, src));
          check(script->Run(context));
        }

        // Save pipeline at JSG layer: only isolate-level handles (visited via
        // IsolateBase::visitPersistentHandles), no Worker handles.
        auto& base = static_cast<IsolateBase&>(saveIsolate);
        kj::Vector<v8::Global<v8::FunctionTemplate>*> ftHandles;
        kj::Vector<v8::Global<v8::Object>*> objHandles;
        kj::Vector<v8::Global<v8::Name>*> nameHandles;
        kj::Vector<v8::Global<v8::DictionaryTemplate>*> dictHandles;
        base.visitPersistentHandles([&](v8::Global<v8::FunctionTemplate>& h) { ftHandles.add(&h); },
            [&](v8::Global<v8::Object>& h) { objHandles.add(&h); }, [&](v8::Global<v8::Name>& h) {
          nameHandles.add(&h);
        }, [&](v8::Global<v8::DictionaryTemplate>& h) { dictHandles.add(&h); });

        auto* creator = base.getSnapshotCreator();
        for (auto* h: ftHandles) creator->AddData(context, h->Get(lock.v8Isolate));
        for (auto* h: objHandles) creator->AddData(context, h->Get(lock.v8Isolate));
        for (auto* h: nameHandles) creator->AddData(context, h->Get(lock.v8Isolate));
        for (auto* h: dictHandles) creator->AddData(context, h->Get(lock.v8Isolate));

        // Reset wrappable handles for the SnapCtx global object: clears strongWrapper,
        // TracedReference, cppgcShim and unlinks from the HeapTracer wrappers list so
        // V8's CheckGlobalAndEternalHandles passes and the JsContext destructor at end
        // of lock scope doesn't trip kj::List's "destroyed while in list" abort.
        jsContext->resetHandlesForSnapshot();

        // Reset the v8::Global<v8::Context> in JsContext (the context itself is
        // serialized via SetDefaultContext, not AddData).
        jsContext.visitHandle([](auto& h) { h.Reset(); });

        // Reset C++ Globals so V8 doesn't see them as roots during CreateBlob.
        for (auto* h: ftHandles) h->Reset();
        for (auto* h: objHandles) h->Reset();
        for (auto* h: nameHandles) h->Reset();
        for (auto* h: dictHandles) h->Reset();

        base.getExternalReferences().add(0);  // null-terminate
        creator->SetDefaultContext(context);
        v8::StartupData blob =
            creator->CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);

        blobBytes = kj::heapArray<kj::byte>(
            reinterpret_cast<const kj::byte*>(blob.data), static_cast<size_t>(blob.raw_size));
        delete[] blob.data;
        refs = kj::heapArray<intptr_t>(base.getExternalReferences().asPtr());
      });
    });
  }  // saveIsolate destroyed

  // === LOAD phase ===
  SnapshotArtifact artifact{
    .blob = kj::mv(blobBytes),
    .externalReferences = kj::mv(refs),
  };
  SnapIsolate loadIsolate(v8System, v8::IsolateGroup::GetDefault(), nullptr,
      kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
      /*instantiateTypeWrapper=*/true, IsolateMode::LOAD_SNAPSHOT, artifact);

  loadIsolate.runInLockScope([&](SnapIsolate::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&] {
      auto context = lock.newContext<SnapCtx>().getHandle(lock.v8Isolate);
      v8::Context::Scope cs(context);

      // Sanity: V8 functional after load.
      {
        auto src = jsg::v8Str(lock.v8Isolate, "1 + 1"_kj);
        auto script = check(v8::Script::Compile(context, src));
        auto result = check(script->Run(context));
        KJ_EXPECT(check(result->Int32Value(context)) == 2);
      }

      // Variable from save isolate is visible in load isolate's default context.
      {
        auto src = jsg::v8Str(lock.v8Isolate, "globalThis.greeting"_kj);
        auto script = check(v8::Script::Compile(context, src));
        auto result = check(script->Run(context));
        v8::String::Utf8Value v(lock.v8Isolate, result);
        KJ_EXPECT(kj::StringPtr(*v) == "hello");
      }

      // Function from save isolate is visible and callable (recompiled on first call).
      {
        auto src = jsg::v8Str(lock.v8Isolate, "globalThis.add(40, 2)"_kj);
        auto script = check(v8::Script::Compile(context, src));
        auto result = check(script->Run(context));
        KJ_EXPECT(check(result->Int32Value(context)) == 42);
      }
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
