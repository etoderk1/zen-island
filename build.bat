@echo off
rem Build with MinGW-w64 (MSYS2 UCRT64 or w64devkit): g++ and windres must be in PATH
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\sqlite3.o gcc -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -c src\third_party\sqlite3.c -o build\sqlite3.o || goto :err
for %%f in (decode identify quirc version_db) do if not exist build\quirc_%%f.o gcc -O2 -c src\third_party\quirc\%%f.c -o build\quirc_%%f.o
windres src\island.rc -O coff -o build\island.res || goto :err
g++ -std=c++17 -O2 -s -municode -mwindows -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ ^
  src\island.cpp src\media.cpp src\lyrics.cpp src\viz.cpp src\sysevents.cpp src\clipstore.cpp src\tools.cpp src\recorder.cpp src\downloads.cpp src\net.cpp src\widgets.cpp src\notify.cpp src\texttools.cpp src\reminders.cpp src\sysctl.cpp src\cmdlogic.cpp src\cmdindex.cpp src\editor.cpp src\pods.cpp src\sysstats.cpp src\vpn.cpp src\fps.cpp src\update.cpp src\log.cpp build\sqlite3.o build\quirc_decode.o build\quirc_identify.o build\quirc_quirc.o build\quirc_version_db.o build\island.res -o build\ZenIsland.exe ^
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -lruntimeobject -lshcore -luuid -lcomctl32 -lwinhttp -lsetupapi -ld3d11 -ldxgi -lmfuuid -lshell32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32 -liphlpapi -lpropsys -lwinmm || goto :err
echo OK: build\ZenIsland.exe
rem установщик: внутри него лежит только что собранный остров
pushd installer
windres -c 65001 setup.rc -O coff -o ..\build\setup.res || (popd & goto :err)
popd
g++ -std=c++17 -O2 -s -municode -mwindows -DUNICODE -D_UNICODE -include src\version.h -static -static-libgcc -static-libstdc++ ^
  installer\setup.cpp build\setup.res -o build\ZenIsland-Setup.exe -lole32 -luuid -lshell32 -lcomctl32 -ladvapi32 -luser32 -lgdi32 || goto :err
echo OK: build\ZenIsland-Setup.exe
exit /b 0
:err
echo Build failed
exit /b 1
