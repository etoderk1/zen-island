#!/bin/sh
# Сборка через MinGW-w64 (Linux/MSYS2). Результат: build/ZenIsland.exe
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
CC=${CC:-x86_64-w64-mingw32-gcc}
SQLITE_FLAGS="-O2 -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED"
[ -f build/sqlite3.o ] || $CC $SQLITE_FLAGS -c src/third_party/sqlite3.c -o build/sqlite3.o
for f in decode identify quirc version_db; do [ -f build/quirc_$f.o ] || $CC -O2 -c src/third_party/quirc/$f.c -o build/quirc_$f.o; done
$WINDRES src/island.rc -O coff -o build/island.res
$CXX -std=c++17 -O2 -s -municode -mwindows -DUNICODE -D_UNICODE \
  -static -static-libgcc -static-libstdc++ \
  src/island.cpp src/media.cpp src/lyrics.cpp src/viz.cpp src/sysevents.cpp src/clipstore.cpp src/tools.cpp src/recorder.cpp src/downloads.cpp src/net.cpp src/widgets.cpp src/notify.cpp src/texttools.cpp src/reminders.cpp src/sysctl.cpp src/cmdlogic.cpp src/cmdindex.cpp src/editor.cpp src/pods.cpp src/sysstats.cpp src/vpn.cpp src/fps.cpp src/update.cpp src/log.cpp build/sqlite3.o build/quirc_*.o build/island.res \
  -o build/ZenIsland.exe \
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -lruntimeobject -lshcore -luuid -lcomctl32 -lwinhttp -lsetupapi -ld3d11 -ldxgi -lmfuuid -lshell32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32 -liphlpapi -lpropsys -lwinmm
echo "OK: build/ZenIsland.exe"
# установщик: внутри него лежит только что собранный остров
(cd installer && $WINDRES -c 65001 setup.rc -O coff -o ../build/setup.res)
$CXX -std=c++17 -O2 -s -municode -mwindows -DUNICODE -D_UNICODE -DZEN_VERSION_SETUP -include src/version.h \
  -static -static-libgcc -static-libstdc++ installer/setup.cpp build/setup.res -o build/ZenIsland-Setup.exe \
  -lole32 -luuid -lshell32 -lcomctl32 -ladvapi32 -luser32 -lgdi32
echo "OK: build/ZenIsland-Setup.exe"
