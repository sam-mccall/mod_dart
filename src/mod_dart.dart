// Copyright 2012 Google Inc.
// Licensed under the Apache License, Version 2.0 (the "License")
// You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

#library('apache');
#import('dart:io');
#import('dart:uri');

void print(text) => response.outputStream.writeString("$text\n");

var _request;
HttpRequest get request() {
  if (_request == null) _request = new _Request();  
  return _request;
}
HttpResponse get response() => request._response;

class _Request extends RequestNative implements HttpRequest {
  _Response _response;
  _Headers _headers;
  Map<String, String> _queryParameters;
  _Request() {
    _response = new _Response(this);
  }

  _write(s) native 'Apache_Response_Write';
  _writeList(list, off, len) native 'Apache_Response_WriteList';
  _flush() native 'Apache_Request_Flush';
  get _responseStatusCode() native 'Apache_Response_GetStatusCode';
  set _responseStatusCode(value) native 'Apache_Response_SetStatusCode';
  _getResponseStatusLine() native 'Apache_Response_GetStatusLine';
  _setResponseStatusLine(value) native 'Apache_Response_SetStatusLine';
  _initRequestHeaders(headers) native 'Apache_Request_InitHeaders';
  _initResponseHeaders(headers) native 'Apache_Response_InitHeaders';
  _getHost() native 'Apache_Request_GetHost';
  _getPort() native 'Apache_Request_GetPort';
  _getResponseContentType() native 'Apache_Response_GetContentType';
  _setResponseContentType(ctype) native 'Apache_Response_SetContentType';
  _getResponseContentLength() native 'Apache_Response_GetContentLength';
  _setResponseContentLength(length) native 'Apache_Response_SetContentLength';
  _setKeepalive(keep) native 'Apache_Connection_SetKeepalive';
  _getProtocolVersion() native 'Apache_Request_GetProtocolVersion';

  HttpHeaders get headers() {
    if (_headers == null) {
      _headers = new _RequestHeaders();
      _initRequestHeaders(_headers);
    }
    return _headers;
  }

  bool get persistentConnection() native 'Apache_Connection_IsKeepalive';
  String get uri() native 'Apache_Request_GetUri';
  String get queryString() native 'Apache_Request_GetQueryString';

  static _decode(queryPart) => decodeUriComponent(queryPart.replaceAll('+',' '));
  Map<String, String> get queryParameters() {
    if (_queryParameters == null) {
      var text = queryString;
      _queryParameters = new Map<String, String>();
      if (text != null) text.split("&").forEach((token) {
        if (token.isEmpty()) return;
        var index = token.indexOf('=');
        if (index < 0) {
          _queryParameters[_decode(token)] = null;
        } else {
          _queryParameters[_decode(token.substring(0, index))] = _decode(token.substring(index+1));
        }
      });
    }
    return _queryParameters;
  }
  String get path() native 'Apache_Request_GetPath';
  String get method() native 'Apache_Request_GetMethod';
  String get protocolVersion() {
    var num = _getProtocolVersion();
    return "${(num/1000).toInt()}.${num%1000}";
  }
  int get contentLength() {
    var lengthText = headers.value('Content-Length');
    return (lengthText == null) ? -1 : Math.parseInt(lengthText);
  }
  InputStream get inputStream() => null;
}

class _Response {
  final _request;
  final _outputStream;
  var _reasonPhrase;
  _Headers _headers;
  _Response(request) : _request = request, _outputStream = new _ResponseOutputStream(request);

  get outputStream() => _outputStream;
  get headers() {
    if (_headers == null) {
      _headers = new _ResponseHeaders(_request);
      _request._initResponseHeaders(_headers);
    }
    return _headers;
  }

  int get contentLength() {
    var result = _request._getResponseContentLength();
    return (result == null) ? -1 : result;
  }
  void set contentLength(int value) {
    if (value is! int) throw new IllegalArgumentException();
    _request._setResponseContentLength((value == -1) ? null : value);
  }

  DetachedSocket detachSocket() {
    throw new NotImplementedException();
  }

  bool get persistentConnection() => _request.persistentConnection;
  void set persistentConnection(bool value) => _request._setKeepalive(value == true);

  String get reasonPhrase() => _reasonPhrase;
  void set reasonPhrase(String value) {
    _reasonPhrase = value;
    _request._setResponseStatusLine("$statusCode $value");
  }

  int get statusCode() => _request._responseStatusCode;
  void set statusCode(int value) {
    if (value is! int) throw new IllegalArgumentException("Status code must be an integer");
    _request._responseStatusCode = value;
    if (_reasonPhrase) _request._setResponseStatusLine("$value $_reasonPhrase");
  }
}

class _ResponseOutputStream implements OutputStream {
  final _request;
  _ResponseOutputStream(this._request);

  bool writeString(String string, [Encoding encoding = Encoding.UTF_8]) {
    if (encoding != Encoding.UTF_8) throw new StreamException("Only UTF_8 is supported");
    _request._write(string.toString());
    return true;
  }

  flush() => _request._flush();

  bool write(List<int> buffer, [bool copyBuffer = true]) => writeFrom(buffer, 0, buffer.length);

  bool writeFrom(List<int> buffer, [int offset = 0, int len]) {
    if (len != null) len = buffer.length - offset;
    if ((offset < 0) || (len < 0) || (offset + len > buffer.length)) {
      throw new IllegalArgumentException("Out of range: offset=$offset len=$len buffer.length=${buffer.length}");
    }
    _request._writeList(buffer, offset, len);
    return true;
  }

  void close() => null; // TODO
  void destroy() => null; // TODO
  void set onClosed(void callback()) => null; // TODO
  void set onError(void callback(e)) => null; // TODO
  void set onNoPendingWrites(void callback()) => null; // TODO
}

class _Headers extends HeadersNative implements HttpHeaders {
  final _request;
  _Headers(this._request);

  List<String> operator [](String name) {
    name = name.toLowerCase();
    var result = [];
    _forEach((k,v) {
      if (k.toLowerCase() == name) result.add(v);
    });
    return result;
  }

  void add(String name, Object value) {
    if (value is List) {
      value.forEach((v) => add(name, _format(v)));
      return;
    }
    _add(name, _format(value));
  }
  void set(String name, Object value) {
    removeAll(name);
    add(name, value);
  }
  void remove(String name, Object value) {
    value = _format(value);
    newValues = this[name];
    var index;
    while ((index = newValues.indexOf(value)) >= 0) newValues.removeRange(index, 1);
    if (index >= 0) {
      removeAll(name);
      add(name, newValues);
    }
  }
  void removeAll(String name) {
    _removeAll(name.toString());
  }
  String _format(Object value) {
    if (value is Date) return _formatDate(value.toUtc());
    return value.toString();
  }
  String _formatDate(Date date) => date.toString(); // TODO
  Date _parseDate(String date) { throw new NotImplementedException(); }

  Date get date() {
    var text = value("Date");
    return (text == null) ? null : _parseDate(text);
  }
  void set date(Date value) => set("Date", _formatDate(value));

  Date get expires() {
    var text = value("Date");
    return (text == null) ? null : _parseDate(text);
  }
  void set expires(Date value) => set("Expires", _formatDate(value));

  ContentType get contentType() {
    var text = value("content-type");
    return (text == null) ? null : new ContentType.fromString(text);
  }

  void set contentType(ContentType ctype) => set("Content-Type", ctype.toString());

  void forEach(void f(String name, List<String> values)) {
    var nameValueMap = {};
    var names = [];
    _forEach((k,v) {
      var klower = k.toLowerCase();
      if (!nameValueMap.containsKey(klower)) {
        names.add(k);
        nameValueMap[klower] = [];
      }
      nameValueMap[klower].add(v);
    });
    names.forEach((name) {
      f(name, nameValueMap[name.toLowerCase()]);
    });
  }

  String value(String name) {
    var result = this[name.toString()];
    if (result.length == 0) return null;
    if (result.length > 1) throw new Exception("Multiple values for header $name");
    return result[0];
  }

  String get host() => null;
  String get port() => null;

  _get(name) native 'Apache_Headers_Get';
  _add(name, value) native 'Apache_Headers_Add';
  _forEach(void f(name, value)) native 'Apache_Headers_Iterate';
  _removeAll(name) native 'Apache_Headers_Remove';
}

class _RequestHeaders extends _Headers {
  String get host() => _request._getHost();
  int get port() {
    var host = value('host');
    if (host != null && host.contains(':')) return Math.parseInt(host.substring(host.indexOf(':') + 1));
    return _request._getPort();
  }
}

class _ResponseHeaders extends _Headers {
  _ResponseHeaders(request) : super(request);

  ContentType get contentType() => new ContentType.fromString(_request._getResponseContentType());
  void set contentType(ContentType type) => _request._setResponseContentType(type.toString());
  void add(String name, String value) {
    if (name.toLowerCase() == "content-type") {
      _setContentType(_request, value.toString());
      return;
    }
    if (name.toLowerCase() == "content-length") {
      _setContentType(_request, Math.parseInt(value.toString()));
      return;
    }
    super.add(name, value);
  }
  void removeAll(String name) {
    if (name.toLowerCase() == "content-type") {
      _setContentType(_request, null);
      return;
    }
    if (name.toLowerCase() == "content-length") {
      _setContentLength(_request, null);
      return;
    }
    super.removeAll(name);
  }

  void forEach(void f(String name, List<String> values)) {
    f("Content-Type", [contentType.toString()]);
    var length = _request._response.contentLength;
    if (length >= 0) f("Content-Length", [length.toString()]);
    super.forEach((name, values) {
      var namelower = name.toLowerCase();
      if ((namelower != "content-type") && (namelower != "content-length")) f(name, values);
    });
  }
}
