from pathlib import Path
import zipfile,shutil,re,json,difflib,hashlib
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SRC=ROOT/'WOW_Legends_CoreLatest_Batch1_Test'
BUILD=ROOT/'migration_ops/build/core-latest-batch1-vs2022'
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
OUT=ROOT/'90_릴리즈/WER REPACK_VER.1.1.0'
BASE=ROOT/'90_릴리즈/WER REPACK_VER.1.0.0.zip'
QA=HERE/'qa/한글 WER 1.1.0'
def write(p,s):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,encoding='utf-8')
def save(p,s):write(p,json.dumps(s,ensure_ascii=False,indent=2)+'\n')
brand=re.compile(r'WOW[ _-]+LEGENDS|와우\s*레전[드트]|와우\s*래전드',re.I)
def comments(s):
    lines=[]
    for l in s.splitlines(keepends=True):
        if l.lstrip().startswith('#'):
            # Technical option names/file paths remain the actual supported identifiers.
            l=re.sub(r'\bWER\.(?=[A-Z])','WowLegends.',l)
            l=l.replace('mod-WER','mod-wowlegends').replace('mod_WER','mod_wowlegends')
            l=brand.sub('WER',l)
            l=l.replace('https://wow-legends.eu','선택한 API 공급자 사이트')
            l=l.replace('호스팅되는 WER AI로 별도 설정 없이 바로 동작합니다:', '외부 LLM은 본인 공급자의 주소·모델·API 키를 설정해야 합니다:')
            l=l.replace('아래 AiChat.ApiKey에 붙여 넣으면 끝입니다 — 모델 설정은 필요 없습니다.', '아래 AiChat.ApiKey를 입력하고 Provider/ApiUrl/Model이 공급자와 일치하는지 확인하세요.')
            l=l.replace('호스팅 WER', '호스팅 제공자')
        lines.append(l)
    return ''.join(lines)
def stage():
    assert not OUT.exists();assert sha(BASE)=='9ca19c9675ebed00f766bc28b627cc43b4c7dfc39f8e84e3d6c2c8be1cf8583b'
    OUT.mkdir(parents=True)
    with zipfile.ZipFile(BASE) as z:
        for n in z.namelist():
            rel=Path(n).relative_to('WER REPACK_VER.1.0.0')
            assert '..' not in rel.parts
            if not n.endswith('/'):
                p=OUT/rel;p.parent.mkdir(parents=True,exist_ok=True)
                with z.open(n) as a,p.open('wb') as b:shutil.copyfileobj(a,b)
    for n in ('worldserver.exe','authserver.exe'):shutil.copy2(HERE/'bin'/n,OUT/n)
    changes=[]
    for p in (OUT/'configs').rglob('*.conf*'):
        before=p.read_text('utf-8');after=comments(before)
        # Preserve active keys/values byte-for-byte; comments are presentation only.
        assert [l for l in before.splitlines() if not l.lstrip().startswith('#')]==[l for l in after.splitlines() if not l.lstrip().startswith('#')]
        if after!=before:write(p,after);changes.append(p.relative_to(OUT).as_posix())
    for p in list(OUT.glob('*.bat'))+list(OUT.glob('*.md')):
        text=p.read_text('utf-8').replace('1.0.0','1.1.0')
        text=brand.sub('WER',text)
        write(p,text)
    notes=OUT/'docs/RELEASE_NOTES.md'
    # Keep previous content/license history as provenance, with current corrections first.
    write(notes,'# WER REPACK VER1.1.0 — 제작일 2026-09-21\n\n한국 에뮬레이터 연구소 구성, 와우 리치왕3.3.5a(12340). 로그인/월드 배너 및 UTF8 콘솔 수정. 실행은 기존처럼 콘솔3개. 내부 WowLegends.* 설정키/모듈명/원 저작권과 출처는 유지합니다. 아래는 기존 콘텐츠/제한의 제작 이력이며 이전 실행기 설명은 루트 QUICKSTART를 따릅니다.\n\n'+notes.read_text('utf-8'))
    banner='한국 에뮬레이터 연구소'
    # Brand only initial realm and greeting text. No users/characters imported.
    p=OUT/'dump/wl_auth.sql';s=p.read_text('utf-8');count=s.count('와우 에뮬레이터 연구소');assert count>0
    write(p,s.replace('와우 에뮬레이터 연구소',banner))
    p=OUT/'scripts/wer-console-prepare.ps1'
    s=p.read_text('utf-8-sig').replace('1.0.0','1.1.0');p.write_text(s,encoding='utf-8-sig')
    # Updated source archive retains licenses and replaces only the reviewed files/comments.
    oldzip=OUT/'source/WER_Source.zip';tmp=HERE/'WER_Source_110.zip';manifest=[];patch=[]
    rels=json.loads((HERE/'build_result.json').read_text())['source']
    with zipfile.ZipFile(oldzip) as src,zipfile.ZipFile(tmp,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as dst:
        for n in src.namelist():
            data=src.read(n);rel=n.removeprefix('WER_Source/')
            if rel in rels:
                before=data.decode('utf-8-sig');after=(HERE/rel).read_text('utf-8');data=after.encode('utf-8')
                patch.extend(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
            elif n.endswith('.conf.dist'):data=comments(data.decode('utf-8-sig')).encode('utf-8')
            dst.writestr(n,data);manifest.append(dict(path=rel,sha256=hashlib.sha256(data).hexdigest(),size=len(data)))
    with zipfile.ZipFile(tmp) as z:assert z.testzip() is None
    shutil.copy2(tmp,oldzip);save(OUT/'source/SOURCE_MANIFEST.json',manifest)
    write(HERE/'reapply.patch',''.join(patch));shutil.copy2(HERE/'reapply.patch',OUT/'source/WER_1.1.0.patch')
    save(HERE/'stage_result.json',dict(config_comment_files=changes,realm_greeting_replacements=count,source_files=len(manifest),api_keys_from_clean_archive_only=True))
    print('1.1.0 RELEASE STAGED',flush=True)

def qa():
    assert not QA.exists()
    shutil.copytree(OUT,QA,ignore=shutil.ignore_patterns('source','전용애드온'))
    p=QA/'scripts/settings.json';c=json.loads(p.read_text('utf-8'));c.update(MySqlPort=57711,AuthPort=37411,WorldPort=59911,RealmAddress='127.0.0.1',BindIP='127.0.0.1',MinFreeCommitMB=6144);save(p,c)
    p=QA/'configs/modules/playerbots.conf';s=p.read_text('utf-8')
    for k,v in {'MinRandomBots':10,'MaxRandomBots':10,'RandomBotAccountCount':2,'AddClassAccountPoolSize':0,'RandomBotGuildCount':2,'RandomBotGuildSizeMax':3}.items():s=re.sub(r'(?m)^AiPlayerbot\.'+k+r'\s*=.*$',f'AiPlayerbot.{k} = {v}',s)
    write(p,s)
    print('QA STAGED',flush=True)

if __name__=='__main__':stage();qa()
