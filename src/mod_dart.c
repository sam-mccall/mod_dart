// Copyright 2012 Google Inc.
// Licensed under the Apache License, Version 2.0 (the "License")
// You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

#include <sys/stat.h>

#include "include/dart_api.h"
#include "bin/builtin.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_config.h"

#define AP_WARN(r, message, ...) do{\
  struct timeval tv;\
  gettimeofday(&tv, NULL);\
  fprintf(stderr, "At %ld.%06d\n", tv.tv_sec, tv.tv_usec);\
  ap_log_rerror(APLOG_MARK, LOG_WARNING, 0, r, message, ##__VA_ARGS__);\
} while(0)

extern const uint8_t* snapshot_buffer; // corelib, dart:io etc
static uint8_t *master_snapshot_buffer = 0; // snapshot_buffer + dart:apache

typedef enum {
  kNull = 0,
  kNo,
  kYes
} NullableBool;

typedef struct dart_config {
  NullableBool debug;
} dart_config;

extern module AP_MODULE_DECLARE_DATA dart_module;
extern "C" Dart_Handle ApacheLibraryInit(request_rec* r);
extern "C" Dart_Handle ApacheLibraryLoad();

static bool IsolateCreate(const char* name, const char* main, void* data, char** error) {
  if (!master_snapshot_buffer) {
    *((const char**) error) = "master_snapshot_buffer == NULL";
    return false;
  }
  Dart_Isolate isolate = Dart_CreateIsolate(name, main, master_snapshot_buffer, data, error);
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

Dart_Handle LoadFile(const char* cpath) {
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

static Dart_Handle LibraryTagHandler(Dart_LibraryTag type, Dart_Handle library, Dart_Handle url) {
  if (type == kCanonicalizeUrl) return url;
  if (type == kLibraryTag) return Dart_Null();

  Dart_Handle source;
  const char* curl;
  Dart_Handle result = Dart_StringToCString(url, &curl);
  if (Dart_IsError(result)) return result;
  if (strstr(curl, ":")) {
    return Dart_Error("Unknown qualified import: %s", curl);
  } else {
    source = LoadFile(curl); // TODO
  }

  if (Dart_IsError(source)) return source;
  if (type == kSourceTag) return Dart_LoadSource(library, url, source);
  return Dart_LoadLibrary(url, source);
}

static Dart_Handle MasterSnapshotLibraryTagHandler(Dart_LibraryTag type, Dart_Handle library, Dart_Handle url) {
  if (type == kCanonicalizeUrl) return url;
  if (type == kLibraryTag) return Dart_Null();
  if (type == kSourceTag) return Dart_Error("Unexpected #source in master snapshot libraries");

  const char* curl;
  Dart_Handle result = Dart_StringToCString(url, &curl);
  if (Dart_IsError(result)) return result;

  if (!strcmp(curl, "dart:utf")) {
    return Builtin::LoadLibrary(Builtin::kUtfLibrary);
  } else if (!strcmp(curl, "dart:uri")) {
    return Builtin::LoadLibrary(Builtin::kUriLibrary);
  } else if (!strcmp(curl, "dart:io")) {
    return Builtin::LoadLibrary(Builtin::kIOLibrary);
  } else if (!strcmp(curl, "dart:crypto")) {
    return Builtin::LoadLibrary(Builtin::kCryptoLibrary);
  } else if (!strcmp(curl, "dart:json")) {
    return Builtin::LoadLibrary(Builtin::kJsonLibrary);
  } else {
    return Dart_Error("Unknown qualified import %s", curl);
  }
}

static bool dart_isolate_create(request_rec *r) {
  char* error;
  if (!IsolateCreate("name", "main", NULL, &error)) {
    AP_WARN(r, "Failed to create isolate: %s", error);
    return false;
  }
  Dart_EnterScope();
  Dart_SetLibraryTagHandler(LibraryTagHandler);
  Dart_Handle result = ApacheLibraryInit(r);
  if (Dart_IsError(result)) {
    AP_WARN(r, "Failed to initialize Apache library: %s", Dart_GetError(result));
    return false;
  }
  return true;
}

static apr_status_t dart_isolate_destroy(void* ctx) {
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  return OK;
}

static bool isDebug(request_rec *r) {
  dart_config *cfg = (dart_config*) ap_get_module_config(r->per_dir_config, &dart_module);
  return cfg->debug == kYes;
}

static bool initializeState = false;
static void dart_child_init(apr_pool_t *p, server_rec *s) {
  // This is allowed to fail, we may have already initialized the VM for snapshot creation
  initializeState |= (Dart_SetVMFlags(0, NULL) && Dart_Initialize(IsolateCreate, IsolateInterrupt));
}

static int fatal(request_rec *r, const char *format, Dart_Handle error) {
  AP_WARN(r, format, Dart_GetError(error));
  if (!isDebug(r)) return HTTP_INTERNAL_SERVER_ERROR;
  r->content_type = "text/plain";
  r->status = HTTP_INTERNAL_SERVER_ERROR;
  ap_rprintf(r, format, Dart_GetError(error));
  ap_rprintf(r, "\n");
  return OK;  
}

static int dart_handler(request_rec *r) {
  AP_WARN(r, "Got request");
  if (strcmp(r->handler, "dart")) {
    return DECLINED;
  }
  if (!initializeState) {
    AP_WARN(r, "Failed to initialize dart VM");  
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  AP_WARN(r, "Creating isolate");
  if (!dart_isolate_create(r)) return HTTP_INTERNAL_SERVER_ERROR;
  apr_pool_cleanup_register(r->pool, NULL, dart_isolate_destroy, apr_pool_cleanup_null);
  AP_WARN(r, "Created isolate");
  Dart_Handle script = LoadFile(r->filename);
  AP_WARN(r, "Loaded script file");
  if (Dart_IsNull(script)) return HTTP_NOT_FOUND;
  Dart_Handle library = Dart_IsError(script) ? script : Dart_LoadScript(Dart_NewString("main"), script);
  AP_WARN(r, "Loaded script into VM");
  if (Dart_IsError(library)) return fatal(r, "Failed to load library: %s", library);
  Dart_Handle result = Dart_Invoke(library, Dart_NewString("main"), 0, NULL);
  AP_WARN(r, "Executed script");
  if (Dart_IsError(result)) return fatal(r, "Failed to execute main(): %s", result);
  AP_WARN(r, "Finished");
  return OK;
}

int dart_master_snapshot(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *server) {
  // Modules are loaded twice, only actually create the snapshot on second load
  void *data = NULL;
  apr_pool_userdata_get(&data, "dart_dryrun_complete", server->process->pool);
  if (!data) { // This is the dry-run
      apr_pool_userdata_set((const void *)1, "dart_dryrun_complete", apr_pool_cleanup_null, server->process->pool);
      printf("Dry run!\n");
      return OK;
  }
  printf("Creating snapshot\n");

  initializeState = Dart_SetVMFlags(0, NULL) && Dart_Initialize(IsolateCreate, IsolateInterrupt);
  char* error;
  Dart_Isolate isolate = Dart_CreateIsolate("master", "main", NULL, NULL, &error);
  if (!isolate) {
    ap_log_error(APLOG_MARK, LOG_WARNING, 0, server, "CreateIsolate for master snapshot failed: %s", error);
    return 1;
  }
  Dart_EnterScope();
  Dart_SetLibraryTagHandler(MasterSnapshotLibraryTagHandler);
  Dart_Handle result = Builtin::LoadLibrary(Builtin::kBuiltinLibrary);
  if (Dart_IsError(result)) {
    ap_log_error(APLOG_MARK, LOG_WARNING, 0, server,
        "Builtin::LoadLibrary(kBuiltinLibrary) for master snapshot failed: %s", Dart_GetError(result));
    Dart_ShutdownIsolate();
    return 1;
  }
  result = ApacheLibraryLoad();
  if (Dart_IsError(result)) {
    ap_log_error(APLOG_MARK, LOG_WARNING, 0, server,
        "ApacheLibraryLoad for master snapshot failed: %s", Dart_GetError(result));
    Dart_ShutdownIsolate();
    return 1;
  }
  uint8_t *buffer;
  intptr_t size;
  result = Dart_CreateSnapshot(&buffer, &size);
  if (Dart_IsError(result)) {
    ap_log_error(APLOG_MARK, LOG_WARNING, 0, server,
        "Dart_CreateSnapshot for master snapshot failed: %s", Dart_GetError(result));
    Dart_ShutdownIsolate();
    return 1;
  }
  master_snapshot_buffer = (uint8_t*) apr_pcalloc(server->process->pool, size); // This lives forever
  if (!master_snapshot_buffer) {
    ap_log_error(APLOG_MARK, LOG_WARNING, 0, server, "Failed to allocate %ld bytes for master snapshot", size);
    Dart_ShutdownIsolate();
    return 1;
  }
  memmove(master_snapshot_buffer, buffer, size);
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  printf("Created snapshot\n");
  return OK;
}

static void dart_register_hooks(apr_pool_t *p) {
  ap_hook_handler(dart_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_child_init(dart_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_post_config(dart_master_snapshot, NULL, NULL, APR_HOOK_MIDDLE);
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
