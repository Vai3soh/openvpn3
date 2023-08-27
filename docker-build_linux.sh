#!/bin/env bash
 
set -eux
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd ${SCRIPT_DIR} || exit
REQUIRED_PKG="rsync"
PKG_OK=$(dpkg-query -W --showformat='${Status}\n' $REQUIRED_PKG|grep "install ok installed")
echo Checking for $REQUIRED_PKG: $PKG_OK
if [ "" = "$PKG_OK" ]; then
  echo "No $REQUIRED_PKG. Setting up $REQUIRED_PKG."
fi

path_client="/ovpn3/core/build/client"
container_name=builder_ovpncli_linux
client_dir="./build/linux/client/"
mkdir -p ${client_dir}

docker build -f dockerfiles/Dockerfile.debian -t linux_builder .
docker run -itd --name ${container_name} linux_builder bash

docker cp ${container_name}:${path_client} ${client_dir}
docker cp ${container_name}:/ovpn3/core/build/test/ovpncli/ovpncli ./build/linux/
docker cp ${container_name}:/build_linux_deps.tar.gz ${client_dir}

mv ${client_dir}/client/libopenvpn3_x86_64.a \
${client_dir}/client/libopenvpn3_Linux_x86_64.a 
rm -rf ${client_dir}/client/{CMakeFiles,cmake_install.cmake,Makefile}

rsync -arctuxz --remove-source-files ${client_dir}/client/* ${client_dir}
rm -rf ${client_dir}/client/

docker rm -f ${container_name}

cat > ${client_dir}/Readme << 'EOF'
Add to string ovpncli.go:

#cgo CFLAGS: -I${SRCDIR}
#cgo linux LDFLAGS: -L${SRCDIR} -L${SRCDIR}/deps/libs/
#cgo linux CXXFLAGS: -DASIO_STANDALONE -DHAVE_LZ4 -DLZ4_DISABLE_DEPRECATE_WARNINGS -DMBEDTLS_DEPRECATED_REMOVED -DUSE_ASIO -DUSE_OPENSSL -I${SRCDIR}/deps/asio -Wall -Wsign-compare -std=gnu++14
#cgo linux LDFLAGS: -lopenvpn3_Linux_x86_64 -lssl -lcrypto -llz4 -lpthread

build binary:
go build -x . 

Copy files:
ovpncli.go
ovpncli_wrap.cxx
ovpncli_wrap.h
libopenvpn3_Linux_x86_64.a
build_linux_deps.tar.gz

Unpack build_linux_deps.tar.gz

Optional add to ovpncli_wrap.cxx:

#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

EOF
