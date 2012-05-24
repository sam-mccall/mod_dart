#library('apache');

#import('dart:io');

var _request;
HttpRequest get request() {
  if (_request == null) _request = new _Request();  
  return _request;
}
HttpResponse get response() => request._response;

class _Request extends RequestNative implements HttpRequest {
  _Response _response;
  _Headers _headers;
  _Request() {
    _response = new _Response(this);
  }

  _write(s) native 'Apache_Request_Write';
  _flush() native 'Apache_Request_Flush';
  get _responseStatusCode() native 'Apache_Response_GetStatusCode';
  set _responseStatusCode(value) native 'Apache_Response_SetStatusCode';
  _initRequestHeaders(headers) native 'Apache_Request_InitHeaders';
  _initResponseHeaders(headers) native 'Apache_Response_InitHeaders';

  get headers() {
    if (_headers == null) {
      _headers = new _RequestHeaders();
      _initRequestHeaders(_headers);
    }
    return _headers;
  }
}

class _Response {
  final Request _request;
  OutputStream _outputStream;
  _Headers _headers;
  _Response(this._request) {
    _outputStream = new _OutputStream(_request);
  }

  get outputStream() => _outputStream;
  get headers() {
    if (_headers == null) {
      _headers = new _ResponseHeaders(_request);
      _request._initResponseHeaders(_headers);
    }
    return _headers;
  }

  int get contentLength() => -1;
  void set contentLength(int value) => null; // Ignored for now

  DetachedSocket detachSocket() {
    throw new NotImplementedException();
  }

  bool get persistentConnection() => true;
  void set persistentConnection(bool value) {
    throw new NotImplementedException();
  }

  String get reasonPhrase() => null;
  void set reasonPhrase(String value) {
    throw new NotImplementedException();
  }

  int get statusCode() => _request._responseStatusCode;
  void set statusCode(int value) {
    if (value is! int) throw new IllegalArgumentException("Status code must be an integer");
    _request._responseStatusCode = value;
  }
}

class _OutputStream implements OutputStream {
  final _Request _request;
  _OutputStream(this._request);

  bool writeString(String string, [Encoding encoding = Encoding.UTF_8]) {
    if (encoding != Encoding.UTF_8) throw new StreamException("Only UTF_8 is supported");
    _request._write(string.toString());
  }
}

class _Headers extends HeadersNative implements HttpHeaders {
  final _Request _request;
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
  String get host() => _getHost(_request);
  int get port() {
    var host = value('host');
    if (host != null && host.contains(':')) return Math.parseInt(host.substring(host.indexOf(':') + 1));
    return _getPort(request);
  }
  static _getHost(request) native 'Apache_Request_GetHost';
  static _getPort(request) native 'Apache_Request_GetPort';
}

class _ResponseHeaders extends _Headers {
  _ResponseHeaders(request) : super(request);

  ContentType get contentType() => new ContentType.fromString(_getContentType(_request));
  void set contentType(ContentType type) => _setContentType(_request, type.toString());
  void add(String name, String value) {
    if (name.toLowerCase() == "content-type") {
      _setContentType(_request, value.toString());
      return;
    }
    super.add(name, value);
  }
  void removeAll(String name) {
    if (name.toLowerCase() == "content-type") {
      _setContentType(_request, null);
      return;
    }
    super.removeAll(name);
  }

  void forEach(void f(String name, List<String> values)) {
    f(name, [contentType.toString()]);
    super.forEach((name, values) {
      if (name.toLowerCase() != "content-type") f(name, values);
    });
  }

  static _getContentType(_request) native 'Apache_Response_GetContentType';
  static _setContentType(_request, ctype) native 'Apache_Response_SetContentType';
}
