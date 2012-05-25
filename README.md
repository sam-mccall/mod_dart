mod_dart: server-side Dart apps in Apache
=========================================

Highly experimental, probably broken and insecure - proceed with caution :-)

# Sample use

httpd.conf

    <Location />
      AddHandler dart .dart
      DartDebug on
    </Location>

helloworld.dart

    #import('dart:apache');
    #import('dart:io');

    main() {
      response.headers.contentType = new ContentType.fromString("text/html");
      response.outputStream.writeString("<h1>Hello, dart!</h1>\n");
      print("<dl>"); // shorthand to write a line to response.outputStream
      request.headers.forEach((name, values) {
        print("<dt>$name</dt>");
        values.forEach((v) => print("<dd>$v</dd>"));
      });
    }

# APIs provided

`dart:apache` provides top level constants, and a convenience function:

    final HttpRequest request;
    final HttpResponse response;
    void print(s) => response.outputStream.writeString(s.toString());

`response` is mostly complete, including `outputStream` and `headers`, see the 
[dart:io documentation](http://api.dartlang.org/io/HttpRequest.html) for details.

`request` is complete, except `inputStream` is not yet implemented.

Date formatting and parsing in `HttpHeaders` is not implemented.

Each request is handled in its own isolate, spawning further isolates is untested and probably doesn't work.

# Apache directives

  * `AddHandler dart .dart`
    * Tells Apache to process *.dart files with mod_dart
  * `DartDebug On`
    * Exceptions and syntax errors will be sent to the browser in addition to the apache error log
    * The X-Dart-Snapshot header will be set, indicating whether the script was loaded from a VM snapshot
  * `DartSnapshot /path/to/script.dart`
    * The script will be loaded at startup and snapshotted, so it doesn't need to be parsed for every page load
    * If the snapshot is stale (older than the script's mtime), it will not be used
  * `DartSnapshotForever /path/to/script.dart`
    * Same as `DartSnapshot`, but doesn't check if the snapshot is stale (and thus avoids one `stat()`)

# Building and installing

You'll need:
  * Mac or Linux
  * g++ toolchain
  * python
  * Apache2, installed and configured
  * The apache2-dev bits (apxs and the headers)
  * Dart sources, built for the same arch as apache (x64 or ia32)

Edit build.config, and at least set `DART_SRC`.

APXS and Apache will be automatically located if they're on your PATH with common names (apxs/apxs2, apache2/httpd). Otherwise set the appropriate variables.

`./build.sh` will build the library, install it, and restart apache.

