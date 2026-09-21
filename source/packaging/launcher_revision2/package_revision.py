"""Build reviewed launcher-only artifacts; never launches or changes production."""
from stage import *
import zipfile,datetime
sys.path.insert(0,str(ROOT/'migration_ops/module_work/wer_repack_100'))
import finish_release as old

def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()

def prepare():
    test=json.loads((HERE/'test_result.json').read_text('utf-8'));assert test['pass_all']
    staging=HERE/'package_staging';assert not staging.exists()
    install(staging)
    for n in ('QUICKSTART.md','먼저_읽어주세요.md'):shutil.copy2(HERE/n,staging/n)
    (staging/'docs').mkdir()
    shutil.copy2(HERE/'RELEASE_NOTES.md',staging/'docs/RELEASE_NOTES.md')
    save(staging/'docs/LAUNCHER_REVISION2.json',dict(release='WER REPACK_VER.1.0.0',launcher_revision=2,at=datetime.datetime.now().isoformat(),verification=test,game_executables_changed=False,release_configs_changed=False,production_started=False))
    write(staging/'실행기_보강_적용안내.md', '# 1.0.0 실행기 보강 적용\n\n신규 사용자는 전체 리팩 ZIP을 새 폴더에 풉니다. 기존 1.0.0 사용자는 먼저 기존 실행기로 게임 서버와 MySQL을 모두 정상 종료하고 감시가 꺼졌는지 확인합니다. 개별 EXE 강제 종료는 하지 마세요. 기존 scripts 폴더와 루트 배치를 백업한 뒤 이 ZIP의 파일을 기존 리팩 루트에 덮어씁니다. 새 00_WER_실행메뉴.bat에서 5번 상태 확인 후 4번으로 시작하세요. 이 보강 ZIP은 DB, configs, settings.json, EXE/DLL을 포함하지 않으므로 기존 데이터·포트·키는 유지됩니다. 전체 리팩 SHA256SUMS는 초기 배포 사본용이며 사용 중인 설정/DB를 검증하는 목록이 아닙니다. 보강 파일 자체 해시는 LAUNCHER_SHA256SUMS.txt를 사용하세요.\n')
    paths=[p for p in staging.rglob('*') if p.is_file()]
    write(staging/'LAUNCHER_SHA256SUMS.txt','\n'.join(sha(p)+'  '+p.relative_to(staging).as_posix() for p in sorted(paths))+'\n')
    backup=ROOT/'migration_ops/history/wer_repack_100_launcher_r1';backup.mkdir(parents=True,exist_ok=True)
    assert not (backup/'files').exists()
    for p in staging.rglob('*'):
        if not p.is_file():continue
        rel=p.relative_to(staging);dest=OUT/rel
        if dest.exists():
            saved=backup/'files'/rel;saved.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dest,saved)
        dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
    shutil.copy2(OUT/'SHA256SUMS.txt',backup/'SHA256SUMS.txt')
    pkg=OUT/'source/packaging/launcher_revision2';pkg.mkdir(parents=True)
    for n in ('stage.py','package_revision.py','test_components.py','wer-server.ps1','wer-entry.ps1','wer-menu.ps1','QUICKSTART.md','RELEASE_NOTES.md','먼저_읽어주세요.md'):shutil.copy2(HERE/n,pkg/n)
    write(pkg/'README.md','# 실행기 개정2 제작 이력\n\n상위 최초 제작 스크립트 이후 이 개정의 stage.py/install로 배치와 PowerShell 실행기를 보강했습니다. 제작자 작업 경로 전용이며 배포 PC에서 실행하는 설치 도구가 아닙니다. 일반 사용자는 루트 한글 메뉴를 사용하세요. test_components.py는 축소 로컬 진단 사본의 프로세스를 강제 종료하는 자동복구 시험을 포함하므로 운영 폴더에서 실행하지 마세요. 게임 소스 ZIP/EXE/DB/configs는 개정1과 동일합니다.\n')
    old.audit()
    original={line.split('  ',1)[1]:line.split('  ',1)[0] for line in (backup/'SHA256SUMS.txt').read_text('utf-8').splitlines()}
    protected=[p for p in OUT.rglob('*') if p.is_file() and (p.relative_to(OUT).parts[0] in ('configs','dump','data','modules') or p.suffix in ('.exe','.dll') or p.relative_to(OUT).as_posix() in ('scripts/settings.json','source/WER_Source.zip'))]
    for p in protected:assert sha(p)==original[p.relative_to(OUT).as_posix()],str(p)
    save(HERE/'unchanged_content.json',dict(protected_files=len(protected),all_match_original=True))
    full=[p for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.txt']
    write(OUT/'SHA256SUMS.txt','\n'.join(sha(p)+'  '+p.relative_to(OUT).as_posix() for p in full)+'\n')
    print('STAGED; PRIVACY AND UNCHANGED CONTENT PASS',len(protected),flush=True)
    small=OUT.parent/'WER REPACK_VER.1.0.0_실행기보강.zip';assert not small.exists()
    with zipfile.ZipFile(small,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for p in sorted(staging.rglob('*')):
            if p.is_file():z.write(p,p.relative_to(staging).as_posix())
    with zipfile.ZipFile(small) as z:assert z.testzip() is None
    write(small.with_suffix('.zip.sha256'),sha(small)+'  '+small.name+'\n')
    pending=OUT.parent/(OUT.name+'.revision2.pending.zip');assert not pending.exists()
    with zipfile.ZipFile(pending,'w',zipfile.ZIP_DEFLATED,compresslevel=1,allowZip64=True) as z:
        for p in sorted(OUT.rglob('*')):
            if p.is_file():z.write(p,OUT.name+'/'+p.relative_to(OUT).as_posix())
    print('FULL ZIP WRITTEN; CRC CHECK',flush=True)
    with zipfile.ZipFile(pending) as z:
        assert z.testzip() is None
        names=z.namelist();assert OUT.name+'/02_로그인서버_시작.bat' in names
        assert not any('/mysql/data/' in n for n in names)
    final=OUT.parent/(OUT.name+'.zip');old_hash=sha(final)
    assert old_hash=='666d193dd66f482c3399d7f4b04e9f74ac94d7ef29320cbdee95d2e763a5da46'
    digest=sha(pending)
    # All exact targets reside under MAKE; archive old artifacts recoverably, no deletion.
    assert not (backup/final.name).exists()
    shutil.move(str(final),str(backup/final.name))
    shutil.move(str(final.with_suffix('.zip.sha256')),str(backup/(final.name+'.sha256')))
    pending.rename(final)
    write(final.with_suffix('.zip.sha256'),digest+'  '+final.name+'\n')
    save(HERE/'artifact.json',dict(release='WER REPACK_VER.1.0.0',launcher_revision=2,path=str(final),bytes=final.stat().st_size,sha256=digest,files=len(names),crc_pass=True,previous_zip_preserved=str(backup/final.name),launcher_patch=dict(path=str(small),bytes=small.stat().st_size,sha256=sha(small))))
    print('FINAL REPLACED',digest,flush=True)

if __name__=='__main__':prepare()
