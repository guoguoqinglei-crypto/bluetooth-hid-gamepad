@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo.
echo   蓝牙手柄上位机 · 诊断
echo   ------------------------------------------
echo   会枚举系统里所有 HID 接口、抓 3 秒原始报文、
echo   并试发一条 RGB，结果写进 diag.txt。
echo.
echo   请先把板子上电、按一下板上的键让它回连，
echo   确认 Windows 蓝牙里「MP GAMEPAD」是「已连接」。
echo.

btgamepad.exe --diag

if exist diag.txt (
    echo 诊断完成，正在打开 diag.txt ……
    start "" notepad diag.txt
) else (
    echo 没有生成 diag.txt，请确认 btgamepad.exe 与本文件在同一目录。
    pause
)
