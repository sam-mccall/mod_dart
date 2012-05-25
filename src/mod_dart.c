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
#include "apr_hash.h"
#include "apr_strings.h"

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

typedef struct dart_dir_config {
  NullableBool debug;
} dart_dir_config;

typedef struct dart_server_config {
  dart_server_config *base;
  apr_hash_t *snapshots;
} dart_server_config;

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

static Dart_Handle ScriptSnapshotLibraryTagHandler(Dart_LibraryTag type, Dart_Handle library, Dart_Handle url) {
  if (type == kImportTag) {
    const char* curl;
    Dart_Handle result = Dart_StringToCString(url, &curl);
    if (Dart_IsError(result)) return result;
    if (!strcmp(curl, "dart:apache")) {
      return ApacheLibraryLoad();
    }
  }
  return MasterSnapshotLibraryTagHandler(type, library, url);
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

static uint8_t *getScriptSnapshot(request_rec *r) {
  dart_server_config *cfg = (dart_server_config*) ap_get_module_config(r->server->module_config, &dart_module);
  while (cfg->base) cfg = cfg->base;
//  printf("Looking up snapshot for %s\n", r->filename);
  void* result = apr_hash_get(cfg->snapshots, r->filename, APR_HASH_KEY_STRING);
//  printf("Result was %p\n", result);
  return (uint8_t*) result;
}

static bool isDebug(request_rec *r) {
  dart_dir_config *cfg = (dart_dir_config*) ap_get_module_config(r->per_dir_config, &dart_module);
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
  uint8_t *snapshot = getScriptSnapshot(r);
  Dart_Handle library;
  if (snapshot) {
    library = Dart_LoadScriptFromSnapshot(snapshot); // TODO did we need to load the main snapshot?
  } else {
    Dart_Handle script = LoadFile(r->filename);
    AP_WARN(r, "Loaded script file");
    if (Dart_IsNull(script)) return HTTP_NOT_FOUND;
    library = Dart_IsError(script) ? script : Dart_LoadScript(Dart_NewString("main"), script);
  }
  AP_WARN(r, "Loaded script into VM");
  if (Dart_IsError(library)) return fatal(r, "Failed to load script: %s", library);
  Dart_Handle result = Dart_Invoke(library, Dart_NewString("main"), 0, NULL);
  AP_WARN(r, "Executed script");
  if (Dart_IsError(result)) return fatal(r, "Failed to execute main(): %s", result);
  AP_WARN(r, "Finished");
  return OK;
}

Dart_Handle create_master_snapshot(apr_pool_t *pool, uint8_t **target, const char* name) {
  printf("Creating master snapshot\n");

  Dart_SetLibraryTagHandler(MasterSnapshotLibraryTagHandler);
  Dart_Handle result = Builtin::LoadLibrary(Builtin::kBuiltinLibrary);
  if (Dart_IsError(result)) return result;
  result = ApacheLibraryLoad();
  if (Dart_IsError(result)) return result;
  uint8_t *buffer;
  intptr_t size;
  result = Dart_CreateSnapshot(&buffer, &size);
  if (Dart_IsError(result)) return result;
  *target = (uint8_t*) apr_pcalloc(pool, size); // This lives forever
  if (!*target) return Dart_Error("Failed to allocate %ld bytes for master snapshot", size);
  memmove(*target, buffer, size);
  printf("Created master snapshot\n");
  return Dart_Null();
}

Dart_Handle create_script_snapshot(apr_pool_t *pool, uint8_t **target, const char *name) {
  Dart_SetLibraryTagHandler(ScriptSnapshotLibraryTagHandler);
  Dart_Handle result = LoadFile(name);
  printf("Loaded script file %s\n", name);
  if (Dart_IsNull(result)) return Dart_Error("Script not found: %s", name);
  printf("and was found\n");
  if (Dart_IsError(result)) return result;
  printf("And no error\n");
  result = Dart_LoadScript(Dart_NewString("main"), result);
  if (Dart_IsError(result)) return result;
  printf("And loaded script\n");
  uint8_t *buffer;
  intptr_t size;
  result = Dart_CreateScriptSnapshot(&buffer, &size);
  if (Dart_IsError(result)) return result;
  printf("and created snapshot");
  *target = (uint8_t*) apr_pcalloc(pool, size); // This lives forever
  if (!*target) return Dart_Error("Failed to allocate %ld bytes for snapshot of %s", size, name);
  memmove(*target, buffer, size);
  printf("Done with script snapshot: %s\n", name);
  return Dart_Null();
}

bool create_snapshot(apr_pool_t *pool, uint8_t** target, const char* name, uint8_t *base_snapshot, Dart_Handle (*creator)(apr_pool_t *pool, uint8_t **target, const char* name), char **error) {
  *error = NULL;
  Dart_Isolate isolate = Dart_CreateIsolate(name, "main", base_snapshot, NULL, error);
  if (!isolate) return false;
  Dart_EnterScope();
  Dart_Handle result = creator(pool, target, name);
  if (Dart_IsError(result)) *error = apr_pstrdup(pool, Dart_GetError(result));
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  printf("Tried to create a snapshot for %s, error state=%p\n", name, *error);
  return *error == NULL;
}

int dart_snapshots(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *server) {
  // Modules are loaded twice, only actually create the snapshot on second load
  void *data = NULL;
  apr_pool_userdata_get(&data, "dart_dryrun_complete", server->process->pool);
  if (!data) { // This is the dry-run
    apr_pool_userdata_set((const void*) 1, "dart_dryrun_complete", apr_pool_cleanup_null, server->process->pool);
    return OK;
  }
  // Only create snapshots in the root server
  dart_server_config *cfg = (dart_server_config*) ap_get_module_config(server->module_config, &dart_module);
  if (cfg->base) return OK;
  initializeState = Dart_SetVMFlags(0, NULL) && Dart_Initialize(IsolateCreate, IsolateInterrupt);

  char* error;
  if (!create_snapshot(server->process->pool, &master_snapshot_buffer, "master", NULL, create_master_snapshot, &error)) {
    ap_log_error(APLOG_MARK, LOG_WARNING, 0, server, "Master snapshot failed: %s", error);
    return 1;
  }
  uint8_t *val;
  const void *key;
  for (apr_hash_index_t *p = apr_hash_first(ptemp, cfg->snapshots); p; p = apr_hash_next(p)) {
    apr_hash_this(p, &key, NULL, NULL);
    // TODO use pconf instead of server->process->pool?
    if (!create_snapshot(server->process->pool, &val, (const char*) key, master_snapshot_buffer, create_script_snapshot, &error)) {
      ap_log_error(APLOG_MARK, LOG_WARNING, 0, server, "Script snapshot failed for %s: %s", (const char*) key, error);
      val = NULL;
    }
    apr_hash_set(cfg->snapshots, key, APR_HASH_KEY_STRING, (const void*) val);
  }

  return OK;
}

static void dart_register_hooks(apr_pool_t *p) {
  ap_hook_handler(dart_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_child_init(dart_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_post_config(dart_snapshots, NULL, NULL, APR_HOOK_MIDDLE);
}

static const char *dart_set_debug(cmd_parms *cmd, void *cfg_, const char *arg) {
  dart_dir_config *cfg = (dart_dir_config*) cfg_;
  cfg->debug = strcasecmp("on", arg) ? kNo : kYes;
  return NULL;
}

static const char *dart_set_snapshot(cmd_parms *cmd, void *cfg_, const char *arg) {
  dart_server_config *cfg = (dart_server_config*) ap_get_module_config(cmd->server->module_config, &dart_module);
  while (cfg->base) cfg = cfg->base;
  printf("Adding snapshot for %s\n", arg);
  apr_hash_set(cfg->snapshots, arg, APR_HASH_KEY_STRING, (void*) 1);
  return NULL;
}

static const command_rec dart_directives[] = {
  AP_INIT_TAKE1("DartDebug", (cmd_func) dart_set_debug, NULL, OR_ALL, "Whether error messages should be sent to the browser"),
  AP_INIT_TAKE1("DartSnapshot", (cmd_func) dart_set_snapshot, NULL, OR_ALL, "A dart file to be snapshotted for fast loading"),
  { NULL },
};

static void *create_dir_conf(apr_pool_t* pool, char* context_) {
  dart_dir_config *cfg = (dart_dir_config*) apr_pcalloc(pool, sizeof(dart_dir_config));
  if (cfg) {
    cfg->debug = kNull;
  }
  return cfg;
}

static void *merge_dir_conf(apr_pool_t* pool, void* base_, void* add_) {
  dart_dir_config *base = (dart_dir_config*) base_;
  dart_dir_config *add = (dart_dir_config*) add_;
  dart_dir_config *cfg = (dart_dir_config*) apr_pcalloc(pool, sizeof(dart_dir_config));
  cfg->debug = add->debug ? add->debug : base->debug;
  return cfg;
}

static void *create_server_conf(apr_pool_t* pool, server_rec *server) {
  printf("Creating server config\n");
  dart_server_config *cfg = (dart_server_config*) apr_pcalloc(pool, sizeof(dart_server_config));
  if (cfg) {
    cfg->base = NULL;
    cfg->snapshots = apr_hash_make(pool);
  }
  return cfg;
}

static void *merge_server_conf(apr_pool_t *pool, void *base_, void *add_) {
  printf("Merging server configs\n");
  dart_server_config *base = (dart_server_config*) base_;
  dart_server_config *add = (dart_server_config*) add_;
  dart_server_config *cfg = (dart_server_config*) apr_pcalloc(pool, sizeof(dart_server_config));
  cfg->base = base;
  cfg->snapshots = NULL;
  while (base->base) base = base->base;
  void *val;
  const void *key;
  for (apr_hash_index_t *p = apr_hash_first(pool, add->snapshots); p; p = apr_hash_next(p)) {
    apr_hash_this(p, &key, NULL, &val);
    printf("Copying key into base: %s\n", (char*) key);
    apr_hash_set(base->snapshots, key, APR_HASH_KEY_STRING, val);
  }
  return cfg;
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA dart_module = {
  STANDARD20_MODULE_STUFF, 
  create_dir_conf,
  merge_dir_conf,
  create_server_conf,
  merge_server_conf,
  dart_directives,
  dart_register_hooks
};
