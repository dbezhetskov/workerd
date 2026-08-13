#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

#include <kj/filesystem.h>

namespace workerd::api {

class CommonJsModuleObject final: public jsg::Object {
 public:
  CommonJsModuleObject(jsg::Lock& js, kj::String path);

  // Snapshot-clone constructor: `exports` starts empty and is recreated lazily on first
  // access (the zygote's JS value travels through the snapshot heap, not the native side).
  explicit CommonJsModuleObject(kj::String path): path(kj::mv(path)) {}

  bool isSnapshotClonable() const override {
    return true;
  }
  kj::Maybe<kj::Own<jsg::Wrappable>> snapshotClone() const override {
    return ownAsWrappable(kj::refcounted<CommonJsModuleObject>(kj::str(path)));
  }

  jsg::JsValue getExports(jsg::Lock& js) const;
  void setExports(jsg::Lock& js, jsg::JsValue value);
  kj::StringPtr getPath() const;

  JSG_RESOURCE_TYPE(CommonJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(path, getPath);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(exports);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // none only right after a snapshot clone, until the lazy recreation in getExports().
  mutable kj::Maybe<jsg::JsRef<jsg::JsValue>> exports;
  kj::String path;
};

class CommonJsModuleContext final: public jsg::Object {
 public:
  CommonJsModuleContext(jsg::Lock& js, kj::Path path);
  CommonJsModuleContext(jsg::Lock& js, const jsg::Url& url);

  // Snapshot-clone constructor.
  CommonJsModuleContext(
      jsg::Ref<CommonJsModuleObject> module, kj::OneOf<kj::Path, jsg::Url> pathOrSpecifier)
      : module(kj::mv(module)),
        pathOrSpecifier(kj::mv(pathOrSpecifier)) {}

  bool isSnapshotClonable() const override {
    return true;
  }
  kj::Maybe<kj::Own<jsg::Wrappable>> snapshotClone() const override {
    auto clonedPath = ([&]() -> kj::OneOf<kj::Path, jsg::Url> {
      KJ_SWITCH_ONEOF(pathOrSpecifier) {
        KJ_CASE_ONEOF(path, kj::Path) {
          return path.clone();
        }
        KJ_CASE_ONEOF(specifier, jsg::Url) {
          return specifier.clone();
        }
      }
      KJ_UNREACHABLE;
    })();
    // The clone gets its own fresh CommonJsModuleObject; `exports` on both starts empty and
    // is recreated lazily (the zygote's JS values travel through the snapshot heap).
    return ownAsWrappable(kj::refcounted<CommonJsModuleContext>(
        jsg::Ref<CommonJsModuleObject>(
            kj::refcounted<CommonJsModuleObject>(kj::str(module->getPath()))),
        kj::mv(clonedPath)));
  }

  jsg::JsValue require(jsg::Lock& js, kj::String specifier);

  jsg::Ref<CommonJsModuleObject> getModule(jsg::Lock& js);

  jsg::JsValue getExports(jsg::Lock& js) const;
  void setExports(jsg::Lock& js, jsg::JsValue value);

  kj::String getFilename() const;
  kj::String getDirname() const;

  jsg::JsValue getModuleExports(jsg::Lock& js) {
    return getModule(js)->getExports(js);
  }

  JSG_RESOURCE_TYPE(CommonJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_INSTANCE_PROPERTY(__filename, getFilename);
    JSG_LAZY_INSTANCE_PROPERTY(__dirname, getDirname);
  }

  jsg::Ref<CommonJsModuleObject> module;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(module, exports);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // If pathOrSpecifier is a path, then we're using the old module registry
  // implementation. If it is a jsg::Url, then we are using the new module
  // registry implementation.
  kj::OneOf<kj::Path, jsg::Url> pathOrSpecifier;
  // none only right after a snapshot clone, until the lazy recreation in getExports().
  mutable kj::Maybe<jsg::JsRef<jsg::JsValue>> exports;
};

// Used with the original module registry implementation.
template <typename LockType>
struct CommonJsImpl: public jsg::ModuleRegistry::CommonJsModuleInfo::CommonJsModuleProvider {
  jsg::Ref<api::CommonJsModuleContext> context;
  CommonJsImpl(jsg::Lock& js, kj::Path path)
      : context(js.alloc<api::CommonJsModuleContext>(js, kj::mv(path))) {}
  KJ_DISALLOW_COPY_AND_MOVE(CommonJsImpl);
  jsg::JsObject getContext(jsg::Lock& js) override {
    auto& lock = kj::downcast<LockType>(js);
    return jsg::JsObject(lock.wrap(js.v8Context(), context.addRef()));
  }
  jsg::JsValue getExports(jsg::Lock& js) override {
    return jsg::JsValue(context->getModule(js)->getExports(js));
  }
};

#define EW_CJS_ISOLATE_TYPES api::CommonJsModuleObject, api::CommonJsModuleContext

}  // namespace workerd::api
