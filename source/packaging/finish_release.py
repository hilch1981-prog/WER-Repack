from build_release import *
import time

def secrets():
    found=[]
    for p in (RT/'run/configs').rglob('*.conf'):
        for k,v in re.findall(r'(?m)^([\w.]+)\s*=\s*(.*?)\s*$',p.read_text('utf-8-sig')):
            if 'DatabaseInfo' in k:
                parts=v.strip('"').split(';')
                if len(parts)>=5 and len(parts[3])>8:found.append(parts[3].encode())
            elif re.search(r'(ApiKey|AccessToken|AuthToken|MasterSecret)$',k,re.I) and len(v.strip('"'))>10:
                found.append(v.strip('"').encode())
    return list(set(found))

def contains_secret(p,private):
    tail=b''
    with p.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):
            block=tail+chunk
            if any(s in block for s in private):return True
            tail=block[-1024:]
    return False

def docs():
    for n,d in [('QUICKSTART.md','QUICKSTART.md'),('RELEASE_NOTES.md','docs/RELEASE_NOTES.md'),('wer-server.ps1','scripts/wer-server.ps1'),('build-source.ps1','source/build-source.ps1')]:copy(HERE/n,OUT/d)
    write(OUT/'_Status.bat','@echo off\ncd /d "%~dp0"\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\\wer-server.ps1" status\npause\n')
    write(OUT/'source/README.md',"""# 배포 실행파일에 대응하는 전체 소스

`WER_Source.zip`을 이 폴더에 풀면 WER_Source/src, modules, deps, data/sql, CMakeLists.txt가 생깁니다. 운영 소스의 현재 변경을 포함한 소스 스냅샷이며 pristine upstream이 아닙니다. SOURCE_MANIFEST.json은 파일별 SHA256입니다.

빌드 환경: Visual Studio 2022 MSVC 14.44.35207 x64 C++/Windows SDK, CMake, Boost 1.84.0 msvc14.3, OpenSSL 4.x(배포 DLL 기준), MySQL 8.4.9 개발 헤더/라이브러리. 운영 PC 절대 경로를 요구하지 않도록 build-source.ps1에서 각 SDK 경로를 인수로 받습니다. PowerShell에서 도움말/인수를 확인하여 실행하세요. 단일 컴파일 프로세스로 제한합니다.

OpenSSL SDK를 3.x로 바꿔 빌드하면 배포 DLL도 그 ABI에 맞게 다시 구성해야 합니다. MySQL의 libmysql.dll은 자체 OpenSSL 3 DLL도 필요하므로 현재 패키지에는 두 ABI가 함께 있습니다. DLL을 이름만 바꿔 대체하면 안 됩니다.

코어 기준 SHA 06234df3d5ab26c93f4f1f06f3edb828b73ecd3c와 WER/Legends 수정 통합본. `_assemble_report.json`은 초기 조립 출처이며 이후 수정의 파일 해시는 SOURCE_MANIFEST.json을 기준으로 합니다. 기본 upstream HEAD만 checkout해서 이 배포판과 같다고 간주하지 마세요.

독립 Ollama 모듈은 소스 보존만 하고 빌드에서 제외합니다. Individual Progression 신규 통합/미완성 후보는 포함하지 않았습니다. 원 저작자 및 모든 LICENSE 파일을 유지합니다.

소스 ZIP은 실행 로그, DB 인증 파일, 운영 설정 `.conf`, 백업/캐시/개인 IDE 자료를 포함하지 않습니다. `.conf.dist`는 공개 기본 템플릿이며 현재 운영 옵션은 별도 configs 폴더에 있습니다. 데이터베이스는 소스 초기 SQL보다 repack dump의 적용 완료본을 사용하세요.
""")
    write(OUT/'licenses/THIRD_PARTY.md',"""# 제3자 구성요소

- AzerothCore/Playerbots/모듈: 원 저작권은 루트 및 source ZIP, modules 각 LICENSE에 유지. 대부분 GPL 계열이며 각 파일의 조건이 우선.
- WOW Legends 수정 및 기존 원 저작권: modules/mod-wowlegends/LICENSE 및 소스 헤더 유지.
- MySQL Community 8.4.9 GPL 및 추가 조건: mysql/LICENSE, mysql/README. portable bin/lib/share 원본을 재배포.
- OpenSSL: OpenSSL-4-LICENSE.txt, MySQL 번들 OpenSSL 3의 고지는 mysql/LICENSE에 포함.
- Microsoft Visual C++ runtime DLL: Microsoft.VC143.CRT x64 재배포 파일. Microsoft 소유이며 오픈소스 GPL 코드로 재라이선스하지 않음. Windows x64 전용.
- 게임 데이터와 WoW 클라이언트의 상표/저작권은 해당 권리자 소유. 클라이언트 본체 미포함. 본 패키지는 공식 Blizzard/AzerothCore 배포물 아님.
""")
    loader=ROOT/'migration_ops/build/core-latest-batch1-vs2022/modules/gen_scriptloader/static/ModulesLoader.cpp'
    copy(loader,OUT/'docs/ModulesLoader.cpp')
    names=re.findall(r'^void Add(\w+)Scripts\(\);',loader.read_text('utf-8'),re.M)
    save(OUT/'docs/BUILD_ID.json',dict(release='WER REPACK_VER.1.0.0',date='2026-09-21',world_sha256=sha(OUT/'worldserver.exe'),auth_sha256=sha(OUT/'authserver.exe'),registered_modules=names,compiled_source=str(SRC.name),core_base='06234df3d5ab26c93f4f1f06f3edb828b73ecd3c',uncommitted_local_modifications_included=True))
    # Record exact actual runtime dependency versions and hashes without original operator secrets.
    files=[p for p in OUT.glob('*.dll')]+list((OUT/'mysql/bin').glob('*.dll'))
    save(OUT/'docs/RUNTIME_LIBRARIES.json',[dict(file=str(p.relative_to(OUT)),sha256=sha(p)) for p in files])
    print('DOCS DONE',flush=True)

def source():
    private=secrets(); manifest=[]
    exclude={'.git','.agents','.claude','.vscode','__pycache__','node_modules'}
    out=OUT/'source/WER_Source.zip';out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED,compresslevel=1,allowZip64=True) as z:
        for p in sorted(SRC.rglob('*')):
            if not p.is_file():continue
            rel=p.relative_to(SRC)
            if any(n in exclude for n in rel.parts) or rel.parts[0] in ('var',):continue
            if p.suffix.lower() in ('.exe','.dll','.pdb','.obj','.log','.bak','.pyc','.conf') or '.before-' in p.name:continue
            redacted=False
            if contains_secret(p,private):
                if not p.name.endswith('.conf.dist'):raise RuntimeError('Private credential in source file '+str(rel))
                data=p.read_bytes()
                for key in private:data=data.replace(key,b'')
                z.writestr('WER_Source/'+rel.as_posix(),data)
                digest=hashlib.sha256(data).hexdigest();size=len(data);redacted=True
            else:
                z.write(p,'WER_Source/'+rel.as_posix());digest=sha(p);size=p.stat().st_size
            manifest.append(dict(path=rel.as_posix(),sha256=digest,size=size,secret_redacted=redacted))
    save(OUT/'source/SOURCE_MANIFEST.json',manifest)
    with zipfile.ZipFile(out) as z:assert z.testzip() is None
    print('SOURCE ZIP VERIFIED',len(manifest),'files',out.stat().st_size,flush=True)

def audit():
    private=secrets(); scanned=0
    for p in OUT.rglob('*'):
        if not p.is_file():continue
        if p.suffix.lower() in ('.sql','.conf','.ini','.cnf','.md','.json','.ps1','.bat','.txt'):
            if contains_secret(p,private):raise RuntimeError('Private credential in '+str(p.relative_to(OUT)))
            scanned+=1
    for p in (OUT/'configs').rglob('*.conf'):
        content=p.read_text('utf-8')
        assert not re.search(r'(?m)^[^#\r\n]*[EC]:[/\\]',content),str(p)
        # Public snapshot: reject hardcoded Radmin hosts; do not publish the producer's old IP.
        assert '57598' not in content and not re.search(r'\b26\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', content),str(p)
    tables={}
    for db in ('wl_auth','wl_characters','wl_playerbots'):
        data=(OUT/'dump'/f'{db}.sql').read_text('utf-8')
        tables[db]=sorted(set(re.findall(r'INSERT INTO `([^`]+)`',data)))
    assert 'characters' not in tables['wl_characters']
    assert 'guild' not in tables['wl_characters'] and 'guild_house' not in tables['wl_characters']
    assert 'item_instance' not in tables['wl_characters'] and 'mail' not in tables['wl_characters']
    assert 'playerbots_random_bots' not in tables['wl_playerbots']
    assert 'playerbots_account_type' not in tables['wl_playerbots']
    assert not (OUT/'mysql/data').exists()
    assert (OUT/'configs/modules/mod_wowlegends.conf').read_text('utf-8').find('WowLegends.AiChat.ApiKey = ""')>=0
    report=dict(at=datetime.datetime.now().isoformat(),private_credentials_found=0,scanned_text_files=scanned,inserted_tables=tables,pristine_mysql_datadir=True,production_changed=False)
    save(HERE/'privacy_audit.json',report);save(OUT/'docs/PRIVACY_AUDIT.json',report)
    print('PRIVACY AUDIT PASS',scanned,'files',flush=True)

def archive():
    hashes=[]
    for p in sorted(OUT.rglob('*')):
        if p.is_file() and p.name!='SHA256SUMS.txt':hashes.append(sha(p)+'  '+p.relative_to(OUT).as_posix())
    write(OUT/'SHA256SUMS.txt','\n'.join(hashes)+'\n')
    archive=OUT.with_suffix('.0.zip') if False else OUT.parent/(OUT.name+'.zip')
    assert not archive.exists(),'Do not overwrite release archive'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=1,allowZip64=True) as z:
        for p in sorted(OUT.rglob('*')):
            if p.is_file():z.write(p,OUT.name+'/'+p.relative_to(OUT).as_posix())
    print('ZIP WRITTEN; CHECKING CRC',flush=True)
    with zipfile.ZipFile(archive) as z:assert z.testzip() is None
    digest=sha(archive);write(archive.with_suffix('.zip.sha256'),digest+'  '+archive.name+'\n')
    save(HERE/'artifact.json',dict(path=str(archive),bytes=archive.stat().st_size,sha256=digest,crc_pass=True,files=len(hashes)+1))
    print('FINAL ARCHIVE VERIFIED',archive.stat().st_size,digest,flush=True)

def verification():
    life=json.loads((HERE/'lifecycle_result.json').read_text('utf-8'));assert life['pass_all']
    qa=json.loads((HERE/'qa_after_recovery.json').read_text('utf-8'))
    assert qa['realm'].split('\t')[1]=='0' and qa['admin_password_verified']
    state=json.loads((ROOT/'migration_ops/runtime_watchdog/status.json').read_text('utf-8'))
    assert state['state']=='running' and state['session']=='20260921_024413_259615' and state['generation']==1
    assert sha(RT/'run/worldserver.exe')==sha(OUT/'worldserver.exe')
    assert sha(RT/'run/authserver.exe')==sha(OUT/'authserver.exe')
    data=dict(at=datetime.datetime.now().isoformat(),release='WER REPACK_VER.1.0.0',
      clean_database_import=True,first_start_bot_creation=True,qa=qa,lifecycle=life,
      release_target_online_bots=2000,qa_target_online_bots=10,
      guild_policy=dict(max_guilds=40,max_members_per_guild=25,all_bots_forced=False,production_config_changed=False),
      production_same_watchdog_session=True,production_same_executables=True,
      external_api_key_included=False,external_api_network_call_tested=False,
      actual_game_client_login_tested=False,external_pc_tested=False,full_2000_bot_load_tested=False,
      known_startup_issues=['items 45280/46104 duration mismatch','gobject_challenge_modes script missing; GO254605'],
      transient_qa_issues_fixed=['ASCII installation path guard','PowerShell5.1 UTF8 SQL input','auth/world startup order and offline flag recovery','ASCII CLI shutdown plus EOF'],
      additional_previous_runtime_issues_documented='docs/RELEASE_NOTES.md')
    save(OUT/'docs/VERIFICATION.json',data);save(HERE/'verification.json',data)
    copy(HERE/'build_release.py',OUT/'source/packaging/build_release.py')
    copy(HERE/'finish_release.py',OUT/'source/packaging/finish_release.py')
    write(OUT/'source/packaging/README.md','# 패키징 이력\n이 스크립트는 제작 당시 읽기 전용 DB 추출/비밀 제거/ZIP 검증 근거입니다. 제작자 전용 경로와 내부 읽기 helper를 참조하므로 다른 PC에서 실행하는 설치 도구가 아닙니다. 실제 사용자는 루트 start.bat, 재빌드는 ../build-source.ps1을 사용하세요.\n')
    print('VERIFICATION READY',flush=True)

if __name__=='__main__':{'docs':docs,'source':source,'audit':audit,'archive':archive,'verification':verification}[sys.argv[1]]()
