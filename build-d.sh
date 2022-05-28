
#OPENSSL_DIR= /home/axolotl/git/t4/t4_openssl_111k/openssl/
#T4SSL_DIR=/home/axolotl/git/t4/t4_openssl_111k/t4ssl/
#T4SSL_LIB_DIR=/home/axolotl/git/t4/t4_openssl_111k/t4ssl/lib

OPENSSL_DIR=/openssl/
T4SSL_DIR=/t4ssl/
T4SSL_LIB_DIR=${T4SSL_DIR}/lib

#gcc -g sslkeylog.c -shared -o libsslkeylog.so -fPIC -ldl -I ${T4SSL_DIR} -L ${T4SSL_LIB_DIR} -l t4ssl -I ${OPENSSL_DIR} -I ${OPENSSL_DIR}/include
gcc -g sslkeylog.c -shared -o libsslkeylog.so -fPIC -ldl -I ${T4SSL_DIR} -L ${T4SSL_LIB_DIR} -l t4ssl -I ${OPENSSL_DIR} -I ${OPENSSL_DIR}/include -L /deps/libconfuse/lib/ -L /deps/sodium/lib -L /deps/jansson/lib -ljansson -lsodium -lconfuse

#SSLKEYLOGFILE=premaster.txt LD_PRELOAD=./libsslkeylog.so openssl ...
