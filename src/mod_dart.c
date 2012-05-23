#ifndef DEBUG
#define NDEBUG
#endif

#include "include/dart_api.h"
#include "bin/builtin.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_config.h"

#define AP_WARN(r, ...) ap_log_error(APLOG_MARK, LOG_WARNING, 0, (r)->server, __VA_ARGS__)

extern const uint8_t* snapshot_buffer;

static bool IsolateCreate(const char* name, void* data, char** error) {
  Dart_Isolate isolate = Dart_CreateIsolate(name, snapshot_buffer, data, error);
  if (!isolate) return false;
  Dart_EnterScope();
  Builtin::SetNativeResolver(Builtin::kBuiltinLibrary);
  Builtin::SetNativeResolver(Builtin::kCryptoLibrary);
  Builtin::SetNativeResolver(Builtin::kIOLibrary);
  Builtin::SetNativeResolver(Builtin::kJsonLibrary);
  Builtin::SetNativeResolver(Builtin::kUriLibrary);
  Builtin::SetNativeResolver(Builtin::kUtfLibrary);
  return true;
}

static bool IsolateInterrupt() {
  printf("Isolate interrupted!\n");
  return true;
}

static Dart_Handle LoadFile(Dart_Handle path) {
  const char* cpath;
  Dart_Handle result = Dart_StringToCString(path, &cpath);
  if (Dart_IsError(result)) return result;
  return Dart_NewString("quick brown fox!");
}

static Dart_Handle LibraryTagHandler(Dart_LibraryTag type, Dart_Handle library, Dart_Handle url, Dart_Handle import_map) {
  if (type == kCanonicalizeUrl) return url;
  if (type == kLibraryTag) return Dart_Null();
  Dart_Handle source = LoadFile(url);
  if (Dart_IsError(source)) return source;
  if (type == kImportTag) return Dart_LoadLibrary(url, source, import_map);
  return Dart_LoadSource(library, url, source);
}

static bool dart_vm_init(request_rec *r) {
  // TODO threadsafe
  static int initialized = 0;
  static int initializeState = 0;
  if (initialized) {
    if (!initializeState) AP_WARN(r, "Previously failed to initialize dart VM");
    return initializeState;
  }
  initialized = 1;
  initializeState = Dart_SetVMFlags(0, NULL) && Dart_Initialize(IsolateCreate, IsolateInterrupt);
  if (!initializeState) AP_WARN(r, "Failed to initialize dart VM");
  return initializeState;
}

static bool dart_isolate_create(request_rec *r) {
  char* error;
  if (!IsolateCreate("name", NULL, &error)) {
    AP_WARN(r, "Failed to create isolate: %s", error);
    return false;
  }
  Dart_EnterScope();
  Dart_SetLibraryTagHandler(LibraryTagHandler);
  return true;
}

static void dart_isolate_destroy(request_rec *r) {
  Dart_ExitScope();
  Dart_ShutdownIsolate();
}

static Dart_Handle dart_library_create(request_rec *r) {
  return Dart_LoadScript(Dart_NewString("main"), Dart_NewString("#import('dart:io');\nmain() => stdout.writeString('hello\\n');"), Dart_Null());
}

static int dart_handler(request_rec *r) {
  if (strcmp(r->handler, "dart")) {
    return DECLINED;
  }
  if (!dart_vm_init(r)) return HTTP_INTERNAL_SERVER_ERROR;
  if (!dart_isolate_create(r)) return HTTP_INTERNAL_SERVER_ERROR;
  Dart_Handle library = dart_library_create(r);
  if (Dart_IsError(library)) {
    AP_WARN(r, "Failed to load library: %s", Dart_GetError(library));
    dart_isolate_destroy(r);
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  Dart_Handle result = Dart_Invoke(library, Dart_NewString("main"), 0, NULL);
  if (Dart_IsError(result)) {
    AP_WARN(r, "Failed to execute main(): %s", Dart_GetError(result));
    r->content_type = "text/plain";
    r->status = HTTP_INTERNAL_SERVER_ERROR;
    ap_rprintf(r, "Failed to execute main(): %s\n", Dart_GetError(result));
    dart_isolate_destroy(r);
    return OK;
  }
  dart_isolate_destroy(r);

  r->content_type = "text/plain";
  if (!r->header_only) ap_rputs("main() executed successfully.\n", r);
  return OK;
}

static void dart_register_hooks(apr_pool_t *p) {
  ap_hook_handler(dart_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA dart_module = {
  STANDARD20_MODULE_STUFF, 
  NULL,          /* create per-dir  config structures */
  NULL,          /* merge  per-dir  config structures */
  NULL,          /* create per-server config structures */
  NULL,          /* merge  per-server config structures */
  NULL,          /* table of config file commands     */
  dart_register_hooks  /* register hooks            */
};

