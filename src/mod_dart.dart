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
  _Request() {
    _response = new _Response(this);
  }

  _write(s) native 'Apache_Request_Write';
  _flush() native 'Apache_Request_Flush';
}

class _Response {
  final Request _request;
  OutputStream _outputStream;
  _Response(this._request) {
    _outputStream = new _OutputStream(_request);
  }

  get outputStream() => _outputStream;
}

class _OutputStream implements OutputStream {
  final _Request _request;
  _OutputStream(this._request);

  bool writeString(String string, [Encoding encoding = Encoding.UTF_8]) {
    if (encoding != Encoding.UTF_8) throw new StreamException("Only UTF_8 is supported");
    _request._write(string);
  }
}
