from pathlib import Path
import sys,shutil,json,hashlib
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
R2=HERE.parent/'wer_launcher_100_r2'
OUT=ROOT/'90_릴리즈/WER REPACK_VER.1.0.0'
QA=R2/'qa/한글 경로 WER 1.0.0'

def write(p,s,bom=False):
    p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,encoding='utf-8-sig' if bom else 'utf-8',newline='\r\n' if p.suffix=='.bat' else '\n')

def install(target):
    text=(R2/'wer-server.ps1').read_text('utf-8-sig')
    lib=text[text.index("$ErrorActionPreference="):text.index('function IsWatchRunning')]
    write(target/'scripts/wer-console-prepare.ps1',"param([ValidateSet('mysql','auth','world')][string]$Action)\n"+lib+(HERE/'prepare-tail.ps1').read_text('utf-8'),True)
    for name,action,cmd in [('1_MYSQL.bat','mysql','"mysql\\bin\\mysqld.exe" --defaults-file=mysql/my.ini --console'),('2_AUTHSERVER.bat','auth','"authserver.exe" -c configs/authserver.conf'),('3_WORLDSERVER.bat','world','"worldserver.exe" -c configs/worldserver.conf')]:
        write(target/name,f'''@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title WER 1.0.0 - {action.upper()}
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\\wer-console-prepare.ps1" -Action {action}
if errorlevel 1 goto failed
{cmd}
set "WER_EXIT=%ERRORLEVEL%"
echo.
echo Process ended. See logs if it stopped unexpectedly.
pause
exit /b %WER_EXIT%
:failed
pause
exit /b 1
''')

if __name__=='__main__':install(QA if sys.argv[1]=='qa' else HERE/'staging')
