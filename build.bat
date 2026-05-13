@echo off
rem btc-miner-psp build wrapper.
rem
rem No native Windows pspdev binaries exist as of v20260501; we
rem invoke WSL Ubuntu where pspdev is installed at ~/pspdev.  The
rem WSL build writes back to the same /mnt/i/btc-miner-psp tree, so
rem EBOOT.PBP lands in the project root visible to Windows.

setlocal enableextensions
cd /d "%~dp0"

wsl --exec bash -lc "cd /mnt/i/btc-miner-psp && export PSPDEV=$HOME/pspdev && export PATH=$PSPDEV/bin:$PATH && make clean 2>&1 | tail -3 && make 2>&1 | tail -20"

if not exist EBOOT.PBP (
    echo Build FAILED.
    exit /b 1
)

echo Build OK: %~dp0EBOOT.PBP
endlocal
