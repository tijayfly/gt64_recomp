#!/usr/bin/env bash
# Build a Linux release in an Ubuntu 22.04 container (glibc 2.35) and package it.
#   scripts/release/build_linux.sh
# Needs docker, plus the generated RecompiledFuncs/ and rsp/ (scripts/recomp.sh) and initialized submodules.
# Output in dist/: GT64Recompiled-<version>-linux-x64.tar.gz (binary, assets, controller database) and
# GT64Recompiled-<version>-x86_64.AppImage (bundles SDL2 and the other libraries).
set -euo pipefail
cd "$(dirname "$0")/../.."

version=$(grep -oP 'version_string = "\K[^"]+' src/main/main.cpp)
image=mi-recomp-build:22.04
[[ -d RecompiledFuncs && -d rsp ]] || { echo "Run scripts/recomp.sh first"; exit 1; }

docker build -t "$image" scripts/release
mkdir -p dist

docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e VERSION="$version" -v "$PWD:/src" -w /src "$image" bash -euo pipefail -c '
    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-15 -DCMAKE_CXX_COMPILER=clang++-15 \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld-15 -static-libstdc++ -static-libgcc" > build/release-configure.log
    cmake --build build/release --target GT64Recompiled -j"$(nproc)"

    # Portable archive, like BanjoRecomp and Zelda64Recomp ship. (build/release/assets is only a symlink to /src/assets.)
    stage=build/release-stage/GT64Recompiled
    rm -rf build/release-stage && mkdir -p "$stage"
    cp build/release/GT64Recompiled recompcontrollerdb.txt README.md COPYING "$stage/"
    cp -r assets "$stage/"
    tar -czf "dist/GT64Recompiled-$VERSION-linux-x64.tar.gz" -C build/release-stage GT64Recompiled

    # AppImage: the app looks for assets/ next to the working directory, so AppRun changes into usr/bin first.
    appdir=build/release-AppDir
    rm -rf "$appdir" && mkdir -p "$appdir/usr/bin"
    cp build/release/GT64Recompiled recompcontrollerdb.txt "$appdir/usr/bin/"
    cp -r assets "$appdir/usr/bin/"
    cp scripts/release/icon.png "$appdir/GT64Recompiled.png"
    cp scripts/release/GT64Recompiled.desktop "$appdir/"
    /opt/linuxdeploy/AppRun --appdir="$appdir" -d "$appdir/GT64Recompiled.desktop" -i "$appdir/GT64Recompiled.png" \
        -e "$appdir/usr/bin/GT64Recompiled" > build/release-linuxdeploy.log
    # linuxdeploy makes AppRun a symlink to the binary; replace the link, not the file it points to.
    rm -f "$appdir/AppRun"
    cat > "$appdir/AppRun" <<"EOF"
#!/bin/sh
this_dir="$(dirname "$(readlink -f "$0")")"
if [ -f "portable.txt" ]; then
    export APP_FOLDER_PATH="$PWD"
fi
cd "$this_dir/usr/bin/"
exec ./GT64Recompiled "$@"
EOF
    chmod +x "$appdir/AppRun"
    # The host'"'"'s Wayland and GIO libraries must match its compositor and desktop, so don'"'"'t bundle them.
    rm -f "$appdir"/usr/lib/libwayland* "$appdir"/usr/lib/libgmodule*
    rm -rf "$appdir"/usr/lib/gio/modules
    cd build && ARCH=x86_64 LDAI_OUTPUT="../dist/GT64Recompiled-$VERSION-x86_64.AppImage" \
        /opt/linuxdeploy/usr/bin/linuxdeploy-plugin-appimage --appdir="release-AppDir" > release-appimage.log
'
ls -la dist/
