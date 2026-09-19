#!/bin/sh
# Builds the host test tools with the device's raster/renderer sources.
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --show-sdk-path)
CXX="clang++ -std=c++17 -O2 -isysroot $SDK -isystem $SDK/usr/include/c++/v1 -I."
$CXX render_scene.cpp ../../maps/raster.cpp -o render_scene
$CXX render_tile.cpp ../../maps/raster.cpp ../../maps/renderer.cpp -o render_tile
echo built render_scene render_tile
