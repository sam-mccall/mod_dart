// Copyright 2012 Google Inc.
// Licensed under the Apache License, Version 2.0 (the "License")
// You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

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
extern const char *mod_dart_source;

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

static apr_table_t *get_table(Dart_Handle headers) {
  intptr_t tptr;
  Dart_GetNativeInstanceField(headers, 0, &tptr);
  if (!tptr) Throw("dart:core", "Exception", "headers.apr_table was NULL!");
  return (apr_table_t*) tptr;
}

static void ThrowIfError(int code, const char* name, request_rec *r) {
  if (code) {
    char buf[1024];
    apr_strerror(code, buf, 1024);
    Throw("dart:io", "StreamException", apr_psprintf(r->pool, "%s failed: %s", name, buf));    
  }
}

static void Apache_Response_Write(Dart_NativeArguments arguments) {
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

static void Apache_Response_WriteList(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  
  Dart_Handle list = Dart_GetNativeArgument(arguments, 1);
  Dart_Handle offHandle = Dart_GetNativeArgument(arguments, 2);
  Dart_Handle lenHandle = Dart_GetNativeArgument(arguments, 3);

  int64_t off, len;
  Dart_IntegerToInt64(offHandle, &off);
  Dart_IntegerToInt64(lenHandle, &len);

  uint8_t* ctext = (uint8_t*) malloc(len);
  Dart_Handle result = Dart_ListGetAsBytes(list, off, ctext, len);
  if (Dart_IsError(result)) {
    free(ctext);
    Dart_PropagateError(result);
  }

  apr_bucket_brigade *out = apr_brigade_create(r->pool, r->connection->bucket_alloc);
  APR_BRIGADE_INSERT_TAIL(out, apr_bucket_transient_create((const char*) ctext, len, out->bucket_alloc));
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

static void Apache_Request_GetHost(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewString(r->parsed_uri.hostname ? r->parsed_uri.hostname
    : r->hostname ? r->hostname
    : r->connection->local_host ? r->connection->local_host
    : r->server->server_hostname ? r->server->server_hostname
    : r->connection->local_ip ? r->connection->local_ip : "localhost"));
  Dart_ExitScope();
}

static void Apache_Request_GetPort(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewInteger(r->connection->local_addr->port));
  Dart_ExitScope();
}

static void Apache_Request_GetProtocolVersion(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewInteger(r->proto_num));
  Dart_ExitScope();
}

static void Apache_Request_GetMethod(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewString(r->method));
  Dart_ExitScope();
}

static void Apache_Request_GetPath(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewString(r->parsed_uri.path));
  Dart_ExitScope();
}

static void Apache_Request_GetQueryString(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, r->parsed_uri.query ? Dart_NewString(r->parsed_uri.query) : Dart_Null());
  Dart_ExitScope();
}

static void Apache_Request_GetUri(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_Handle empty = Dart_NewString("");
  Dart_Handle scheme = Dart_NewString(r->server->server_scheme ? r->server->server_scheme : "http");
  Dart_Handle userInfo = empty;
  Dart_Handle host = Dart_NewString(r->parsed_uri.hostname ? r->parsed_uri.hostname
    : r->hostname ? r->hostname
    : r->connection->local_host ? r->connection->local_host
    : r->server->server_hostname ? r->server->server_hostname
    : r->connection->local_ip ? r->connection->local_ip : "localhost");
  Dart_Handle port = r->parsed_uri.port_str ? Dart_NewInteger(r->parsed_uri.port) : Dart_NewInteger(0);
  Dart_Handle path = Dart_NewString(r->parsed_uri.path);
  Dart_Handle query = r->parsed_uri.query ? Dart_NewString(r->parsed_uri.query) : empty;
  Dart_Handle fragment = empty;

  Dart_Handle uri_library = Dart_LookupLibrary(Dart_NewString("dart:uri"));
  if (Dart_IsError(uri_library)) Dart_PropagateError(uri_library);
  Dart_Handle uri_class = Dart_GetClass(uri_library, Dart_NewString("Uri"));
  if (Dart_IsError(uri_class)) Dart_PropagateError(uri_class);

  Dart_Handle args[] = {scheme, userInfo, host, port, path, query, fragment};
  Dart_Handle uri = Dart_New(uri_class, Dart_Null(), 7, args);
  if (Dart_IsError(uri)) Dart_PropagateError(uri);
  Dart_SetReturnValue(arguments, Dart_ToString(uri));
  Dart_ExitScope();
}

static void Apache_Connection_IsKeepalive(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewBoolean(r->connection->keepalive != AP_CONN_CLOSE));
  Dart_ExitScope();
}

static void Apache_Connection_SetKeepalive(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  bool keepalive;
  Dart_BooleanValue(Dart_GetNativeArgument(arguments, 1), &keepalive);
  r->connection->keepalive = keepalive ? AP_CONN_KEEPALIVE : AP_CONN_CLOSE;
  Dart_ExitScope();
}

static void Apache_Response_GetStatusCode(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewInteger(r->status ? r->status : 200));
  Dart_ExitScope();
}

static void Apache_Response_SetStatusCode(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_Handle statusHandle = Dart_GetNativeArgument(arguments, 1);
  int64_t status;
  Dart_IntegerToInt64(statusHandle, &status);
  r->status = status;
  Dart_ExitScope();
}

static void Apache_Response_GetStatusLine(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, r->status_line ? Dart_NewString(r->status_line) : Dart_Null());
  Dart_ExitScope();
}

static void Apache_Response_SetStatusLine(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_Handle statusHandle = Dart_GetNativeArgument(arguments, 1);
  const char* status_line = NULL;
  if (Dart_IsNull(statusHandle)) {
    r->status_line = NULL;
  } else {
    Dart_StringToCString(statusHandle, &status_line);
    r->status_line = apr_pstrdup(r->pool, status_line);
  }
  Dart_ExitScope();
}

static void Apache_Response_SetContentType(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_Handle ctypeHandle = Dart_GetNativeArgument(arguments, 1);
  if (Dart_IsNull(ctypeHandle)) {
    r->content_type = "text/plain";
  } else {
    const char* ctype;
    Dart_StringToCString(ctypeHandle, &ctype);
    r->content_type = apr_pstrdup(r->pool, ctype);
  }
  Dart_ExitScope();
}

static void Apache_Response_SetContentLength(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_Handle lengthHandle = Dart_GetNativeArgument(arguments, 1);
  int64_t length = 0;
  if (!Dart_IsNull(lengthHandle)) Dart_IntegerToInt64(lengthHandle, &length);
  ap_set_content_length(r, length);
  Dart_ExitScope();
}

static void Apache_Response_GetContentLength(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, (r->clength > 0) ? Dart_NewInteger(r->clength) : Dart_Null());
  Dart_ExitScope();
}

static void Apache_Response_GetContentType(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetReturnValue(arguments, Dart_NewString(r->content_type));
  Dart_ExitScope();
}

static void Apache_Headers_Get(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  apr_table_t *t = get_table(Dart_GetNativeArgument(arguments, 0));

  Dart_Handle name = Dart_GetNativeArgument(arguments, 1);
  const char* cname;
  Dart_StringToCString(name, &cname);

  const char* result = apr_table_get(t, cname);
  Dart_SetReturnValue(arguments, result ? Dart_NewString(result) : Dart_Null());
  Dart_ExitScope();
}

static void Apache_Headers_Add(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  apr_table_t *t = get_table(Dart_GetNativeArgument(arguments, 0));

  Dart_Handle name = Dart_GetNativeArgument(arguments, 1);
  const char* cname;
  Dart_StringToCString(name, &cname);

  Dart_Handle value = Dart_GetNativeArgument(arguments, 2);
  const char* cvalue;
  Dart_StringToCString(value, &cvalue);

  apr_table_add(t, cname, cvalue);
  Dart_ExitScope();
}

static void Apache_Headers_Remove(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  apr_table_t *t = get_table(Dart_GetNativeArgument(arguments, 0));

  Dart_Handle name = Dart_GetNativeArgument(arguments, 1);
  const char* cname;
  Dart_StringToCString(name, &cname);

  apr_table_unset(t, cname);
  Dart_ExitScope();
}

struct iterate_state {
  Dart_Handle callback;
  Dart_Handle error;
};

static int iterate_callback(void *ctx, const char *key, const char *value) {
  Dart_EnterScope();
  Dart_Handle args[2] = {Dart_NewString(key), Dart_NewString(value)};
  struct iterate_state *state = (struct iterate_state*) ctx;
  Dart_Handle result = Dart_InvokeClosure(state->callback, 2, args);
  if (Dart_IsError(result)) {
    state->error = result;
    return 0; // Don't exit scope, we don't want to lose [result].
  }
  Dart_ExitScope();
  return 1;
}

static void Apache_Headers_Iterate(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  apr_table_t *t = get_table(Dart_GetNativeArgument(arguments, 0));
  struct iterate_state state = {Dart_GetNativeArgument(arguments, 1), Dart_Null()};
  if (!apr_table_do(iterate_callback, &state, t, NULL)) Dart_PropagateError(state.error);
  Dart_ExitScope();
}

static void Apache_Request_InitHeaders(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetNativeInstanceField(Dart_GetNativeArgument(arguments, 1), 0, (intptr_t) r->headers_in);
  Dart_ExitScope();  
}

static void Apache_Response_InitHeaders(Dart_NativeArguments arguments) {
  Dart_EnterScope();
  request_rec *r = get_request(Dart_GetNativeArgument(arguments, 0));
  Dart_SetNativeInstanceField(Dart_GetNativeArgument(arguments, 1), 0, (intptr_t) r->headers_out);
  Dart_ExitScope();  
}

static Dart_NativeFunction NativeResolver(Dart_Handle name, int args) {
  const char* cname;
  if (Dart_IsError(Dart_StringToCString(name, &cname))) return NULL; // not enough context to log!
  if (!strcmp(cname, "Apache_Connection_IsKeepalive") && (args == 1)) return Apache_Connection_IsKeepalive;
  if (!strcmp(cname, "Apache_Connection_SetKeepalive") && (args == 2)) return Apache_Connection_SetKeepalive;
  if (!strcmp(cname, "Apache_Response_Write") && (args == 2)) return Apache_Response_Write;
  if (!strcmp(cname, "Apache_Response_WriteList") && (args == 4)) return Apache_Response_WriteList;
  if (!strcmp(cname, "Apache_Request_Flush") && (args == 1)) return Apache_Request_Flush;
  if (!strcmp(cname, "Apache_Request_InitHeaders") && (args == 2)) return Apache_Request_InitHeaders;
  if (!strcmp(cname, "Apache_Request_GetHost") && (args == 1)) return Apache_Request_GetHost;
  if (!strcmp(cname, "Apache_Request_GetPort") && (args == 1)) return Apache_Request_GetPort;
  if (!strcmp(cname, "Apache_Request_GetProtocolVersion") && (args == 1)) return Apache_Request_GetProtocolVersion;
  if (!strcmp(cname, "Apache_Request_GetMethod") && (args == 1)) return Apache_Request_GetMethod;
  if (!strcmp(cname, "Apache_Request_GetPath") && (args == 1)) return Apache_Request_GetPath;
  if (!strcmp(cname, "Apache_Request_GetQueryString") && (args == 1)) return Apache_Request_GetQueryString;
  if (!strcmp(cname, "Apache_Request_GetUri") && (args == 1)) return Apache_Request_GetUri;
  if (!strcmp(cname, "Apache_Response_GetStatusCode") && (args == 1)) return Apache_Response_GetStatusCode;
  if (!strcmp(cname, "Apache_Response_SetStatusCode") && (args == 2)) return Apache_Response_SetStatusCode;
  if (!strcmp(cname, "Apache_Response_GetStatusLine") && (args == 1)) return Apache_Response_GetStatusLine;
  if (!strcmp(cname, "Apache_Response_SetStatusLine") && (args == 2)) return Apache_Response_SetStatusLine;
  if (!strcmp(cname, "Apache_Response_GetContentType") && (args == 1)) return Apache_Response_GetContentType;
  if (!strcmp(cname, "Apache_Response_SetContentType") && (args == 2)) return Apache_Response_SetContentType;
  if (!strcmp(cname, "Apache_Response_GetContentLength") && (args == 1)) return Apache_Response_GetContentLength;
  if (!strcmp(cname, "Apache_Response_SetContentLength") && (args == 2)) return Apache_Response_SetContentLength;
  if (!strcmp(cname, "Apache_Response_InitHeaders") && (args == 2)) return Apache_Response_InitHeaders;
  if (!strcmp(cname, "Apache_Headers_Get") && (args == 2)) return Apache_Headers_Get;
  if (!strcmp(cname, "Apache_Headers_Add") && (args == 3)) return Apache_Headers_Add;
  if (!strcmp(cname, "Apache_Headers_Iterate") && (args == 2)) return Apache_Headers_Iterate;
  if (!strcmp(cname, "Apache_Headers_Remove") && (args == 2)) return Apache_Headers_Remove;
  return NULL;
}

extern "C" Dart_Handle ApacheLibraryLoad() {
  Dart_Handle library = Dart_LoadLibrary(Dart_NewString("dart:apache"), Dart_NewString(mod_dart_source));
  if (Dart_IsError(library)) return library;

  Dart_Handle wrapper = Dart_CreateNativeWrapperClass(library, Dart_NewString("RequestNative"), 1);
  if (Dart_IsError(wrapper)) return wrapper;
  wrapper = Dart_CreateNativeWrapperClass(library, Dart_NewString("HeadersNative"), 1);
  if (Dart_IsError(wrapper)) return wrapper;

  return library;  
}

extern "C" Dart_Handle ApacheLibraryInit(request_rec *r) {
  Dart_Handle library = Dart_LookupLibrary(Dart_NewString("dart:apache"));
  if (Dart_IsError(library)) return library;
  Dart_Handle result = Dart_SetNativeResolver(library, NativeResolver);
  if (Dart_IsError(result)) return result;
  Dart_Handle request = Dart_Invoke(library, Dart_NewString("get:request"), 0, NULL);
  if (Dart_IsError(request)) return request;
  result = Dart_SetNativeInstanceField(request, 0, (intptr_t) r);
  r->content_type = "text/plain";
  return Dart_IsError(result) ? result : Dart_Null();
}
