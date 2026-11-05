@echo off
rem Unified test runner: build & run all unit tests, exit non-zero on any failure.
rem Usage: tests\run_tests.bat   (needs C:\zig\zig.exe or zig on PATH)
setlocal enabledelayedexpansion
set ZIG=C:\zig\zig.exe
if not exist "%ZIG%" set ZIG=zig
cd /d "%~dp0.."
if not exist build mkdir build

set FAILED=0

echo [1/9] selftest (text clean + search, incl. multi-keyword AND)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/selftest.cpp src/search.cpp src/text_clean.cpp src/config.cpp src/cache.cpp src/office.cpp src/legacy_office.cpp -o build\t_selftest.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_selftest.exe || set /a FAILED+=1 )

echo [2/9] test_align (OCR line aligned to UIA doc text)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_align.cpp src/align.cpp src/text_clean.cpp -o build\t_align.exe -luser32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_align.exe || set /a FAILED+=1 )

echo [3/9] test_config (settings read/write + 4-mode migration)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_config.cpp src/config.cpp -o build\t_config.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_config.exe || set /a FAILED+=1 )

echo [4/9] test_cache (cache dedup + disk quota)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_cache.cpp src/cache.cpp src/config.cpp -o build\t_cache.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_cache.exe || set /a FAILED+=1 )

echo [5/9] test_cachebuild (pre-build cache for a folder: build / reuse / empty / cancel)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_cachebuild.cpp src/search.cpp src/text_clean.cpp src/config.cpp src/cache.cpp src/office.cpp src/legacy_office.cpp -o build\t_cachebuild.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_cachebuild.exe || set /a FAILED+=1 )

echo [6/9] test_legacy (doc/xls: bad input must not crash, real samples must parse)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_legacy.cpp src/office.cpp src/legacy_office.cpp src/config.cpp -o build\t_legacy.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_legacy.exe || set /a FAILED+=1 )

echo [7/9] test_alloc (multi-file hits: fair share + grouping + pending)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_alloc.cpp src/search.cpp src/text_clean.cpp src/config.cpp src/cache.cpp src/office.cpp src/legacy_office.cpp -o build\t_alloc.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_alloc.exe || set /a FAILED+=1 )

echo [8/9] test_office_fmt (docx table separators + xlsx shared-string attrs)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_office_fmt.cpp src/office.cpp src/legacy_office.cpp src/config.cpp -o build\t_officefmt.exe -luser32 -lshell32 -lole32 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_officefmt.exe || set /a FAILED+=1 )

echo [9/9] test_ocr (projection line + right-side pure block, 24px)
"%ZIG%" c++ -target x86_64-windows-gnu -DUNICODE -D_UNICODE -O2 tests/test_ocr.cpp src/capture.cpp src/ocr_onnx.cpp src/text_clean.cpp src/config.cpp -o build\t_ocr.exe -luser32 -lgdi32 -lole32 -lshell32 -loleaut32 -luuid 2>nul
if errorlevel 1 ( echo   BUILD FAILED & set /a FAILED+=1 ) else ( build\t_ocr.exe || set /a FAILED+=1 )
if %FAILED% gtr 0 (
    echo RESULT: %FAILED% suite^(s^) FAILED
    exit /b 1
)
echo RESULT: all suites passed
exit /b 0
