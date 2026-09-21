from stage_release import *
import sys,datetime

qa_result=json.loads((HERE/'qa_result.json').read_text('utf-8'));assert qa_result['pass_all']
assert json.loads((HERE/'redirect_test.json').read_text('utf-8'))['pass_all']
build=json.loads((HERE/'build_result.json').read_text('utf-8'));assert build['exit_code']==0
for n,digest in build['executables'].items():assert sha(OUT/n)==digest
assert sha(BASE)=='9ca19c9675ebed00f766bc28b627cc43b4c7dfc39f8e84e3d6c2c8be1cf8583b'

for p in (OUT/'configs').rglob('*.conf*'):write(p,comments(p.read_text('utf-8')))
# The three patches are compiled, not binary string replacements. Refresh corresponding source.
srczip=OUT/'source/WER_Source.zip';fresh=HERE/'WER_Source_final.zip';manifest=[];patch=[]
with zipfile.ZipFile(srczip) as before,zipfile.ZipFile(fresh,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as after:
    for name in before.namelist():
        data=before.read(name);rel=name.removeprefix('WER_Source/')
        if rel in build['source']:
            data=(HERE/rel).read_bytes()
            assert sha(SRC/rel)==build['source'][rel]['before'],'Baseline source changed unexpectedly'
            patch.extend(difflib.unified_diff((SRC/rel).read_text('utf-8').splitlines(True),data.decode('utf-8').splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
        elif name.endswith('.conf.dist'):data=comments(data.decode('utf-8-sig')).encode('utf-8')
        after.writestr(name,data);manifest.append(dict(path=rel,sha256=hashlib.sha256(data).hexdigest(),size=len(data)))
with zipfile.ZipFile(fresh) as z:assert z.testzip() is None
shutil.copy2(fresh,srczip);save(OUT/'source/SOURCE_MANIFEST.json',manifest)
write(HERE/'reapply.patch',''.join(patch));shutil.copy2(HERE/'reapply.patch',OUT/'source/WER_1.1.0.patch')

bid=json.loads((OUT/'docs/BUILD_ID.json').read_text('utf-8'))
bid.update(release='WER REPACK_VER.1.1.0',date='2026-09-21',producer='한국 에뮬레이터 연구소',game_version='3.3.5a (12340)',world_sha256=build['executables']['worldserver.exe'],auth_sha256=build['executables']['authserver.exe'],source_patch='source/WER_1.1.0.patch',isolated_incremental_link=True)
save(OUT/'docs/BUILD_ID.json',bid)
shutil.copy2(OUT/'docs/VERIFICATION.json',OUT/'docs/VERIFICATION_1.0.0_HISTORY.json')
verify=dict(release='WER REPACK_VER.1.1.0',production_date='2026-09-21',build=build,console_qa=qa_result,redirect_qa=json.loads((HERE/'redirect_test.json').read_text()),three_visible_consoles=True,gameplay_behavior_changed=False,game_client_login_tested=False,full_2000_bot_load_tested=False,external_api_call_tested=False,old_100_user_database_changed=False,known_content_issues='기존 아이템45280/46104 지속시간 플래그 및 gobject_challenge_modes 누락은 별도 미해결')
save(OUT/'docs/VERIFICATION.json',verify);save(OUT/'docs/CONSOLE_LAUNCHER.json',verify)
save(OUT/'docs/BRANDING_CHANGES.json',dict(date='2026-09-21',display_brand='한국 에뮬레이터 연구소 / WER REPACK VER1.1.0',realm_name='한국 에뮬레이터 연구소',old_config_keys_preserved=True,original_copyright_licenses_preserved=True,active_config_values_changed=False,source_patch=build['source'],config_comments=json.loads((HERE/'stage_result.json').read_text())['config_comment_files']))

for name in ('QUICKSTART.md','먼저_읽어주세요.md'):
    p=OUT/name;text=p.read_text('utf-8');first,rest=text.split('\n',1)
    write(p,first+'\n\n**와우 리치왕 3.3.5a · 한국 에뮬레이터 연구소 구성 · WER REPACK VER1.1.0 · 제작일 2026-09-21**\n'+rest)
p=OUT/'source/README.md';write(p,'# WER REPACK VER1.1.0 소스\n\n2026-09-21 WER_1.1.0.patch의 배너/콘솔Unicode/로그접두사3파일을 적용한 전체 소스입니다. 원 저작권/라이선스/내부 키·모듈명은 보존합니다. 배포 제작자는 한국 에뮬레이터 연구소이며 원 코어/모듈 저작자를 대체하는 뜻이 아닙니다.\n\n'+p.read_text('utf-8'))
pack=OUT/'source/packaging/wer110';pack.mkdir(parents=True)
for n in ('build_brand.py','stage_release.py','finish_release.py','test_redirect.py','test_utf8_console.py','encoding_probe.cpp','reapply.patch'):shutil.copy2(HERE/n,pack/n)
write(pack/'README.md','# 제작/시험 이력\n\n제작자 환경의 기존 검증된 obj/lib를 읽기 전용 재사용하고 후보3파일만 별도 컴파일하여 새 EXE로 링크했습니다. 이 폴더의 스크립트는 사용자 설치 도구가 아닙니다. 다른 PC에서의 전체 빌드는 source/build-source.ps1 및 동봉 전체소스를 사용하세요. 테스트 스크립트는 분리QA에서만 사용하며 서버 콘솔 Ctrl+C를 발생시킵니다.\n')

# Audit against the clean archive, never copy the user's now-initialized 1.0.0 directory.
noncomment_changes=[]
with zipfile.ZipFile(BASE) as base:
    for p in (OUT/'configs').rglob('*.conf*'):
        old=base.read('WER REPACK_VER.1.0.0/'+p.relative_to(OUT).as_posix()).decode('utf-8')
        new=p.read_text('utf-8')
        assert [l for l in old.splitlines() if not l.lstrip().startswith('#')]==[l for l in new.splitlines() if not l.lstrip().startswith('#')]
        assert not re.search(r'WOW\s+LEGENDS|wow-legends.eu',new,re.I)
    for name in ('dump/wl_characters.sql','dump/wl_playerbots.sql','dump/wl_world.sql'):
        assert hashlib.sha256(base.read('WER REPACK_VER.1.0.0/'+name)).hexdigest()==sha(OUT/name)
assert not (OUT/'mysql/data').exists() and not (OUT/'mysql/client.cnf').exists()
assert re.search(r'^WowLegends.AiChat.ApiKey\s*=\s*""\s*$',(OUT/'configs/modules/mod_wowlegends.conf').read_text('utf-8'),re.M)
# Obtain prior audit helpers without running their packaging entry point.
import importlib.util
oldpath=ROOT/'migration_ops/module_work/wer_repack_100'
sys.path.insert(0,str(oldpath))
spec=importlib.util.spec_from_file_location('wer100_privacy',oldpath/'finish_release.py');old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
private=old.secrets();scanned=0
for p in OUT.rglob('*'):
    if p.is_file() and p.suffix.lower() in ('.sql','.conf','.ini','.cnf','.md','.json','.ps1','.bat','.txt','.py','.patch'):
        assert not old.contains_secret(p,private),p.relative_to(OUT)
        scanned+=1
audit=dict(at=datetime.datetime.now().isoformat(),private_credentials_found=0,text_files=scanned,pristine_database=True,configs_active_values_unchanged=True,characters_playerbots_world_dumps_unchanged=True,auth_realm_brand_only=True)
save(OUT/'docs/PRIVACY_AUDIT.json',audit);save(HERE/'privacy_audit.json',audit)
files=[p for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.txt']
write(OUT/'SHA256SUMS.txt','\n'.join(sha(p)+'  '+p.relative_to(OUT).as_posix() for p in files)+'\n')
print('METADATA SOURCE AND PRIVACY PASS',flush=True)
archive=OUT.parent/(OUT.name+'.zip');assert not archive.exists()
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=1,allowZip64=True) as z:
    for p in sorted(OUT.rglob('*')):
        if p.is_file():z.write(p,OUT.name+'/'+p.relative_to(OUT).as_posix())
print('ZIP WRITTEN; CRC CHECK',flush=True)
with zipfile.ZipFile(archive) as z:assert z.testzip() is None;count=len(z.namelist())
digest=sha(archive);write(archive.with_suffix('.zip.sha256'),digest+'  '+archive.name+'\n')
save(HERE/'artifact.json',dict(path=str(archive),bytes=archive.stat().st_size,sha256=digest,files=count,crc_pass=True))
print('1.1.0 FINAL',digest,flush=True)
