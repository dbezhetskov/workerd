// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "module.h"

#include <workerd/io/features.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api::node {

bool ModuleUtil::isBuiltin(kj::String specifier) {
  return jsg::checkNodeSpecifier(specifier) != kj::none;
}

jsg::JsValue ModuleUtil::createRequire(jsg::Lock& js, kj::String path) {
  // Node.js requires that the specifier path is a File URL or an absolute
  // file path string. To be compliant, we will convert whatever specifier
  // is into a File URL if possible, then take the path as the actual
  // specifier to use.
  auto parsed = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(path.asPtr(), "file:///"_kj), TypeError,
      "The argument must be a file URL object, "
      "a file URL string, or an absolute path string.");

  if (FeatureFlags::get(js).getNewModuleRegistry()) {
    // The require logic lives in a static trampoline (registered as a V8 external reference);
    // the referrer travels as the function's `data` so the function is snapshot-serializable.
    return jsg::JsValue(jsg::check(v8::Function::New(js.v8Context(),
        jsg::modules::getRequireCallback(), v8::Local<v8::String>(js.str(parsed.getHref())))));
  }

  // We do not currently handle specifiers as URLs, so let's treat any
  // input that has query string params or hash fragments as errors.
  if (parsed.getSearch().size() > 0 || parsed.getHash().size() > 0) {
    JSG_FAIL_REQUIRE(
        Error, "The specifier must not have query string parameters or hash fragments.");
  }

  // The specifier must be a file: URL
  JSG_REQUIRE(parsed.getProtocol() == "file:"_kj, TypeError, "The specifier must be a file: URL.");

  // The require logic lives in a static trampoline (registered as a V8 external reference);
  // the referrer travels as the function's `data` so the function is snapshot-serializable.
  return jsg::JsValue(jsg::check(v8::Function::New(js.v8Context(), jsg::getLegacyRequireCallback(),
      v8::Local<v8::String>(js.str(parsed.getPathname())))));
}

}  // namespace workerd::api::node
