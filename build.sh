#!/bin/bash
set -e
git clone https://github.com/warmcat/libwebsockets.git
cd libwebsockets
mkdir build
cd build
cmake .. -DLWS_WITH_JOSE=1
sudo make install
cd ../..
mkdir build
cd build
cmake ..
make -j8
