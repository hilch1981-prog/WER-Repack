from build_console import *
import subprocess,socket,time,ctypes,os
events=[];windows=[]
def event(s):
    events.append(s);write(HERE/'test_progress.json',json.dumps(events,ensure_ascii=False,indent=2));print(s,flush=True)
def port(n):
    try:
        with socket.create_connection(('127.0.0.1',n),.3):return True
    except OSError:return False
def wait(fn,t=120):
    end=time.monotonic()+t
    while time.monotonic()<end:
        if fn():return
        time.sleep(1)
    raise RuntimeError('timed out')
def sql(q):
    p=subprocess.run([str(QA/'mysql/bin/mysql.exe'),'--defaults-extra-file=mysql/client.cnf','--batch','--skip-column-names','-e',q],cwd=QA,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=0x08000000)
    assert p.returncode==0,p.stderr.decode(errors='replace');return p.stdout.decode('utf-8').strip()
def ps(s):
    import base64
    p=subprocess.run(['powershell.exe','-NoProfile','-EncodedCommand',base64.b64encode(s.encode('utf-16le')).decode()],stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=0x08000000)
    assert p.returncode==0,p.stderr.decode(errors='replace');return p.stdout.decode('utf-8-sig').strip()
def owned(name):
    path=str(QA/('mysql/bin/mysqld.exe' if name=='mysqld' else name+'.exe')).replace('/','\\')
    return ps(f"@(Get-CimInstance Win32_Process -Filter \"Name='{name}.exe'\" | Where-Object {{$_.ExecutablePath -eq '{path}'}}).Count")!='0'
def start(name,portnum):
    p=subprocess.Popen(['cmd.exe','/d','/c',str(QA/name)],cwd=QA,creationflags=subprocess.CREATE_NEW_CONSOLE)
    windows.append(p);wait(lambda:port(portnum));assert p.poll() is None
    # Assert the batch is actually attached to a console window, not a hidden watchdog.
    probe=subprocess.run([sys.executable,__file__,'probe',str(p.pid)],capture_output=True,creationflags=0x08000000)
    assert probe.returncode==0,probe.stderr
    event(name+' visible console PASS');return p
def stop(p,name,portnum):
    r=subprocess.run([sys.executable,__file__,'ctrlc',str(p.pid)],capture_output=True,creationflags=0x08000000)
    assert r.returncode==0,r.stderr
    wait(lambda:not port(portnum) and not owned(name))
    if p.poll() is None:p.terminate();p.wait(timeout=10) # Empty QA batch window only, after server stopped.
    event(name+' console Ctrl+C graceful exit PASS')

if len(sys.argv)>1:
    k=ctypes.WinDLL('kernel32',use_last_error=True);u=ctypes.WinDLL('user32',use_last_error=True)
    k.GetConsoleWindow.restype=ctypes.c_void_p;u.IsWindowVisible.argtypes=[ctypes.c_void_p]
    k.FreeConsole();assert k.AttachConsole(int(sys.argv[2])),ctypes.get_last_error()
    assert k.GetConsoleWindow() and u.IsWindowVisible(k.GetConsoleWindow())
    if sys.argv[1]=='ctrlc':
        assert k.SetConsoleCtrlHandler(None,True)
        assert k.GenerateConsoleCtrlEvent(0,0);time.sleep(1)
    k.FreeConsole();sys.exit(0)

try:
    assert not any(port(n) for n in [57701,37401,59901,57598,3724,59823])
    db=start('1_MYSQL.bat',57701)
    # Preparation briefly uses a private initialization process, then replaces it with foreground mysqld.
    wait(lambda:(QA/'logs/watchdog.log').read_text('utf-8-sig').count('Realm address=127.0.0.1')>0)
    time.sleep(4);wait(lambda:port(57701))
    auth=start('2_AUTHSERVER.bat',37401);world=start('3_WORLDSERVER.bat',59901)
    wait(lambda:sql('SELECT flag & 3 FROM wl_auth.realmlist WHERE id=1;')=='0')
    event('Three independent consoles / realm online PASS')
    active=ps("@(Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object {$_.CommandLine -like '*wer-server.ps1*watch*' -and $_.CommandLine -like '*wer_launcher_100*'}).Count")
    assert active=='0';event('No background watchdog PASS')
    stop(world,'worldserver',59901);assert port(37401) and port(57701)
    stop(auth,'authserver',37401);assert port(57701)
    stop(db,'mysqld',57701)
    event('Production remains stopped PASS')
    assert not any(port(n) for n in [57598,3724,59823])
    write(HERE/'test_result.json',json.dumps(dict(pass_all=True,events=events,visible_windows_verified=True,production_started=False),ensure_ascii=False,indent=2))
except Exception as e:
    event('FAIL '+str(e));raise
