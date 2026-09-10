@echo off
echo =======================PANCAKE 1 刷机工具=========================
echo =====================请先安装好adb驱动在进行烧录======================

echo                   当设备开机后，插入设备，进行烧写
echo ======================================================================


set key_pancake=17CB:1101
set key_sxr=17CB:1103
set wifi_node=/sys/bus/pci/devices/0000:01:00.0/uevent
del temp_results.txt >NUL 2>&1

:find_adb
@adb devices |findstr /E device
if %errorlevel%==1 (
    goto find_fastboot
) else (
    adb shell cat "%wifi_node%" | findstr /i /r /c:"%key_pancake%" /c:"%key_sxr%" > temp_results.txt
    findstr /E "%key_sxr%" temp_results.txt>null && goto reboot_bootloader || echo.
    goto device_error
)

:find_fastboot
echo find_fastboot
@fastboot devices|findstr /E fastboot
if %errorlevel%==1 (
    goto wait_device
) else (
    goto flashall
)

:wait_device
echo wait_device
adb wait-for-device
goto find_adb

:reboot_bootloader
adb reboot bootloader
goto flashall

:flashall
echo Detecting device type...
for /f "tokens=2 delims=:" %%a in ('fastboot.exe getvar ddr-type 2^>^&1 ^| findstr /r "^ddr-type:"') do set DEVICE_TYPE=%%a
set DEVICE_TYPE=%DEVICE_TYPE: =%

if "%DEVICE_TYPE%"=="7" (
    set TYPE_SUFFIX=_typeB
    echo Device Type: Type B
) else (
	if not "%DEVICE_TYPE%"=="8" (
	echo WARNING: device type not detected or unknown ^(%DEVICE_TYPE%^), defaulting to Type A
	)
	set TYPE_SUFFIX=_typeA
	echo Device Type: Type A
)
fastboot.exe oem xbl-unlock
fastboot.exe oem xbl-lock-status

fastboot.exe flash abl_a abl.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash abl_b abl.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash aop_a aop.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash aop_b aop.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash apdp apdp.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash boot_a boot.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash boot_b boot.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash bluetooth_a BTFM.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash bluetooth_b BTFM.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash cmnlib_a cmnlib.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash cmnlib64_a cmnlib64.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash cmnlib_b cmnlib.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash cmnlib64_b cmnlib64.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash devcfg_a devcfg.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash devcfg_b devcfg.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash dsp_a dspso.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash dsp_b dspso.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash dtbo_a dtbo.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash dtbo_b dtbo.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash featenabler_a featenabler.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash featenabler_b featenabler.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash hyp_a hyp.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash hyp_b hyp.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash imagefv_a imagefv%TYPE_SUFFIX%.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash imagefv_b imagefv%TYPE_SUFFIX%.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash keymaster_a km41.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash keymaster_b km41.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash logfs logfs_ufs_8mb.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash metadata metadata.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash multiimgoem_a multi_image.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash multiimgoem_b multi_image.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash modem_a NON-HLOS.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash modem_b NON-HLOS.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash qupfw_a qupv3fw.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash qupfw_b qupv3fw.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash recovery_a  recovery.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash recovery_b  recovery.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash spunvm spunvm.bin
if %errorlevel%==1 goto flasherr

fastboot.exe flash tz_a tz.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash tz_b tz.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash uefisecapp_a uefi_sec.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash uefisecapp_b uefi_sec.mbn
if %errorlevel%==1 goto flasherr

fastboot.exe flash vbmeta_a vbmeta.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash vbmeta_b vbmeta.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash vbmeta_system_a vbmeta_system.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash vbmeta_system_b vbmeta_system.img
if %errorlevel%==1 goto flasherr

fastboot.exe flash xbl_a xbl%TYPE_SUFFIX%.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash xbl_b xbl%TYPE_SUFFIX%.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash xbl_config_a xbl_config%TYPE_SUFFIX%.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash xbl_config_b xbl_config%TYPE_SUFFIX%.elf
if %errorlevel%==1 goto flasherr

fastboot.exe flash ddr zeros_5sectors.bin
if %errorlevel%==1 goto flasherr
fastboot.exe erase super
fastboot.exe -S 512M flash super  super.img
if %errorlevel%==1 goto flasherr

fastboot.exe erase userdata
fastboot.exe -S 512M flash userdata  userdata.img
if %errorlevel%==1 goto flasherr
fastboot.exe oem xbl-lock
fastboot.exe oem xbl-lock-status

fastboot.exe set_active a

echo ==============================================================
echo #######  烧写成功，设备会自动重启进入系统                 #######
echo #######          fastboot success                         #######
echo #######  device will automatically restart                #######
echo ==============================================================
fastboot.exe reboot
pause
exit 0


:flasherr
color C
echo ==============================================================
echo #######   烧写失败，请拔掉usb线，然后重新运行该脚本进行烧写    ########
echo #######               fastboot error                           ########
echo #######   Please unplug the USB cable and run the script again ########
echo ==============================================================
pause
exit 0


:device_error
color C
echo ==============================================================
echo #######                                               ########
echo #######             Unsupported devices               ########
echo #######                                               ########
echo ==============================================================
pause
exit 0
