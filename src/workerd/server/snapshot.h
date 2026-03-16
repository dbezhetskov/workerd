// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-source.h>

#include <kj/array.h>

namespace workerd::jsg {
class V8System;
}

namespace workerd::server {

// Compiles and evaluates all ES modules from the given ModulesSource in a plain V8 context
// (without JSG TypeWrapper or workerd API bindings), then serializes the resulting V8 heap
// state into a startup snapshot blob.
kj::Array<kj::byte> createWorkerSnapshot(
    jsg::V8System& v8System, const WorkerSource::ModulesSource& source);

// Executes the Service Worker script in a plain V8 context with a mock `addEventListener`
// that records {type, handler} pairs into `globalThis.__snapshot_listeners__` instead of
// registering with the workerd runtime.  The resulting V8 heap — including the compiled
// handler closures — is serialized into a startup snapshot blob.
//
// At Worker startup with this snapshot, `Context::FromSnapshot(isolate, 0)` restores the
// script execution context and `__snapshot_listeners__` is replayed via the real workerd
// `addEventListener`, skipping re-execution of the top-level script.
kj::Array<kj::byte> createWorkerSnapshot(
    jsg::V8System& v8System, const WorkerSource::ScriptSource& source);

}  // namespace workerd::server
