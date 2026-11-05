@echo off
setlocal
chcp 65001 >nul

set ZIG=C:\zig\zig.exe
if not exist "%ZIG%" set ZIG=zig

if not exist dist mkdir dist
if not exist build mkdir build

echo [build] compiling with zig c++ ...
"%ZIG%" c++ -target x86_64-windows-gnu -municode -DUNICODE -D_UNICODE -O2 ^
  -Wl,--subsystem,windows ^
  src\main.cpp src\config.cpp src\text_clean.cpp src\search.cpp src\uia.cpp ^
  src\capture.cpp src\ocr_onnx.cpp src\office.cpp src\legacy_office.cpp src\settings.cpp src\cache.cpp ^
  src\bundle.cpp src\align.cpp src\listview.cpp src\theme.cpp src\flatbutton.cpp src\icon.cpp ^
  -o dist\LineOcrSearch.exe ^
  -luser32 -lgdi32 -lcomctl32 -lole32 -loleaut32 -lshell32 -lshlwapi -lcomdlg32 -luuid -loleacc

if errorlevel 1 (
  echo [build] compile FAILED
  exit /b 1
)

echo [build] compiling packer ...
"%ZIG%" c++ -target x86_64-windows-gnu -municode -DUNICODE -D_UNICODE -O2 ^
  -Wl,--subsystem,console ^
  tools\pack.cpp -o build\pack.exe

if errorlevel 1 (
  echo [build] packer FAILED
  exit /b 1
)

echo [build] embedding runtime resources into the exe ...
build\pack.exe dist\LineOcrSearch.exe ^
  "runtime\onnxruntime.dll=res\runtime\onnxruntime.dll" ^
  "model\rec.onnx=res\model\rec.onnx" ^
  "model\ppocr_keys_v1.txt=res\model\ppocr_keys_v1.txt"

if errorlevel 1 (
  echo [build] pack FAILED
  exit /b 1
)

echo [build] OK -^> dist\LineOcrSearch.exe
echo [build] deps are embedded; first run extracts them into LineOcrSearchData\ next to the exe (skipped if size matches)
endlocal
