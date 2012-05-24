#ifndef DEBUG
#define NDEBUG
#endif

#include <stdio.h>
#include "include/dart_api.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "apr_buckets.h"
#include "apr_strings.h"

#define AP_WARN(r, message, ...) ap_log_error(APLOG_MARK, LOG_WARNING, 0, (r)->server, message "\n", ##__VA_ARGS__)
extern Dart_Handle LoadFile(const char* cpath);

static void Throw(const char* library, const char* exception, const char* message) {
  Dart_Handle lib = Dart_LookupLibrary(Dart_NewString(library));
  if (Dart_IsError(lib)) Dart_PropagateError(lib);
  Dart_Handle cls = Dart_GetClass(lib, Dart_NewString(exception));
  if (Dart_IsError(cls)) Dart_PropagateError(cls);
  Dart_Handle msg = message ? Dart_NewString(message) : Dart_Null();
  Dart_Handle exc = Dart_New(cls, Dart_Null(), message ? 1 : 0, &msg);
  if (Dart_IsError(exc)) Dart_PropagateError(exc);
  Dart_PropagateError(Dart_ThrowException(exc));
}

static request_rec *get_request(Dart_Handle request) {
  intptr_t rptr;
  Dart_GetNativeInstanceField(request, 0, &rptr);
  if (!rptr) Throw("dart:core", "Exception", "request.record_rec was NULL!");
  return (request_rec*) rptr;
}

static void ThrowIfError(int code, const char* name, request_rec *r) {
  if (code) {
    char buf[1024];
    apr_strerror(code, buf, 1024);
    Throw("dart:io", "StreamException", apr_psprintf(r->pool, "%s failed: %s", name, buf));    
  }
}

static void Apache_Request_Write(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  
  Dart_Handle text = Dart_GetNativeArgument(arguments, 1);
  const char* ctext;
  Dart_StringToCString(text, &ctext);

  apr_bucket_brigade *out = apr_brigade_create(r->pool, r->connection->bucket_alloc);
  APR_BRIGADE_INSERT_TAIL(out, apr_bucket_transient_create(ctext, strlen(ctext), out->bucket_alloc));
  ThrowIfError(ap_pass_brigade(r->output_filters, out), "ap_pass_brigade", r);

  Dart_ExitScope();
}

static void Apache_Request_Flush(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));

  apr_bucket_brigade *out = apr_brigade_create(r->pool, r->connection->bucket_alloc);
  APR_BRIGADE_INSERT_TAIL(out, apr_bucket_flush_create(out->bucket_alloc));
  ThrowIfError(ap_pass_brigade(r->output_filters, out), "ap_pass_brigade", r);

  Dart_ExitScope();
}

static Dart_NativeFunction NativeResolver(Dart_Handle name, int args) {
  const char* cname;
  if (Dart_IsError(Dart_StringToCString(name, &cname))) return NULL; // not enough context to log!
  if (!strcmp(cname, "Apache_Request_Write") && (args == 2)) return Apache_Request_Write;
  if (!strcmp(cname, "Apache_Request_Flush") && (args == 1)) return Apache_Request_Flush;
  return NULL;
}

static Dart_Handle ApacheLibraryLoad() {
  Dart_Handle source = LoadFile("/usr/local/google/home/sammccall/gits/mod_dart/src/mod_dart.dart"); // TODO
  if (Dart_IsError(source)) return source;
  Dart_Handle library = Dart_LoadLibrary(Dart_NewString("dart:apache"), source, Dart_Null());
  if (Dart_IsError(library)) return library;
  Dart_Handle requestNative = Dart_CreateNativeWrapperClass(library, Dart_NewString("RequestNative"), 1);
  if (Dart_IsError(requestNative)) return requestNative;
  Dart_SetNativeResolver(library, NativeResolver);
  return library;  
}

extern "C" Dart_Handle ApacheLibraryInit(request_rec *r) {
  Dart_Handle library = Dart_LookupLibrary(Dart_NewString("dart:apache"));
  if (Dart_IsError(library)) library = ApacheLibraryLoad();
  if (Dart_IsError(library)) return library;
  Dart_Handle request = Dart_Invoke(library, Dart_NewString("get:request"), 0, NULL);
  if (Dart_IsError(request)) return request;
  Dart_Handle result = Dart_SetNativeInstanceField(request, 0, (intptr_t) r);
  return Dart_IsError(result) ? result : Dart_Null();
}
