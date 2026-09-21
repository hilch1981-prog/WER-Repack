from pathlib import Path
import os,re,subprocess,json,hashlib
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
BUILD=ROOT/'migration_ops/build/core-latest-batch1-vs2022'
SRC=ROOT/'WOW_Legends_CoreLatest_Batch1_Test'
VS=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools')
TOOL=VS/'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64'
def write(p,s):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,encoding='utf-8')
def sha(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def opt(s,key,value):return re.sub(r'/'+key+r'(?:"[^"]*"|\S+)',lambda m:'/'+key+'"'+str(value)+'"',s,flags=re.I)
steps=[];inputs={}
for rel,project in [('src/common/Banner.cpp','src/common/common.dir/Release/common.tlog'),('src/common/Utilities/Util.cpp','src/common/common.dir/Release/common.tlog'),('modules/mod-wowlegends/src/wowlegends_instantswing.cpp','modules/modules.dir/Release/modules.tlog')]:
    lines=(BUILD/project/'CL.command.1.tlog').read_text('utf-16').splitlines()
    old=str(SRC/rel).replace('/','\\');name=Path(rel).stem
    idx=next(i for i,l in enumerate(lines) if l.startswith('^') and old.upper() in l.upper())
    cmd=lines[idx+1]
    cmd=cmd.replace(old.upper(),str(HERE/rel)).replace(old,str(HERE/rel))
    cmd=opt(cmd,'Fo',HERE/'obj'/f'{name}.obj');cmd=opt(cmd,'Fd',HERE/'obj'/f'{name}.pdb')
    rsp=HERE/f'{name}.rsp';write(rsp,cmd)
    steps.append(f'"{TOOL / "cl.exe"}" @"{rsp}"')
    inputs[rel]=dict(before=sha(SRC/rel),after=sha(HERE/rel))
for kind,folder in [('common','src/common/common.dir/Release'),('modules','modules/modules.dir/Release')]:
    objs=sorted((BUILD/folder).rglob('*.obj'));assert len(objs)>10
    changed={'Banner','Util'} if kind=='common' else {'wowlegends_instantswing'}
    args=['/nologo','/machine:x64',f'/OUT:"{HERE / "lib" / (kind+".lib")}"']
    for p in objs:args.append('"'+str(HERE/'obj'/p.name if p.stem in changed else p)+'"')
    rsp=HERE/f'{kind}-lib.rsp';write(rsp,'\n'.join(args));steps.append(f'"{TOOL / "lib.exe"}" @"{rsp}"')
for name in ('authserver','worldserver'):
    t=BUILD/f'src/server/apps/{name}.dir/Release/{name}.tlog/link.command.1.tlog'
    cmd='\n'.join(t.read_text('utf-16').splitlines()[1:])
    for key,ext in [('OUT:','.exe'),('PDB:','.pdb'),('IMPLIB:','.lib')]:cmd=opt(cmd,key,HERE/'bin'/(name+ext))
    cmd=re.sub(r'(?i)(?:"[^"\r\n]*COMMON\.LIB"|\S*COMMON\.LIB)',lambda m:'"'+str(HERE/'lib/common.lib')+'"',cmd)
    cmd=re.sub(r'(?i)(?:"[^"\r\n]*MODULES\.LIB"|\S*MODULES\.LIB)',lambda m:'"'+str(HERE/'lib/modules.lib')+'"',cmd)
    rsp=HERE/(name+'-link.rsp');write(rsp,cmd)
    steps.append(f'cd /d "{BUILD / "src/server/apps"}"')
    steps.append(f'"{TOOL / "link.exe"}" @"{rsp}"')
for d in ('obj','lib','bin'):(HERE/d).mkdir(exist_ok=True)
batch='@echo off\ncall "'+str(VS/'VC/Auxiliary/Build/vcvars64.bat')+'" >nul\n'
for s in steps:batch+=s+'\nif errorlevel 1 exit /b %ERRORLEVEL%\n'
write(HERE/'compile.cmd',batch)
with (HERE/'build.log').open('wb') as f:r=subprocess.run(['cmd.exe','/d','/c',str(HERE/'compile.cmd')],stdout=f,stderr=subprocess.STDOUT,creationflags=0x08000000)
write(HERE/'build_result.json',json.dumps(dict(exit_code=r.returncode,source=inputs,executables={n:sha(HERE/'bin'/n) for n in ['authserver.exe','worldserver.exe'] if (HERE/'bin'/n).exists()}),indent=2))
print('BUILD EXIT',r.returncode,flush=True);raise SystemExit(r.returncode)
