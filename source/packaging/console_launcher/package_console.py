from build_console import *
import zipfile,datetime
sys.path.insert(0,str(ROOT/'migration_ops/module_work/wer_repack_100'))
import finish_release as old

def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()
def save(p,o):write(p,json.dumps(o,ensure_ascii=False,indent=2)+'\n')

test=json.loads((HERE/'test_result.json').read_text('utf-8'));assert test['pass_all']
assert not (OUT/'mysql/data').exists()
backup=ROOT/'migration_ops/history/wer_repack_100_launcher_r2';assert not backup.exists();backup.mkdir(parents=True)
manifest={line.split('  ',1)[1]:line.split('  ',1)[0] for line in (OUT/'SHA256SUMS.txt').read_text('utf-8').splitlines()}
retire=list(OUT.glob('*.bat'))+[OUT/p for p in ('scripts/wer-server.ps1','scripts/wer-entry.ps1','scripts/wer-menu.ps1','mysql/init_mysql.bat','LAUNCHER_SHA256SUMS.txt','실행기_보강_적용안내.md','docs/LAUNCHER_REVISION2.json')]
for p in retire:
    assert p.is_file() and p.resolve().is_relative_to(OUT.resolve())
    dest=backup/'files'/p.relative_to(OUT);dest.parent.mkdir(parents=True,exist_ok=True);shutil.move(str(p),str(dest))
for rel in ('QUICKSTART.md','먼저_읽어주세요.md','docs/RELEASE_NOTES.md','SHA256SUMS.txt'):
    dest=backup/'files'/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(OUT/rel,dest)
install(OUT)
for n in ('QUICKSTART.md','먼저_읽어주세요.md'):shutil.copy2(HERE/n,OUT/n)
shutil.copy2(HERE/'RELEASE_NOTES.md',OUT/'docs/RELEASE_NOTES.md')
save(OUT/'docs/CONSOLE_LAUNCHER.json',dict(release='WER REPACK_VER.1.0.0',launcher_revision=3,mode='three_visible_consoles',at=datetime.datetime.now().isoformat(),verification=test,automatic_restart=False,production_started=False,configs_changed=False,game_executables_changed=False))
pkg=OUT/'source/packaging/console_launcher';pkg.mkdir(parents=True)
for n in ('build_console.py','prepare-tail.ps1','package_console.py','test_console.py','QUICKSTART.md','먼저_읽어주세요.md','RELEASE_NOTES.md'):shutil.copy2(HERE/n,pkg/n)
write(pkg/'README.md','# 콘솔 실행기 제작 이력\n\n현재 1.0.0 실행방식은 루트 배치3개입니다. 이 폴더는 제작자 전용 재현/시험 코드이며 사용자가 실행하는 도구가 아닙니다. build_console.py는 이전 launcher_revision2의 검증된 DB 준비 함수를 재사용하되 감시와 메뉴를 포함하지 않습니다. test_console.py는 별도 QA 콘솔의 Ctrl+C 신호 시험이며 운영에서 실행하지 마세요.\n')
old.audit()
protected=[p for p in OUT.rglob('*') if p.is_file() and (p.relative_to(OUT).parts[0] in ('configs','dump','data','modules') or p.suffix in ('.exe','.dll') or p.relative_to(OUT).as_posix() in ('scripts/settings.json','source/WER_Source.zip'))]
for p in protected:assert sha(p)==manifest[p.relative_to(OUT).as_posix()],str(p)
assert sorted(p.name for p in OUT.glob('*.bat'))==['1_MYSQL.bat','2_AUTHSERVER.bat','3_WORLDSERVER.bat']
paths=[p for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.txt']
write(OUT/'SHA256SUMS.txt','\n'.join(sha(p)+'  '+p.relative_to(OUT).as_posix() for p in paths)+'\n')
print('THREE LAUNCHERS; PROTECTED FILES MATCH',len(protected),flush=True)
pending=OUT.parent/(OUT.name+'.console.pending.zip');assert not pending.exists()
with zipfile.ZipFile(pending,'w',zipfile.ZIP_DEFLATED,compresslevel=1,allowZip64=True) as z:
    for p in sorted(OUT.rglob('*')):
        if p.is_file():z.write(p,OUT.name+'/'+p.relative_to(OUT).as_posix())
print('ZIP WRITTEN; CRC CHECK',flush=True)
with zipfile.ZipFile(pending) as z:
    assert z.testzip() is None
    names=z.namelist();bats=[n for n in names if n.count('/')==1 and n.endswith('.bat')]
    assert len(bats)==3 and not any('/mysql/data/' in n for n in names)
final=OUT.parent/(OUT.name+'.zip');assert sha(final)=='f3eecb296dd49c4be77c85f53b7f50606f315f7b2f48d9d8daa8b96e38549139'
digest=sha(pending)
for p in [final,final.with_suffix('.zip.sha256'),OUT.parent/'WER REPACK_VER.1.0.0_실행기보강.zip',OUT.parent/'WER REPACK_VER.1.0.0_실행기보강.zip.sha256']:
    assert p.is_file() and p.resolve().parent==OUT.parent.resolve();shutil.move(str(p),str(backup/p.name))
pending.rename(final);write(final.with_suffix('.zip.sha256'),digest+'  '+final.name+'\n')
save(HERE/'artifact.json',dict(path=str(final),bytes=final.stat().st_size,sha256=digest,files=len(names),crc_pass=True,root_batches=bats,protected_files_unchanged=len(protected),previous_files_preserved=str(backup)))
print('FINAL CONSOLE RELEASE',digest,flush=True)
