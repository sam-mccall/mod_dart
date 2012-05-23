#!/bin/bash

. build.config

rm src/snapshot_gen.c; ln -s $DART_GEN/snapshot_gen.cc src/snapshot_gen.c
$APXS -S CC=g++ -c -o mod_dart.so -Wc,-Wall -Wc,-Werror -I $DART_SRC/runtime -lstdc++ \
-Wl,-Wl,--start-group,$DART_LIB/libdart_export.a,$DART_LIB/libdart_lib_withcore.a,$DART_LIB/libdart_builtin.a,$DART_LIB/libdart_vm.a,$DART_LIB/libjscre.a,$DART_LIB/libdouble_conversion.a,--end-group \
src/snapshot_gen.c src/mod_dart.c&& \
sudo $APXS -i -a -n dart mod_dart.la && \
sudo APACHE_RUN_USER=`id -un` APACHE_RUN_GROUP=`id -gn` apache2 -X

#,dart_vm,dart_lib_withcore,jscre

# TOPOSORT: [libdart, libdart_lib, libdart_withcore]