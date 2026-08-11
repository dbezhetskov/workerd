// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Types describing a V8 startup snapshot and how an isolate relates to one. Only high-level code
// that creates isolates (jsg::IsolateBase and its callers) needs to include this file.

#include <v8-snapshot.h>

#include <kj/one-of.h>
#include <kj/refcount.h>

namespace workerd::jsg {

// Everything needed to create a new isolate from a V8 startup snapshot:
// * `blob` is the serialized snapshot data, allocated with `new[]` by v8::SnapshotCreator.
// * `externalReferences` is the list of addresses of all C++ functions and objects referenced
//   from the snapshot. The producer and the consumer of a snapshot must register exactly the
//   same references, in the same order, or deserialization will fail.
// * `externalReferenceCursor` is the next free slot in `externalReferences`.
struct SnapshotArtifact: public kj::AtomicRefcounted {
  v8::StartupData blob{nullptr, 0};
  kj::Array<intptr_t> externalReferences;
  size_t externalReferenceCursor = 0;

  // Snapshot index of every resource type's constructor-template slot (memoizedConstructor,
  // then contextConstructor, per JSG_RESOURCE type), in iterateResourceTypeTemplates order.
  // kNoConstructorTemplate marks slots that were empty at PREPARE_SNAPSHOT. No key is needed:
  // the enumeration order is fixed at compile time and the producer and consumers of a
  // snapshot are the same binary — the same invariant externalReferences already relies on.
  // Context constructors matter as much as memoized ones: the deserialized global's brand and
  // the v8::Signature of every snapshot-baked method both reference the zygote's context
  // constructor; rebuilding it fresh on load would make baked bound methods fail V8's
  // signature check.
  static constexpr uint32_t kNoConstructorTemplate = kj::maxValue;
  kj::Vector<uint32_t> constructorTemplateIndices;

  // Wrappables that must outlive the worker they originated from.
  // Each worker created from this artifact gets a fresh snapshotClone() of one Wrappable.
  kj::Vector<kj::Own<Wrappable>> liveWrappables;

  // Module-registry handles pinned into the snapshot at PREPARE_SNAPSHOT so the loading worker
  // can overwrite its freshly re-registered entries with the baked module instances (preserving
  // identity between the baked main-module graph and runtime import()/require()). Field types
  // are raw ints to avoid coupling this header to modules.h: moduleType is a
  // jsg::ModuleRegistry::Type (capnp ModuleType), handleKind is a
  // jsg::ModuleRegistry::SnapshotHandleKind, dataIndex is a context-level
  // SnapshotCreator::AddData index.
  struct ModuleRecord {
    kj::String specifier;
    uint8_t moduleType;
    uint8_t handleKind;
    uint32_t dataIndex;
  };
  kj::Vector<ModuleRecord> moduleRecords;

  ~SnapshotArtifact() noexcept(false) {
    // v8::SnapshotCreator::CreateBlob() allocates the data with `new[]` and hands over ownership.
    delete[] blob.data;
  }

  kj::Arc<SnapshotArtifact> addRef() const {
    return addRefToThis();
  }
};

// Different views for snapshot artifacts:
// * MutableSnapshot: a zygote isolate built via v8::SnapshotCreator; holds an owning ref to
//   the artifact and fills it during IsolateBase::prepareSnapshot().
// * FinalizedSnapshot: an isolate that boots from a previously produced blob; the Arc
//   keeps the artifact alive for the isolate's entire life.
struct MutableSnapshot {
  kj::Own<SnapshotArtifact> artifact;
};
struct FinalizedSnapshot {
  kj::Arc<SnapshotArtifact> artifact;
};
using SnapshotConfig = kj::OneOf<MutableSnapshot, FinalizedSnapshot>;

}  // namespace workerd::jsg
