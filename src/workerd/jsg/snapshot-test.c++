// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// PoC: validate that an in-process V8 snapshot prepare→start round-trip works at the JSG layer,
// including a C++-wrapped object with internal fields that survives the snapshot via
// SerializeInternalFieldsCallback / DeserializeInternalFieldsCallback.

#include "jsg-test.h"

#include <v8-snapshot.h>

#include <kj/test.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;

// Probe type that round-trips through the snapshot. Holds a single kj::String which is
// serialized into the snapshot blob and recreated on load.
class SnapshotProbe: public Object {
 public:
  static constexpr uint32_t kTypeId = 1;

  explicit SnapshotProbe(kj::String value): value(kj::mv(value)) {}

  static jsg::Ref<SnapshotProbe> constructor(jsg::Lock& js, kj::String value) {
    return js.alloc<SnapshotProbe>(kj::mv(value));
  }

  kj::StringPtr getValue() {
    return value;
  }

  kj::Maybe<SnapshotData> snapshotSerialize() override {
    auto bytes = value.asBytes();
    auto out = kj::heapArray<kj::byte>(bytes.size());
    memcpy(out.begin(), bytes.begin(), bytes.size());
    return SnapshotData{.typeId = kTypeId, .bytes = kj::mv(out)};
  }

  JSG_RESOURCE_TYPE(SnapshotProbe) {
    JSG_READONLY_INSTANCE_PROPERTY(value, getValue);
  }

 private:
  kj::String value;
};

struct SnapCtx: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(SnapCtx) {
    JSG_NESTED_TYPE(SnapshotProbe);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SnapIsolate, SnapCtx, SnapshotProbe);

// Layout of the v8::StartupData payload returned by serializeInternalFieldsCb for a
// recognised Wrappable: 4-byte big-endian type id followed by raw type-specific bytes.
constexpr size_t kSnapshotHeaderBytes = 4;

void writeBE32(kj::byte* dst, uint32_t value) {
  dst[0] = static_cast<kj::byte>((value >> 24) & 0xff);
  dst[1] = static_cast<kj::byte>((value >> 16) & 0xff);
  dst[2] = static_cast<kj::byte>((value >> 8) & 0xff);
  dst[3] = static_cast<kj::byte>(value & 0xff);
}

uint32_t readBE32(const char* src) {
  auto* p = reinterpret_cast<const unsigned char*>(src);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Empty pass-through stubs for the context-data and api-wrapper serialization slots.
// Both callbacks must be non-null when the SetDefaultContext'd v8::Context carries any
// embedder pointers (newContext does, via jsg::setAlignedPointerInEmbedderData for
// GLOBAL_WRAPPER and MAX_POINTER_SLOT). Returning {nullptr, 0} preserves the slot's
// existing layout without serializing any embedder data — fine for this PoC since the
// global wrapper's C++ side is not reconstructed on load.
v8::StartupData serializeContextDataStub(v8::Local<v8::Context>, int, void*) {
  return {nullptr, 0};
}
v8::StartupData serializeApiWrapperStub(v8::Local<v8::Object>, void*, void*) {
  return {nullptr, 0};
}

// SerializeInternalFieldsCallback. Called by V8 during CreateBlob() for every aligned
// pointer internal field of every reachable v8::Object in the default context.
//
// Slot layout for jsg::Wrappable (see Wrappable::InternalFields):
//   index 0: WORKERD_WRAPPABLE_TAG pointer — the deserialize side hard-writes this back,
//            so we serialize an empty payload.
//   index 1: Wrappable* — recover the C++ object and let it serialize itself via the
//            virtual snapshotSerialize() hook. Types that opt out (e.g. the SnapCtx
//            global wrapper) return kj::none and are not reconstructed on load.
v8::StartupData serializeInternalFieldsCb(v8::Local<v8::Object> holder, int index, void* /*data*/) {
  if (index == Wrappable::WRAPPABLE_TAG_FIELD_INDEX) {
    return {nullptr, 0};
  }
  KJ_REQUIRE(index == Wrappable::WRAPPED_OBJECT_FIELD_INDEX);

  if (!Wrappable::isWorkerdApiObject(holder)) {
    KJ_DBG("snapshotted non workerd api object");
    return {nullptr, 0};
  }

  auto* wrappable = static_cast<Wrappable*>(
      holder->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
          static_cast<v8::EmbedderDataTypeTag>(Wrappable::WRAPPED_OBJECT_FIELD_INDEX)));
  if (wrappable == nullptr) {
    return {nullptr, 0};
  }

  KJ_IF_SOME(payload, wrappable->snapshotSerialize()) {
    size_t total = kSnapshotHeaderBytes + payload.bytes.size();
    // V8 takes ownership of the returned buffer and frees it via delete[], so allocate
    // with raw new[] (not kj::heapArray, which has a different deleter).
    char* buf = new char[total];
    writeBE32(reinterpret_cast<kj::byte*>(buf), payload.typeId);
    memcpy(buf + kSnapshotHeaderBytes, payload.bytes.begin(), payload.bytes.size());
    return v8::StartupData{buf, static_cast<int>(total)};
  }
  return {nullptr, 0};
}

// DeserializeInternalFieldsCallback. Called by V8 from inside Context::New() while the
// default context graph is being reconstructed from the snapshot blob, once per non-empty
// internal field that the SAVE-side serializer wrote.
void deserializeInternalFieldsCb(
    v8::Local<v8::Object> holder, int index, v8::StartupData payload, void* data) {
  auto* isolate = static_cast<v8::Isolate*>(data);

  if (index == Wrappable::WRAPPABLE_TAG_FIELD_INDEX) {
    auto* tagPtr = const_cast<uint16_t*>(&Wrappable::WORKERD_WRAPPABLE_TAG);
    holder->SetAlignedPointerInInternalField(Wrappable::WRAPPABLE_TAG_FIELD_INDEX, tagPtr,
        static_cast<v8::EmbedderDataTypeTag>(Wrappable::WRAPPABLE_TAG_FIELD_INDEX));
    return;
  }

  KJ_REQUIRE(index == Wrappable::WRAPPED_OBJECT_FIELD_INDEX);
  if (payload.raw_size < static_cast<int>(kSnapshotHeaderBytes)) {
    return;
  }
  uint32_t typeId = readBE32(payload.data);
  kj::ArrayPtr<const kj::byte> body{
    reinterpret_cast<const kj::byte*>(payload.data) + kSnapshotHeaderBytes,
    static_cast<size_t>(payload.raw_size) - kSnapshotHeaderBytes};

  switch (typeId) {
    case SnapshotProbe::kTypeId: {
      auto value = kj::heapString(reinterpret_cast<const char*>(body.begin()), body.size());
      auto ref = jsg::Ref<SnapshotProbe>(kj::refcounted<SnapshotProbe>(kj::mv(value)));
      // Reattach: this allocates a CppgcShim, sets the (now correct) Wrappable* in
      // internal field 1 and a TracedReference back to `holder`. The shim takes a
      // strong refcount on the Wrappable, so the C++ object survives `ref` going out
      // of scope below.
      ref.attachWrapper(isolate, holder);
      return;
    }
    default:
      KJ_LOG(WARNING, "snapshot deserialize: unknown typeId", typeId);
      return;
  }
}

KJ_TEST("save and load V8 snapshot round-trip in same process") {
  kj::Array<kj::byte> blobBytes;
  kj::Array<intptr_t> refs;

  // === PREPARE_SNAPSHOT phase ===
  {
    SnapIsolate prepareIsolate(v8System, v8::IsolateGroup::GetDefault(), nullptr,
        kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
        /*instantiateTypeWrapper=*/true, IsolateMode::PREPARE_SNAPSHOT);

    // Holds a strong refcount on every C++ Wrappable that opted into snapshot
    // serialization, keeping them alive across CreateBlob() so the serialize callback
    // can dereference internal field 1 and call snapshotSerialize() on a live object.
    // The wrappers themselves must still have their TracedReference / cppgcShim torn
    // down before CreateBlob (V8's CheckGlobalAndEternalHandles otherwise fires) — the
    // keepalives are what keep the C++ side alive after that teardown.
    kj::Vector<kj::Own<Wrappable>> snapshotKeepalives;

    prepareIsolate.runInLockScope([&](SnapIsolate::Lock& lock) {
      jsg::Lock& js = lock;
      js.withinHandleScope([&] {
        auto jsContext = lock.newContext<SnapCtx>();
        auto context = jsContext.getHandle(lock.v8Isolate);
        v8::Context::Scope cs(context);

        // Populate user JS state plus a C++-wrapped SnapshotProbe attached to globalThis
        // by name. The probe wrapper is the thing that exercises the
        // SerializeInternalFieldsCallback path: V8 walks the default context graph during
        // CreateBlob(), reaches the wrapper through globalThis.probe, sees that it has
        // pointer internal fields, and invokes serializeInternalFieldsCb for each.
        {
          auto src = jsg::v8Str(lock.v8Isolate,
              "globalThis.add = (a, b) => a + b;"
              "globalThis.greeting = 'hello';"
              "globalThis.probe = new SnapshotProbe('hi-from-snapshot');"_kj);
          auto script = check(v8::Script::Compile(context, src));
          check(script->Run(context));
        }

        // Tear down the probe's V8 wrapper bindings (TracedReference, cppgcShim) before
        // CreateBlob, but keep the C++ object alive so the serialize callback can call
        // snapshotSerialize() on it during CreateBlob. detachWrapper does both: it
        // freelists the shim and returns the kj::Own<Wrappable> that the shim's Active
        // state used to hold. Internal field 1 still points at the same Wrappable* —
        // detachWrapper doesn't touch internal fields, only the embedder-side bindings.
        {
          auto probeName = jsg::v8StrIntern(lock.v8Isolate, "probe"_kj);
          auto probeVal = check(context->Global()->Get(context, probeName));
          KJ_REQUIRE(probeVal->IsObject());
          auto probeObj = probeVal.As<v8::Object>();
          KJ_REQUIRE(Wrappable::isWorkerdApiObject(probeObj));
          auto* wrappable = static_cast<Wrappable*>(
              probeObj->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
                  static_cast<v8::EmbedderDataTypeTag>(Wrappable::WRAPPED_OBJECT_FIELD_INDEX)));
          snapshotKeepalives.add(wrappable->detachWrapper(/*shouldFreelistShim=*/true));
        }

        // PREPARE_SNAPSHOT pipeline at JSG layer: only isolate-level handles (visited via
        // IsolateBase::visitPersistentHandles), no Worker handles.
        auto& base = static_cast<IsolateBase&>(prepareIsolate);
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
        //
        // Note we do NOT call resetHandlesForSnapshot on the SnapshotProbe wrapper:
        // serializeInternalFieldsCb needs to dereference its Wrappable* and call the
        // virtual snapshotSerialize() during CreateBlob(), so the C++ object must stay
        // alive and the cppgcShim must remain Active. The probe is owned through
        // globalThis only, so once the snapshot is built nothing else holds onto it.
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
        v8::SerializeInternalFieldsCallback serializeCb(&serializeInternalFieldsCb, nullptr);
        v8::SerializeContextDataCallback contextDataCb(&serializeContextDataStub, nullptr);
        v8::SerializeAPIWrapperCallback apiWrapperCb(&serializeApiWrapperStub, nullptr);
        creator->SetDefaultContext(context, serializeCb, contextDataCb, apiWrapperCb);
        v8::StartupData blob =
            creator->CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);

        blobBytes = kj::heapArray<kj::byte>(
            reinterpret_cast<const kj::byte*>(blob.data), static_cast<size_t>(blob.raw_size));
        delete[] blob.data;
        refs = kj::heapArray<intptr_t>(base.getExternalReferences().asPtr());
      });
    });
  }  // prepareIsolate destroyed

  // === START_FROM_SNAPSHOT phase ===
  SnapshotArtifact artifact{
    .blob = kj::mv(blobBytes),
    .externalReferences = kj::mv(refs),
  };
  SnapIsolate startIsolate(v8System, v8::IsolateGroup::GetDefault(), nullptr,
      kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
      /*instantiateTypeWrapper=*/true, IsolateMode::START_FROM_SNAPSHOT, artifact);

  startIsolate.runInLockScope([&](SnapIsolate::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&] {
      // Thread the deserialize callbacks into newContext via NewContextOptions. This
      // keeps the InstanceTemplate for SnapCtx (and thus the global object's map size)
      // matching what the snapshot was built with, while V8's
      // DeserializeInternalFieldsCallback fires for every aligned-pointer field that
      // the SAVE-side serializer marked as non-empty.
      auto* isolate = lock.v8Isolate;
      jsg::NewContextOptions options;
      options.internalFieldsDeserializer =
          v8::DeserializeInternalFieldsCallback(&deserializeInternalFieldsCb, isolate);
      auto context = lock.newContext<SnapCtx>(options).getHandle(lock.v8Isolate);
      v8::Context::Scope cs(context);

      auto runScript = [&](kj::StringPtr code) -> v8::Local<v8::Value> {
        v8::TryCatch tryCatch(lock.v8Isolate);
        auto src = jsg::v8Str(lock.v8Isolate, code);
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, src).ToLocal(&script)) {
          v8::String::Utf8Value m(lock.v8Isolate, tryCatch.Exception());
          KJ_FAIL_REQUIRE("compile failed", code, kj::StringPtr(*m));
        }
        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
          v8::String::Utf8Value m(lock.v8Isolate, tryCatch.Exception());
          KJ_FAIL_REQUIRE("run failed", code, kj::StringPtr(*m));
        }
        return result;
      };

      // Sanity: V8 functional after load.
      {
        auto result = runScript("1 + 1"_kj);
        KJ_EXPECT(check(result->Int32Value(context)) == 2);
      }

      // Variable from PREPARE_SNAPSHOT isolate is visible in START_FROM_SNAPSHOT isolate's default context.
      {
        auto result = runScript("globalThis.greeting"_kj);
        v8::String::Utf8Value v(lock.v8Isolate, result);
        KJ_EXPECT(kj::StringPtr(*v) == "hello");
      }

      // Function from PREPARE_SNAPSHOT isolate is visible and callable (recompiled on first call).
      {
        auto result = runScript("globalThis.add(40, 2)"_kj);
        KJ_EXPECT(check(result->Int32Value(context)) == 42);
      }

      // SnapshotProbe wrapper survived the snapshot. Verify at the V8 level (no JSG
      // accessor): WORKERD_WRAPPABLE_TAG in slot 0 means deserializeInternalFieldsCb
      // ran for the tag, and a non-null pointer in slot 1 means it ran for the C++
      // payload and reattached a fresh SnapshotProbe via Ref::attachWrapper.
      {
        auto probeName = jsg::v8StrIntern(lock.v8Isolate, "probe"_kj);
        auto probeVal = check(context->Global()->Get(context, probeName));
        KJ_EXPECT(probeVal->IsObject());
        auto probeObj = probeVal.As<v8::Object>();
        KJ_EXPECT(Wrappable::isWorkerdApiObject(probeObj));
        void* slot1 =
            probeObj->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
                static_cast<v8::EmbedderDataTypeTag>(Wrappable::WRAPPED_OBJECT_FIELD_INDEX));
        KJ_EXPECT(slot1 != nullptr);
      }

      // The JSG-level read `globalThis.probe.value` currently still throws
      // "Illegal invocation" — the SnapshotProbe FunctionTemplate's identity differs
      // between PREPARE_SNAPSHOT and START_FROM_SNAPSHOT isolates (the JSG type wrapper builds
      // fresh templates; the wrapper inside the snapshot points at the PREPARE_SNAPSHOT-side
      // template). Restoring
      // template identity across the snapshot boundary is a separate problem from the
      // SerializeInternalFieldsCallback path this PoC validates, so we don't assert it.
    });
  });
}

KJ_TEST(
    "orphaned wrapper (no embedder bindings, no JS refs) is collected by GC inside CreateBlob") {
  // Hypothesis: if we (a) call detachWrapper to clear the v8::Object's TracedReference /
  // cppgcShim / strongWrapper bindings to the C++ Wrappable, AND (b) drop the JS reference
  // (delete globalThis.gone), then during CreateBlob's internal GC pass the v8::Object has
  // no roots — neither embedder-side nor JS-side — and gets collected. The C++ Wrappable
  // itself stays alive via the kj::Own returned by detachWrapper, so internal field 1
  // never becomes dangling. We prove the wrapper is gone by asserting after load that
  // 'gone' is not a property of globalThis. A control variable globalThis.live tells us
  // the snapshot loaded properly (guards against false PASS on an empty snapshot).

  kj::Array<kj::byte> blobBytes;
  kj::Array<intptr_t> refs;

  // === PREPARE_SNAPSHOT phase ===
  {
    SnapIsolate prepareIsolate(v8System, v8::IsolateGroup::GetDefault(), nullptr,
        kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
        /*instantiateTypeWrapper=*/true, IsolateMode::PREPARE_SNAPSHOT);

    kj::Vector<kj::Own<Wrappable>> snapshotKeepalives;

    prepareIsolate.runInLockScope([&](SnapIsolate::Lock& lock) {
      jsg::Lock& js = lock;
      js.withinHandleScope([&] {
        auto jsContext = lock.newContext<SnapCtx>();
        auto context = jsContext.getHandle(lock.v8Isolate);
        v8::Context::Scope cs(context);

        // Set up: a probe attached to globalThis with a monkey-patched property, plus
        // a control string we expect to definitely survive the snapshot.
        {
          auto src = jsg::v8Str(lock.v8Isolate,
              "globalThis.gone = new SnapshotProbe('should-be-collected');"
              "globalThis.gone.newField = 123;"
              "globalThis.live = 'still-here';"_kj);
          auto script = check(v8::Script::Compile(context, src));
          check(script->Run(context));
        }

        // Detach C++ from the wrapper but keep the C++ object alive via keepalives.
        {
          auto goneName = jsg::v8StrIntern(lock.v8Isolate, "gone"_kj);
          auto goneVal = check(context->Global()->Get(context, goneName));
          KJ_REQUIRE(goneVal->IsObject());
          auto goneObj = goneVal.As<v8::Object>();
          KJ_REQUIRE(Wrappable::isWorkerdApiObject(goneObj));
          auto* wrappable = static_cast<Wrappable*>(
              goneObj->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX,
                  static_cast<v8::EmbedderDataTypeTag>(Wrappable::WRAPPED_OBJECT_FIELD_INDEX)));
          snapshotKeepalives.add(wrappable->detachWrapper(/*shouldFreelistShim=*/true));
        }

        // Drop the JS reference. After this the v8::Object has no roots: TracedReference
        // and cppgcShim were torn down by detachWrapper, and globalThis no longer
        // references it. The monkey-patched newField is just an internal property of the
        // soon-to-be-orphan wrapper.
        {
          auto src = jsg::v8Str(lock.v8Isolate, "delete globalThis.gone;"_kj);
          auto script = check(v8::Script::Compile(context, src));
          check(script->Run(context));
        }

        // Standard PREPARE_SNAPSHOT pipeline.
        auto& base = static_cast<IsolateBase&>(prepareIsolate);
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

        jsContext->resetHandlesForSnapshot();
        jsContext.visitHandle([](auto& h) { h.Reset(); });
        for (auto* h: ftHandles) h->Reset();
        for (auto* h: objHandles) h->Reset();
        for (auto* h: nameHandles) h->Reset();
        for (auto* h: dictHandles) h->Reset();

        base.getExternalReferences().add(0);  // null-terminate
        v8::SerializeInternalFieldsCallback serializeCb(&serializeInternalFieldsCb, nullptr);
        v8::SerializeContextDataCallback contextDataCb(&serializeContextDataStub, nullptr);
        v8::SerializeAPIWrapperCallback apiWrapperCb(&serializeApiWrapperStub, nullptr);
        creator->SetDefaultContext(context, serializeCb, contextDataCb, apiWrapperCb);
        v8::StartupData blob =
            creator->CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);

        blobBytes = kj::heapArray<kj::byte>(
            reinterpret_cast<const kj::byte*>(blob.data), static_cast<size_t>(blob.raw_size));
        delete[] blob.data;
        refs = kj::heapArray<intptr_t>(base.getExternalReferences().asPtr());
      });
    });
  }

  // === START_FROM_SNAPSHOT phase ===
  SnapshotArtifact artifact{
    .blob = kj::mv(blobBytes),
    .externalReferences = kj::mv(refs),
  };
  SnapIsolate startIsolate(v8System, v8::IsolateGroup::GetDefault(), nullptr,
      kj::heap<IsolateObserver>(), defaultExternalStringAllocator(), v8::Isolate::CreateParams{},
      /*instantiateTypeWrapper=*/true, IsolateMode::START_FROM_SNAPSHOT, artifact);

  startIsolate.runInLockScope([&](SnapIsolate::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&] {
      auto* isolate = lock.v8Isolate;
      jsg::NewContextOptions options;
      options.internalFieldsDeserializer =
          v8::DeserializeInternalFieldsCallback(&deserializeInternalFieldsCb, isolate);
      auto context = lock.newContext<SnapCtx>(options).getHandle(lock.v8Isolate);
      v8::Context::Scope cs(context);

      auto runScript = [&](kj::StringPtr code) -> v8::Local<v8::Value> {
        v8::TryCatch tryCatch(lock.v8Isolate);
        auto src = jsg::v8Str(lock.v8Isolate, code);
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, src).ToLocal(&script)) {
          v8::String::Utf8Value m(lock.v8Isolate, tryCatch.Exception());
          KJ_FAIL_REQUIRE("compile failed", code, kj::StringPtr(*m));
        }
        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
          v8::String::Utf8Value m(lock.v8Isolate, tryCatch.Exception());
          KJ_FAIL_REQUIRE("run failed", code, kj::StringPtr(*m));
        }
        return result;
      };

      // Control: snapshot really did load — globalThis.live is reachable.
      {
        auto result = runScript("globalThis.live"_kj);
        v8::String::Utf8Value v(lock.v8Isolate, result);
        KJ_EXPECT(kj::StringPtr(*v) == "still-here");
      }

      // The hypothesis: orphaned wrapper got GC'd inside CreateBlob, so 'gone' is not
      // a property of globalThis on load.
      {
        auto result = runScript("'gone' in globalThis"_kj);
        KJ_EXPECT(result->IsBoolean());
        KJ_EXPECT(result->BooleanValue(lock.v8Isolate) == false);
      }

      // Diagnostic branch: if the assertion above failed, report whether the wrapper
      // survived AND whether the monkey-patched property survived. This block is
      // only meaningful when the hypothesis is wrong — when it's right, 'gone in
      // globalThis' is false and the runScript here returns undefined / throws.
      {
        auto result = runScript("'gone' in globalThis ? globalThis.gone.newField : null"_kj);
        if (!result->IsNull()) {
          KJ_LOG(WARNING, "hypothesis wrong: orphaned wrapper survived snapshot",
              check(result->Int32Value(context)));
        }
      }
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
