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

#define AP_WARN(r, message, ...) ap_log_error(APLOG_MARK, LOG_WARNING, 0, (r)->server, message "\n", ##__VA_ARGS__)

extern const uint8_t* snapshot_buffer;

typedef enum {
  kNull = 0,
  kNo,
  kYes
} NullableBool;

typedef struct dart_config {
  NullableBool debug;
} dart_config;

extern module AP_MODULE_DECLARE_DATA dart_module;

static bool IsolateCreate(const char* name, void* data, char** error) {
  Dart_Isolate isolate = Dart_CreateIsolate(name, snapshot_buffer, data, error);
  if (!isolate) return false;
  Dart_EnterScope();
  Builtin::SetNativeResolver(Builtin::kBuiltinLibrary);
  Builtin::SetNativeResolver(Builtin::kIOLibrary);
  return true;
}

static bool IsolateInterrupt() {
  printf("Isolate interrupted!\n");
  return true;
}

static Dart_Handle LoadFile(const char* cpath) {
  struct stat status;
  if (stat(cpath, &status)) {
    if (errno == ENOENT) return Dart_Null();
    return Dart_Error("Couldn't stat %s: %s", cpath, strerror(errno));
  }

  if (!status.st_size) return Dart_NewString("");

  char *bytes = (char*) malloc(status.st_size + 1);
  if (!bytes) return Dart_Error("Failed to malloc %d bytes for %s: %s", status.st_size, cpath, strerror(errno));

  FILE *fh = fopen(cpath, "r");
  if (!fh) {
    free(bytes);
    return Dart_Error("Failed to open %s for read: %s", cpath, strerror(errno));
  }

  int total = 0, r;
  while (total < status.st_size) {
    r = fread(&(bytes[total]), 1, status.st_size - total + 1, fh);
    if (r == 0) {
      free(bytes);
      fclose(fh);
      return Dart_Error("File was shorter: %s expected %d got %d", cpath, status.st_size, total);
    }
    total += r;
  }

  if (!feof(fh) || total > status.st_size) {
    free(bytes);
    fclose(fh);
    return Dart_Error("File was longer: %s expected %d", cpath, status.st_size);
  }

  fclose(fh);
  bytes[status.st_size] = 0;
  Dart_Handle result = Dart_NewString(bytes);
  free(bytes);
  return result;
}

static Dart_Handle LibraryTagHandler(Dart_LibraryTag type, Dart_Handle library, Dart_Handle url, Dart_Handle import_map) {
  if (type == kCanonicalizeUrl) return url;
  if (type == kLibraryTag) return Dart_Null();

  Dart_Handle source;
  const char* curl;
  Dart_Handle result = Dart_StringToCString(url, &curl);
  if (Dart_IsError(result)) return result;
  if (type == kImportTag && !strcmp("dart:apache", curl)) {
    source = LoadFile("/usr/local/google/home/sammccall/gits/mod_dart/src/mod_dart.dart"); // TODO
  } else if (strstr(curl, ":")) {
    return Dart_Error("Unknown qualified import: %s", curl);
  } else {
    source = LoadFile(curl); // TODO
  }

  if (Dart_IsError(source)) return source;
  if (type == kImportTag) return Dart_LoadLibrary(url, source, import_map);
  return Dart_LoadSource(library, url, source);
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

static bool isDebug(request_rec *r) {
  dart_config *cfg = (dart_config*) ap_get_module_config(r->per_dir_config, &dart_module);
  return cfg->debug == kYes;
}

static bool initializeState = false;
static void dart_child_init(apr_pool_t *p, server_rec *s) {
  initializeState = Dart_SetVMFlags(0, NULL) && Dart_Initialize(IsolateCreate, IsolateInterrupt);
}

static int dart_handler(request_rec *r) {
  if (strcmp(r->handler, "dart")) {
    return DECLINED;
  }
  if (!initializeState) {
    AP_WARN(r, "Failed to initialize dart VM");  
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  if (!dart_isolate_create(r)) return HTTP_INTERNAL_SERVER_ERROR;
  Dart_Handle script = LoadFile(r->filename);
  if (Dart_IsNull(script)) return HTTP_NOT_FOUND;
  Dart_Handle library = Dart_IsError(script) ? script : Dart_LoadScript(Dart_NewString("main"), script, Dart_Null());
  if (Dart_IsError(library)) {
    AP_WARN(r, "Failed to load library: %s", Dart_GetError(library));
    if (!isDebug(r)) {
      dart_isolate_destroy(r);
      return HTTP_INTERNAL_SERVER_ERROR;      
    }
    r->content_type = "text/plain";
    r->status = HTTP_INTERNAL_SERVER_ERROR;
    ap_rprintf(r, "Failed to load library: %s\n", Dart_GetError(library));
    dart_isolate_destroy(r);
    return OK;
  }
  Dart_Handle result = Dart_Invoke(library, Dart_NewString("main"), 0, NULL);
  if (Dart_IsError(result)) {
    AP_WARN(r, "Failed to execute main(): %s", Dart_GetError(result));
    if (!isDebug(r)) {
      dart_isolate_destroy(r);
      return HTTP_INTERNAL_SERVER_ERROR;
    }
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
  ap_hook_child_init(dart_child_init, NULL, NULL, APR_HOOK_MIDDLE);
}

static const char *dart_set_debug(cmd_parms *cmd, void *cfg_, const char *arg) {
  dart_config *cfg = (dart_config*) cfg_;
  cfg->debug = strcasecmp("on", arg) ? kNo : kYes;
  return NULL;
}

static const command_rec dart_directives[] = {
  AP_INIT_TAKE1("DartDebug", (cmd_func) dart_set_debug, NULL, ACCESS_CONF, "Whether error messages should be sent to the browser"),
  { NULL },
};

static void *create_dir_conf(apr_pool_t* pool, char* context_) {
  dart_config *cfg = (dart_config*) apr_pcalloc(pool, sizeof(dart_config));
  if (cfg) {
    cfg->debug = kNull;
  }
  return cfg;
}

static void *merge_dir_conf(apr_pool_t* pool, void* base_, void* add_) {
  dart_config *base = (dart_config*) base_;
  dart_config *add = (dart_config*) add_;
  dart_config *cfg = (dart_config*) apr_pcalloc(pool, sizeof(dart_config));
  cfg->debug = add->debug ? add->debug : base->debug;
  return cfg;
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA dart_module = {
  STANDARD20_MODULE_STUFF, 
  create_dir_conf,          /* create per-dir  config structures */
  merge_dir_conf,          /* merge  per-dir  config structures */
  NULL,          /* create per-server config structures */
  NULL,          /* merge  per-server config structures */
  dart_directives,          /* table of config file commands     */
  dart_register_hooks  /* register hooks            */
};

