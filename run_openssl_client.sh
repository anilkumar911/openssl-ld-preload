export LD_LIBRARY_PATH=/openssl/:/workspace/t4openssl:/t4ssl/lib:/deps/jansson/lib:/deps/libconfuse/lib/:/deps/sodium/lib/
export LD_PRELOAD=$PWD/libsslkeylog.so

/openssl/apps/openssl s_client -connect protect-server-pod:4433 -msg
