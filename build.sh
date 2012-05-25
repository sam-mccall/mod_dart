#!/bin/bash

. build.config

type "$APXS" 2>/dev/null || APXS="apxs2"
type "$APXS" 2>/dev/null || APXS="apxs"
type "$APXS" || exit "Couldn't find APXS, edit build.config"

type "$HTTPD" 2>/dev/null || HTTPD="httpd"
type "$HTTPD" 2>/dev/null || HTTPD="apache2"
type "$HTTPD" || exit "Couldn't find httpd, edit build.config"

UNAME=`uname`
if [[ "$UNAME" == "Darwin" ]]; then
  DART_LIB=$DART_SRC/runtime/xcodebuild/${DART_MODE}_$DART_ARCH
  DART_GEN=$DART_SRC/runtime/xcodebuild/DerivedSources/${DART_MODE}_$DART_ARCH
  LIBRARY_GROUP_START= # not needed - mac linker 'just works' with circular dependencies
  LIBRARY_GROUP_END=
else
  if [[ "$UNAME" != "Linux" ]]; then
    echo "Warning: Unrecognized OS $UNAME, this likely won't work"
  fi
  DART_LIB=$DART_SRC/runtime/out/${DART_MODE}_$DART_ARCH/obj.target/runtime
  DART_GEN=$DART_SRC/runtime/out/${DART_MODE}_$DART_ARCH/obj/gen
  LIBRARY_GROUP_START=,--start-group # dart static libraries have circular references
  LIBRARY_GROUP_END=,--end-group
fi

if [[ "$DART_MODE" == "Debug" ]]; then
  COPTS="-DDEBUG"
else
  COPTS="-DNDEBUG"
fi

rm src/mod_dart_gen.c; python $DART_SRC/runtime/tools/create_string_literal.py --output src/mod_dart_gen.c --include 'none' --input_cc src/mod_dart_gen.c.tmpl --var_name "mod_dart_source" src/mod_dart.dart
$APXS -S CC=g++ -c $COPTS -o mod_dart.so -Wc,-Wall -Wc,-Werror -I $DART_SRC/runtime -lstdc++ -I $DART_GEN \
-Wl,-Wl$LIBRARY_GROUP_START,$DART_LIB/libdart_export.a,$DART_LIB/libdart_builtin.a,$DART_LIB/libdart_lib_withcore.a,$DART_LIB/libdart_builtin.a,$DART_LIB/libdart_vm.a,$DART_LIB/libjscre.a,$DART_LIB/libdouble_conversion.a$LIBRARY_GROUP_END \
src/builtin.c src/mod_dart_gen.c src/apache_library.c src/mod_dart.c && \
sudo $APXS -i -a -n dart mod_dart.la && \
sudo apachectl restart
