@echo off
setlocal enabledelayedexpansion

set "ERRFILE=%~dp0Error.txt"

echo Waiting for device...
adb wait-for-device >nul 2>&1

echo.
echo ============================================
echo   XR Camera Control - VR APK Installer
echo ============================================
echo.
echo Hint:
echo   Choose Y (uninstall + clear data) if keep-data install failed.
echo.
echo   WARNING: Choosing Y will CLEAR ALL APP DATA!
echo ============================================
echo.

set /p CLEAR="Uninstall old version (clear data)? [y/N]: "
if /i not "!CLEAR!"=="y" goto DO_INSTALL

call :Uninstall
if "!FAILED!"=="1" goto FAILED
set "LABEL=Install new app"
goto DO_INSTALL

:DO_INSTALL
if not defined LABEL set "LABEL=Install app (keep data)"
call :Install "!LABEL!"
if "!FAILED!"=="1" goto FAILED
call :StartApp
if "!FAILED!"=="1" goto FAILED
call :SetAutoStart
if "!FAILED!"=="1" goto FAILED
call :Reboot
if "!FAILED!"=="1" goto FAILED

echo.
echo ============================================
echo   INSTALL COMPLETE!
echo ============================================
goto CLEANUP

:FAILED
echo.
echo ============================================
echo   INSTALL FAILED!
echo ============================================

:CLEANUP
del "!ERRFILE!" >nul 2>&1
pause
if "!FAILED!"=="1" exit /b 1
exit /b 0

:Install
echo.
echo [%~1]
adb install -r app-release-vr.apk >nul 2>"!ERRFILE!"
call :CheckResult
goto :eof

:Uninstall
echo.
echo [Uninstall old app (clear data)]
adb uninstall com.ssnwt.egoserver >nul 2>nul
adb uninstall com.ssnwt.helloxr >nul 2>nul
echo   -^> Done
goto :eof

:StartApp
echo.
echo [Start app]
adb shell am force-stop com.ssnwt.egoserver >nul 2>&1
adb shell am start -n com.ssnwt.egoserver/com.ssnwt.helloxr.VrNativeActivity >nul 2>&1
goto :eof

:SetAutoStart
echo.
echo [Configure auto-start on boot]
adb shell setprop persist.vr.autostartapp.pkg com.ssnwt.egoserver >nul 2>"!ERRFILE!"
adb shell setprop persist.vr.autostartapp.entry com.ssnwt.helloxr.VrNativeActivity >nul 2>>"!ERRFILE!"
adb shell setprop persist.sxr.autostartapp.pkg com.ssnwt.egoserver >nul 2>"!ERRFILE!"
adb shell setprop persist.sxr.autostartapp.entry com.ssnwt.helloxr.VrNativeActivity >nul 2>>"!ERRFILE!"
call :CheckResult
goto :eof

:Reboot
echo.
set /a SEC=5
echo [Reboot device, waiting 5s]
:RebootLoop
timeout /t 1 /nobreak >nul
set /a SEC-=1
<nul set /p "=."
if !SEC! gtr 0 goto RebootLoop
echo.
adb reboot >nul 2>"!ERRFILE!"
call :CheckResult
goto :eof

:CheckResult
for %%i in ("!ERRFILE!") do (
    if %%~zi gtr 0 (
        echo   -^> Failed
        type "!ERRFILE!"
        set "FAILED=1"
    ) else (
        echo   -^> Success
    )
)
goto :eof
