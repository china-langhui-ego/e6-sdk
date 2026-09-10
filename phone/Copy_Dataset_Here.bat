@echo off
set power_on_off=0

adb wait-for-device pull /sdcard/Android/data/com.ssnwt.egoserver/files/dataset .
adb wait-for-device shell rm -rf /sdcard/Android/data/com.ssnwt.egoserver/files/dataset

pause
