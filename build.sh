gcc -g sslkeylog.c -shared -o libsslkeylog.so -fPIC -ldl

#SSLKEYLOGFILE=premaster.txt LD_PRELOAD=./libsslkeylog.so openssl ...