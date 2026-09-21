from stage import *
import subprocess,socket,time
events=[]
def event(s):
    events.append(dict(at=time.strftime('%Y-%m-%d %H:%M:%S'),step=s));save(HERE/'test_progress.json',events);print(s,flush=True)
def run(cmd,expect=0,input=None,timeout=180):
    stamp=time.time_ns();out=HERE/f'test-{stamp}.out';err=HERE/f'test-{stamp}.err'
    with out.open('wb') as o,err.open('wb') as e:
        p=subprocess.run(cmd,input=input,stdout=o,stderr=e,creationflags=0x08000000,timeout=timeout)
    if p.returncode!=expect:raise RuntimeError(f'Unexpected code {p.returncode}; see {out.name}, {err.name}')
    return out.read_text('utf-8',errors='replace')
def batch(file,arg='--no-pause',expect=0,input=None):
    return run(f'cmd.exe /d /s /c ""{QA/file}" {arg}"',expect,input)
def ps(code):return run(['powershell.exe','-NoProfile','-Command',code]).strip()
def port(n):
    try:
        with socket.create_connection(('127.0.0.1',n),.4):return True
    except OSError:return False
def sql(q):
    return run([str(QA/'mysql/bin/mysql.exe'),'--defaults-extra-file=mysql/client.cnf','--default-character-set=utf8mb4','--batch','--skip-column-names','-e',q]).strip()
def wait(test,t=150):
    end=time.monotonic()+t
    while time.monotonic()<end:
        if test():return
        time.sleep(2)
    raise RuntimeError('Timed out waiting for QA state')
def pids(name):
    s=ps(f"@(Get-CimInstance Win32_Process -Filter \"Name='{name}.exe'\" | Where-Object {{$_.ExecutablePath -eq '{QA}/{name}.exe'.Replace('/','\\')}} | Select-Object -ExpandProperty ProcessId) -join ','")
    return [int(i) for i in s.split(',') if i]
def ready():return port(37401) and port(59901) and sql('SELECT flag & 3 FROM wl_auth.realmlist WHERE id=1;')=='0'
def fault(name):
    old=pids(name);assert len(old)==1
    ps(f"$p=Get-CimInstance Win32_Process -Filter 'ProcessId={old[0]}'; if($p.ExecutablePath -ne '{QA}/{name}.exe'.Replace('/','\\')){{throw 'wrong target'}};Stop-Process -Id {old[0]} -Force")
    wait(lambda:ready() and pids(name)!=old)
try:
    os.chdir(QA)
    for n in list(WRAPPERS)+['start.bat','00_WER_실행메뉴.bat']:
        text=batch(n,'--check');assert '검사 완료' in text,(n,text)
    event('All numbered/English/menu wrappers --check PASS')
    before=(port(57701),port(37401),port(59901));text=batch('00_WER_실행메뉴.bat','',input=b'0\n');assert '로그인 서버 시작' in text
    assert before==(port(57701),port(37401),port(59901));event('Korean menu render and exit without starting servers PASS')
    batch('01_MySQL_시작.bat');assert port(57701) and not port(37401) and not port(59901);event('MySQL-only start PASS')
    batch('02_로그인서버_시작.bat');assert port(37401) and not port(59901);auth=pids('authserver');event('Auth-only start PASS')
    batch('_Start_AuthServer.bat');assert pids('authserver')==auth;event('Duplicate auth start reuses same PID PASS')
    batch('03_월드서버_시작.bat');wait(ready);assert pids('authserver')==auth;event('World start with existing auth / online realm PASS')
    original_world=pids('worldserver');batch('_Start_Server.bat');assert pids('worldserver')==original_world;event('Duplicate all-start PASS')
    batch('08_로그인서버_종료.bat',expect=1);batch('09_MySQL_종료.bat',expect=1);assert ready();event('Unsafe auth/DB stop while world runs rejected PASS')
    fault('worldserver');event('World crash autorecovery PASS')
    fault('authserver');event('Auth crash autorecovery and online flag PASS')
    snapshot=dict(bots=sql("SELECT COUNT(*),SUM(c.online) FROM wl_characters.characters c JOIN wl_auth.account a ON a.id=c.account WHERE a.username LIKE 'rndbot%';"),guilds=sql('SELECT g.name,COUNT(m.guid) FROM wl_characters.guild g JOIN wl_characters.guild_member m ON m.guildid=g.guildid GROUP BY g.guildid,g.name;'),auctions=sql('SELECT COUNT(*) FROM wl_characters.auctionhouse;'),realm=sql('SELECT flag FROM wl_auth.realmlist WHERE id=1;'))
    batch('07_월드서버_종료.bat');assert not port(59901) and port(37401) and port(57701);event('World-only graceful stop; auth and DB preserved PASS')
    time.sleep(18);assert not port(59901);event('Intentionally stopped world does NOT autorestart PASS')
    batch('08_로그인서버_종료.bat');assert not port(37401) and port(57701);event('Auth-only stop; DB preserved PASS')
    batch('09_MySQL_종료.bat');assert not port(57701);event('MySQL-only shutdown PASS')
    batch('04_전체서버_시작.bat');wait(ready);event('Full restart of existing DB in Korean directory PASS')
    batch('_Stop_Server.bat');assert not any(port(p) for p in [57701,37401,59901]);event('Reference-compatible Stop_Server stops world/auth/MySQL PASS')
    text=batch('05_서버상태_확인.bat');assert '종료' in text and '월드 서버' in text
    batch('06_오류로그_확인.bat');event('Korean status and log viewer PASS')
    prod=json.loads((ROOT/'migration_ops/runtime_watchdog/status.json').read_text('utf-8'));assert prod['state']=='stopped'
    assert not any(port(p) for p in [57598,3724,59823]);event('Production remains STOPPED PASS')
    save(HERE/'test_result.json',dict(pass_all=True,events=events,snapshot=snapshot,korean_path=str(QA),production_started=False))
except Exception as e:
    event('FAIL '+str(e));save(HERE/'test_result.json',dict(pass_all=False,events=events));raise
