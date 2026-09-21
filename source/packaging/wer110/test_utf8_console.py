from stage_release import HERE,ROOT,QA,OUT,write,save
from pathlib import Path
import subprocess,socket,time,ctypes,sys,os,base64,json
events=[]
def event(s):events.append(s);save(HERE/'qa_progress.json',events);print(s,flush=True)
def port(n):
    try:
        with socket.create_connection(('127.0.0.1',n),.2):return True
    except OSError:return False
def wait(fn,seconds=180):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        if fn():return
        time.sleep(1)
    raise RuntimeError('Timeout')
def ps(s):
    s='[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false);'+s
    r=subprocess.run(['powershell.exe','-NoProfile','-EncodedCommand',base64.b64encode(s.encode('utf-16le')).decode()],capture_output=True,creationflags=0x08000000)
    assert r.returncode==0,r.stderr;return r.stdout.decode('utf-8-sig').strip()
def query(s):
    r=subprocess.run([str(QA/'mysql/bin/mysql.exe'),'--defaults-extra-file=mysql/client.cnf','--default-character-set=utf8mb4','--batch','--skip-column-names','-e',s],cwd=QA,capture_output=True,creationflags=0x08000000)
    assert r.returncode==0,r.stderr;return r.stdout.decode('utf-8').strip()
def native(name):
    target=str(QA/('mysql/bin/mysqld.exe' if name=='mysqld' else name+'.exe')).replace('/','\\')
    return ps(f"@(Get-CimInstance Win32_Process -Filter \"Name='{name}.exe'\" | Where-Object {{$_.ExecutablePath -eq '{target}'}}).Count")!='0'
def child(mode,p,path):
    r=subprocess.run([sys.executable,__file__,mode,str(p.pid),str(path)],capture_output=True,creationflags=0x08000000)
    if mode=='read' and r.returncode!=0:return False
    assert r.returncode==0,r.stderr.decode(errors='replace')
    return True
def launch(n):
    p=subprocess.Popen(['cmd.exe','/d','/c',str(QA/n)],cwd=QA,creationflags=subprocess.CREATE_NEW_CONSOLE)
    time.sleep(.3)
    child('size',p,HERE/'unused')
    return p
def capture(p,name):
    dest=HERE/(name+'_console.txt')
    if not child('read',p,dest):return ''
    return dest.read_text('utf-8')
def stop(p,name,n):
    child('ctrlc',p,HERE/'unused');wait(lambda:not port(n) and not native(name))
    if p.poll() is None:p.terminate();p.wait(10)
    event(name+' Ctrl+C normal exit PASS')

if len(sys.argv)>1:
    from ctypes import wintypes as w
    class COORD(ctypes.Structure):_fields_=[('X',w.SHORT),('Y',w.SHORT)]
    class RECT(ctypes.Structure):_fields_=[('Left',w.SHORT),('Top',w.SHORT),('Right',w.SHORT),('Bottom',w.SHORT)]
    class INFO(ctypes.Structure):_fields_=[('size',COORD),('cursor',COORD),('attributes',w.WORD),('window',RECT),('maxsize',COORD)]
    class CELL(ctypes.Structure):_fields_=[('character',w.WCHAR),('attributes',w.WORD)]
    k=ctypes.WinDLL('kernel32',use_last_error=True);u=ctypes.WinDLL('user32',use_last_error=True)
    k.GetConsoleWindow.restype=w.HWND;u.IsWindowVisible.argtypes=[w.HWND]
    k.FreeConsole();assert k.AttachConsole(int(sys.argv[2]));assert u.IsWindowVisible(k.GetConsoleWindow())
    if sys.argv[1]=='ctrlc':
        assert k.SetConsoleCtrlHandler(None,True);assert k.GenerateConsoleCtrlEvent(0,0);time.sleep(1)
    else:
        k.CreateFileW.restype=w.HANDLE;k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,ctypes.c_void_p,w.DWORD,w.DWORD,w.HANDLE]
        h=k.CreateFileW('CONOUT$',0xc0000000,3,None,3,0,None)
        k.GetConsoleScreenBufferInfo.argtypes=[w.HANDLE,ctypes.POINTER(INFO)]
        info=INFO();assert k.GetConsoleScreenBufferInfo(h,ctypes.byref(info))
        if sys.argv[1]=='size':
            k.SetConsoleScreenBufferSize.argtypes=[w.HANDLE,COORD]
            assert k.SetConsoleScreenBufferSize(h,COORD(info.size.X,10000))
        else:
            k.ReadConsoleOutputW.argtypes=[w.HANDLE,ctypes.POINTER(CELL),COORD,COORD,ctypes.POINTER(RECT)]
            # Preserve row/cell boundaries; Korean occupies leading+trailing cells.
            rows=[]
            for y in range(0,info.size.Y,100):
                height=min(100,info.size.Y-y);cells=(CELL*(info.size.X*height))();rect=RECT(0,y,info.size.X-1,y+height-1)
                assert k.ReadConsoleOutputW(h,cells,COORD(info.size.X,height),COORD(0,0),ctypes.byref(rect))
                for row in range(height):
                    rows.append(''.join(c.character for c in cells[row*info.size.X:(row+1)*info.size.X] if not(c.attributes & 0x0200)).rstrip())
            write(Path(sys.argv[3]),'\n'.join(rows))
        k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle(h)
    k.FreeConsole();sys.exit(0)

from pathlib import Path
try:
    assert not any(port(n) for n in (57711,37411,59911))
    # Production/user-owned 1.0.0 stays untouched; record exact running identity.
    identity="Get-CimInstance Win32_Process -Filter \"Name='authserver.exe'\" | Where-Object {$_.ExecutablePath -like '*90_릴리즈*1.0.0*'} | Select-Object ProcessId,CreationDate,ExecutablePath | ConvertTo-Json -Compress"
    before=ps(identity)
    db=launch('1_MYSQL.bat')
    wait(lambda:(QA/'mysql/wer-ready').exists() and port(57711) and ' --console' in ps("(Get-CimInstance Win32_Process -Filter \"Name='mysqld.exe'\" | Where-Object {$_.ExecutablePath -like '*wer_repack_110*'}).CommandLine -join ' '"),240)
    assert port(57711);event('Fresh Korean-path DB and visible MySQL PASS')
    auth=launch('2_AUTHSERVER.bat');wait(lambda:port(37411))
    text=capture(auth,'auth')
    for term in ['한국 에뮬레이터 연구소','WER REPACK VER1.1.0','와우 리치왕 버전 3.3.5a','2026-09-21','Added realm "한국 에뮬레이터 연구소"']:assert term in text,term
    assert 'wow-legends.eu' not in text and 'WOW Legends 3.3.5a' not in text
    event('Actual auth console Korean banner and realm text PASS')
    world=launch('3_WORLDSERVER.bat')
    wait(lambda:'WER REPACK VER1.1.0' in capture(world,'world'))
    text=(HERE/'world_console.txt').read_text('utf-8');assert '한국 에뮬레이터 연구소' in text and '와우 리치왕 버전 3.3.5a' in text
    wait(lambda:port(59911) and query('SELECT flag & 3 FROM wl_auth.realmlist WHERE id=1;')=='0')
    assert query('SELECT name FROM wl_auth.realmlist WHERE id=1;')=='한국 에뮬레이터 연구소'
    event('Actual world console Korean banner / realm online PASS')
    stop(world,'worldserver',59911);stop(auth,'authserver',37411);stop(db,'mysqld',57711)
    after=ps(identity);assert before==after;event('Existing 1.0.0 auth process identity unchanged PASS')
    save(HERE/'qa_result.json',dict(pass_all=True,events=events,realm_name='한국 에뮬레이터 연구소',existing_user_server_unchanged=True))
except Exception as e:event('FAIL '+str(e));raise
