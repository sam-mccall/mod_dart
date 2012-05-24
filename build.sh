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
  DART_LIB=$DART_SRC/runtime/xcodebuild/Debug_$DART_ARCH
  DART_GEN=$DART_SRC/runtime/xcodebuild/DerivedSources/Debug_$DART_ARCH
  LIBRARY_GROUP_START= # not needed - mac linker 'just works' with circular dependencies
  LIBRARY_GROUP_END=
else
  if [[ "$UNAME" != "Linux" ]]; then
    echo "Warning: Unrecognized OS $UNAME, this likely won't work"
  fi
  DART_LIB=$DART_SRC/runtime/out/Debug_$DART_ARCH/obj.target/runtime
  DART_GEN=$DART_SRC/runtime/out/Debug_$DART_ARCH/obj/gen
  LIBRARY_GROUP_START=,--start-group # dart static libraries have circular references
  LIBRARY_GROUP_END=,--end-group
fi


rm src/snapshot_gen.c; ln -s $DART_GEN/snapshot_gen.cc src/snapshot_gen.c
rm src/mod_dart_gen.c; python $DART_SRC/runtime/tools/create_string_literal.py --output src/mod_dart_gen.c --include 'none' --input_cc src/mod_dart_gen.c.tmpl --var_name "mod_dart_source" src/mod_dart.dart
$APXS -S CC=g++ -c -o mod_dart.so -Wc,-Wall -Wc,-Werror -I $DART_SRC/runtime -lstdc++ \
-Wl,-Wl$LIBRARY_GROUP_START,$DART_LIB/libdart_export.a,$DART_LIB/libdart_lib_withcore.a,$DART_LIB/libdart_builtin.a,$DART_LIB/libdart_vm.a,$DART_LIB/libjscre.a,$DART_LIB/libdouble_conversion.a$LIBRARY_GROUP_END \
src/snapshot_gen.c src/mod_dart_gen.c src/apache_library.c src/mod_dart.c && \
sudo $APXS -i -a -n dart mod_dart.la && \
sudo APACHE_RUN_USER=`id -un` APACHE_RUN_GROUP=`id -gn` $HTTPD -X

#,dart_vm,dart_lib_withcore,jscre

# TOPOSORT: [libdart, libdart_lib, libdart_withcore]