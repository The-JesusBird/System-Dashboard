#!/usr/bin/env bash

set -e

echo "=========================================="
echo " Building Fluent System Control..."
echo "=========================================="

# Kill running instance to avoid write permission errors
taskkill //IM SystemDashboard.exe //F 2>/dev/null || true

# 1. Compile the resource file containing your icon
windres resource.rc -o resource.o

# 2. Compile C++ code and link resource.o
g++ -std=c++20 -O2 -Wall -Wno-array-bounds -mwindows \
    -static -static-libgcc -static-libstdc++ \
    -Iimgui -Iimgui/backends -Isrc \
    src/main.cpp \
    resource.o \
    imgui/imgui.cpp \
    imgui/imgui_draw.cpp \
    imgui/imgui_tables.cpp \
    imgui/imgui_widgets.cpp \
    imgui/backends/imgui_impl_win32.cpp \
    imgui/backends/imgui_impl_dx11.cpp \
    -o SystemDashboard.exe \
    -ld3d11 -lpropsys -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -lsetupapi -lole32 -loleaut32 -lpsapi -lpdh

if [ -f "SystemDashboard.exe" ]; then
    echo "=========================================="
    echo " SUCCESS! Built SystemDashboard.exe"
    echo "=========================================="
fi