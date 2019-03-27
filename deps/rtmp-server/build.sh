#!/bin/sh

path=$1

echo "Build rtmp-server sdk"
cd ${path}/sdk
make clean
make RELEASE=1 -j $(nproc)

echo "Build rtmp-server media-server"
cd ${path}/media-server
make clean
make RELEASE=1 -j $(nproc)

echo "Copy libs"
cd ${path}/
mkdir -p ../../build/deps/rtmp-server/

cp media-server/librtmp/release.linux/librtmp.a ../../build/deps/rtmp-server/
cp media-server/libflv/release.linux/libflv.a ../../build/deps/rtmp-server/

ls ../../build/deps/rtmp-server/libflv.a
ls ../../build/deps/rtmp-server/librtmp.a