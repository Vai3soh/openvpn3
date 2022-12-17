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

path_client="./build-x86_64/client"
client_dir="./build/windows/client/"
mkdir -p ${client_dir}
container="builder_ovpncli_python_cross"

EMPT=$(docker images -q win1:latest);
if [ -n ${EMPT} ]; then
	docker build -f dockerfiles/Dockerfile.win -t win1 .
fi  

docker build -f dockerfiles/Dockerfile.stage2_win -t win2 .
docker run -itd --name ${container} win2 bash

docker cp ${container}:${path_client} ${client_dir}
docker cp ${container}:/build_windows_deps.tar.gz ${client_dir}
docker cp ${container}:/build-x86_64/test/ovpncli/ovpncli.exe build/windows/ovpncli.exe
docker cp ${container}:/build-x86_64/test/ovpncli/ovpncliagent.exe build/windows/ovpncliagent.exe

rm -rf ${client_dir}/client/{CMakeFiles,cmake_install.cmake,Makefile}

mv ${client_dir}/client/libopenvpn3_x86_64.a \
${client_dir}/client/libopenvpn3_Windows_x86_64.a 

rsync -arctuxz --remove-source-files ${client_dir}/client/* ${client_dir}
rm -rf ${client_dir}/client/

cat > ${client_dir}/Readme << 'EOF'
Add to string ovpncli.go:

#cgo CFLAGS: -I${SRCDIR}
#cgo windows LDFLAGS: -L${SRCDIR} -L${SRCDIR}/deps_win/deps-x86_64/lib/
#cgo windows CXXFLAGS: -DASIO_DISABLE_LOCAL_SOCKETS -DASIO_STANDALONE -DHAVE_LZ4 -DLZ4_DISABLE_DEPRECATE_WARNINGS -DMBEDTLS_DEPRECATED_REMOVED -DTAP_WIN_COMPONENT_ID=tap0901 -DUSE_ASIO -DUSE_OPENSSL -D_CRT_SECURE_NO_WARNINGS -I${SRCDIR}/deps_win/deps-x86_64/includes/asio -O3 -Wa,-mbig-obj -Wall -Wsign-compare -std=gnu++14
#cgo windows LDFLAGS: -lopenvpn3_Windows_x86_64 -lssl -lcrypto -llz4 -lws2_32 -lrpcrt4 -liphlpapi -lsetupapi -lwininet -lole32 -lfwpuclnt -lwtsapi32 -luuid

build binary:
GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++-posix go build -x . 

Copy files:
ovpncli.go
ovpncli_wrap.cxx
ovpncli_wrap.h
libopenvpn3_Windows_x86_64.a
build_windows_deps.tar.gz

Unpack build_windows_deps.tar.gz

Optional add to ovpncli_wrap.cxx:

#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

EOF
