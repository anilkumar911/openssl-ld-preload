sudo docker run \
	-it \
	--rm \
	--ipc=host \
	--name t4openssl \
	--net=container:protect-client-pod \
	-v $PWD:/workspace/t4openssl/ \
  -v /home/axolotl/git/t4/t4_openssl_111k/t4ssl/:/t4ssl/ \
  -v /home/axolotl/git/t4/t4_openssl_111k/deps/artifacts/:/deps/ \
	-v /home/axolotl/git/openssl:/openssl \
	t4openssl-bld-interactive bash
