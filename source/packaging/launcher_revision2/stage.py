from pathlib import Path
import shutil,os,json,re,sys,hashlib
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
OUT=ROOT/'90_릴리즈/WER REPACK_VER.1.0.0'
QA=HERE/'qa/한글 경로 WER 1.0.0'
def write(p,s,bom=False):
    p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,encoding='utf-8-sig' if bom else 'utf-8',newline='\r\n' if p.suffix=='.bat' else '\n')
def save(p,obj):write(p,json.dumps(obj,ensure_ascii=False,indent=2)+'\n')
WRAPPERS={
 '01_MySQL_시작.bat':'mysql','02_로그인서버_시작.bat':'auth','03_월드서버_시작.bat':'world','04_전체서버_시작.bat':'start',
 '05_서버상태_확인.bat':'status','06_오류로그_확인.bat':'logs','07_월드서버_종료.bat':'stopworld',
 '08_로그인서버_종료.bat':'stopauth','09_MySQL_종료.bat':'stopmysql','10_전체서버_종료.bat':'stopall',
 '_Start_MySQL.bat':'mysql','_Start_AuthServer.bat':'auth','_Start_WorldServer.bat':'world','_Start_Server.bat':'start',
 '_Stop_MySQL.bat':'stopmysql','_Stop_AuthServer.bat':'stopauth','_Stop_WorldServer.bat':'stopworld','_Stop_Server.bat':'stopall',
 '_Status.bat':'status','_Logs.bat':'logs','_Check.bat':'check'}
def install(root):
    for n in ['wer-server.ps1','wer-entry.ps1','wer-menu.ps1']:write(root/'scripts'/n,(HERE/n).read_text('utf-8-sig'),True)
    for name,action in WRAPPERS.items():
        write(root/name,fr'''@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title WER 1.0.0 - {action}
cd /d "%~dp0"
set "WER_ACTION={action}"
if /I "%~1"=="--check" set "WER_ACTION=check"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\wer-entry.ps1" -Action "%WER_ACTION%"
set "WER_EXIT=%ERRORLEVEL%"
if /I not "%~1"=="--no-pause" if /I not "%~1"=="--check" pause
endlocal & exit /b %WER_EXIT%
''')
    menu=r'''@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title WER REPACK_VER.1.0.0
cd /d "%~dp0"
set "WER_CHECK="
if /I "%~1"=="--check" set "WER_CHECK=-Check"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\wer-menu.ps1" %WER_CHECK%
set "WER_EXIT=%ERRORLEVEL%"
if not "%WER_EXIT%"=="0" if /I not "%~1"=="--check" pause
endlocal & exit /b %WER_EXIT%
'''
    write(root/'00_WER_실행메뉴.bat',menu);write(root/'start.bat',menu)
    write(root/'mysql/init_mysql.bat','@echo off\ncall "%~dp0..\\_Start_MySQL.bat" %*\nexit /b %ERRORLEVEL%\n')
def setup():
    assert not QA.exists()
    def copy(a,b):
        a,b=Path(a),Path(b)
        if a.suffix.lower() in ('.exe','.dll','.sql') or 'data' in a.relative_to(OUT).parts:os.link(a,b)
        else:shutil.copy2(a,b)
    shutil.copytree(OUT,QA,copy_function=copy,ignore=shutil.ignore_patterns('source','전용애드온'))
    install(QA)
    p=QA/'scripts/settings.json';c=json.loads(p.read_text('utf-8'));c.update(MySqlPort=57701,AuthPort=37401,WorldPort=59901,RealmAddress='127.0.0.1',BindIP='127.0.0.1',MinFreeCommitMB=6144);save(p,c)
    p=QA/'configs/modules/playerbots.conf';s=p.read_text('utf-8')
    opts={'MinRandomBots':10,'MaxRandomBots':10,'AddClassAccountPoolSize':0,'RandomBotAccountCount':2,'RandomBotGuildCount':2,'RandomBotGuildSizeMax':3}
    for k,v in opts.items():s=re.sub(r'(?m)^AiPlayerbot\.'+k+r'\s*=.*$',f'AiPlayerbot.{k} = {v}',s)
    write(p,s);save(HERE/'qa_policy.json',dict(root=str(QA),mode='LOCALHOST_ONLY_DIAGNOSTIC',ports=[57701,37401,59901],bots=opts,production_started=False))
    print('QA staged')
if __name__=='__main__':
    if sys.argv[1]=='qa':setup()
    elif sys.argv[1]=='refresh-qa':install(QA)
