from stage_release import HERE,write,save
from pathlib import Path
import re,subprocess,os
vs=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools')
tool=vs/'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64'
s=(HERE/'Banner.rsp').read_text('utf-8').replace(str(HERE/'src/common/Banner.cpp'),str(HERE/'encoding_probe.cpp')).replace('Banner.obj','encoding_probe.obj').replace('Banner.pdb','encoding_probe.pdb')
write(HERE/'probe-cl.rsp',s)
s=(HERE/'authserver-link.rsp').read_text('utf-8').replace('authserver.exe','encoding_probe.exe').replace('authserver.pdb','encoding_probe.pdb').replace('authserver.lib','encoding_probe.lib')
s=re.sub(r'(?i)AUTHSERVER\.DIR\\RELEASE\\[^\s]+\.(OBJ|RES)','',s)
s+='\n"'+str(HERE/'obj/encoding_probe.obj')+'"\n'
write(HERE/'probe-link.rsp',s)
batch=f'@echo off\ncall "{vs / "VC/Auxiliary/Build/vcvars64.bat"}" >nul\n"{tool / "cl.exe"}" @"{HERE / "probe-cl.rsp"}"\nif errorlevel 1 exit /b 1\ncd /d "{HERE.parents[1] / "build/core-latest-batch1-vs2022/src/server/apps"}"\n"{tool / "link.exe"}" @"{HERE / "probe-link.rsp"}"\n'
write(HERE/'probe-build.cmd',batch)
with (HERE/'probe-build.log').open('wb') as f:r=subprocess.run(['cmd.exe','/d','/c',str(HERE/'probe-build.cmd')],stdout=f,stderr=subprocess.STDOUT,creationflags=0x08000000)
assert r.returncode==0
env=os.environ.copy();env['PATH']=str(HERE.parents[2]/'90_릴리즈/WER REPACK_VER.1.1.0')+';'+env['PATH']
r=subprocess.run([str(HERE/'bin/encoding_probe.exe')],capture_output=True,creationflags=0x08000000,env=env)
assert r.returncode==0
out=r.stdout.decode('utf-8').replace('\r\n','\n');err=r.stderr.decode('utf-8').replace('\r\n','\n')
assert out=='한국 에뮬레이터 연구소 WER VER1.1.0\n'+'한'*40000+'\n'
assert err=='한국어 오류 출력 12340\n'
save(HERE/'redirect_test.json',dict(pass_all=True,utf8_stdout=True,utf8_stderr=True,long_utf8_line_bytes=120000,old_limit_bytes=32768))
print('UTF8 redirection stdout/stderr + 120KB Korean line PASS')
