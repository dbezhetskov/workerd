// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-context.h"

#include "jsg.h"

#include <workerd/jsg/memory.h>

#include <v8.h>

namespace workerd::jsg {

namespace {
inline void maybeSetV8ContinuationContext(
    v8::Isolate* isolate, kj::Maybe<AsyncContextFrame&> maybeFrame) {
  v8::Local<v8::Value> value;
  KJ_IF_SOME(frame, maybeFrame) {
    value = frame.getJSWrapper(isolate);
  } else {
    value = v8::Undefined(isolate);
  }
  isolate->SetContinuationPreservedEmbedderDataV2(value);
}
}  // namespace

AsyncContextFrame::AsyncContextFrame(Lock& js, StorageEntry storageEntry) {
  KJ_IF_SOME(frame, current(js)) {
    // Propagate the storage context of the current frame (if any).
    // If current(js) returns nullptr, we assume we're in the root
    // frame and there is no storage to propagate.
    frame.storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
    for (auto& entry: frame.storage) {
      storage.insert(entry.clone(js));
    }
  }

  // This case is extremely unlikely to happen but let's handle it anyway
  // just out of an excess of caution.
  if (storageEntry.key->isDead()) return;

  storage.upsert(kj::mv(storageEntry), [](StorageEntry& existing, StorageEntry&& row) mutable {
    existing.value = kj::mv(row.value);
  });
}

AsyncContextFrame::StorageEntry::StorageEntry(kj::Arc<StorageKey> key, Value value)
    : key(kj::mv(key)),
      value(kj::mv(value)) {}

AsyncContextFrame::StorageEntry AsyncContextFrame::StorageEntry::clone(Lock& js) {
  return StorageEntry(key.addRef(), value.addRef(js));
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(Lock& js) {
  return current(js.v8Isolate);
}

kj::Maybe<Ref<AsyncContextFrame>> AsyncContextFrame::currentRef(Lock& js) {
  return currentRef(js.v8Isolate);
}

kj::Maybe<Ref<AsyncContextFrame>> AsyncContextFrame::currentRef(v8::Isolate* isolate) {
  return current(isolate).map([](AsyncContextFrame& frame) { return frame.addRef(); });
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::current(v8::Isolate* isolate) {
  auto value = isolate->GetContinuationPreservedEmbedderDataV2();
  return tryUnwrap(isolate, value.As<v8::Value>());
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::tryUnwrap(
    v8::Isolate* isolate, v8::Local<v8::Value> value) {
  KJ_IF_SOME(wrappable, Wrappable::tryUnwrapOpaque(isolate, value)) {
    AsyncContextFrame* frame = dynamic_cast<AsyncContextFrame*>(&wrappable);
    KJ_ASSERT(frame != nullptr);
    return *frame;
  }
  return kj::none;
}

Ref<AsyncContextFrame> AsyncContextFrame::create(Lock& js, StorageEntry storageEntry) {
  return js.alloc<AsyncContextFrame>(js, kj::mv(storageEntry));
}

kj::Maybe<Value&> AsyncContextFrame::get(const StorageKey& key) {
  KJ_ASSERT(!key.isDead());
  storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
  return storage.find(key).map([](auto& entry) -> Value& { return entry.value; });
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<AsyncContextFrame&> resource)
    : Scope(js.v8Isolate, resource) {}

AsyncContextFrame::Scope::Scope(v8::Isolate* ptr, kj::Maybe<AsyncContextFrame&> maybeFrame)
    : isolate(ptr),
      prior(AsyncContextFrame::currentRef(ptr)) {
  maybeSetV8ContinuationContext(isolate, maybeFrame);
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<Ref<AsyncContextFrame>>& resource)
    : Scope(js.v8Isolate, resource.map([](Ref<AsyncContextFrame>& frame) -> AsyncContextFrame& {
        return *frame.get();
      })) {}

AsyncContextFrame::Scope::~Scope() noexcept(false) {
  maybeSetV8ContinuationContext(isolate,
      prior.map([](Ref<AsyncContextFrame>& frame) -> AsyncContextFrame& { return *frame; }));
}

AsyncContextFrame::StorageScope::StorageScope(Lock& js, kj::Arc<StorageKey> key, Value store)
    : frame(AsyncContextFrame::create(js, StorageEntry(kj::mv(key), kj::mv(store)))),
      scope(js, *frame) {}

v8::Local<v8::Object> AsyncContextFrame::getJSWrapper(v8::Isolate* isolate) {
  KJ_IF_SOME(handle, tryGetHandle(isolate)) {
    return handle;
  }
  return attachOpaqueWrapper(isolate->GetCurrentContext(), true);
}

v8::Local<v8::Object> AsyncContextFrame::getJSWrapper(Lock& js) {
  return getJSWrapper(js.v8Isolate);
}

void AsyncContextFrame::jsgVisitForGc(GcVisitor& visitor) {
  // tracing will make the members weak and will allow
  // them to be gc'd, which is not what we want.
}
}  // namespace workerd::jsg
