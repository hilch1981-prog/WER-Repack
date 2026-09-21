"""Read-only production export -> clean portable WER release. Never changes production."""
from pathlib import Path
import datetime, hashlib, json, os, re, shutil, subprocess, sys, zipfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SRC=ROOT/'WOW_Legends_CoreLatest_Batch1_Test'
RT=ROOT/'migration_ops/tmp/core_latest_runtime_sjcyohie'
MYSQL=ROOT/'migration_ops/tmp/mysql_runtime_8_4_9'
OUT=ROOT/'90_릴리즈/WER REPACK_VER.1.0.0'
sys.path.insert(0,str(ROOT/'migration_ops/module_work/batch_update_20260921'))
from deploy_steps import client
sys.path.insert(0,str(ROOT/'migration_ops/deploy_defaults'))
from make_admin_account_sql import verifier

def write(p,s):
    p.parent.mkdir(parents=True,exist_ok=True); p.write_text(s,encoding='utf-8',newline='\n')
def save(p,obj): write(p,json.dumps(obj,ensure_ascii=False,indent=2)+'\n')
def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''): h.update(b)
    return h.hexdigest()
def copy(a,b): b.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(a,b)
def dump(db,tables,args=()):
    cmd=[str(MYSQL/'bin/mysqldump.exe'),'--defaults-extra-file='+str(RT/'client.cnf'),
         '--default-character-set=utf8mb4','--single-transaction','--skip-lock-tables','--no-tablespaces',
         '--set-gtid-purged=OFF','--hex-blob','--skip-dump-date',*args,db,*tables]
    p=OUT/'dump'/db
    with p.with_suffix('.part').open('wb') as f:
        r=subprocess.run(cmd,stdout=f,stderr=subprocess.PIPE,creationflags=0x08000000)
    if r.returncode: raise RuntimeError(r.stderr.decode(errors='replace')[:500])
    return p.with_suffix('.part')
def exports():
    keep={
      'wl_auth':['build_info','rbac_default_permissions','rbac_linked_permissions','rbac_permissions','motd','motd_localized','updates','updates_include'],
      'wl_characters':['active_arena_season','banned_addons','chat_filter','mail_server_template','mail_server_template_conditions','mail_server_template_items','playerbots_arena_team_names','playerbots_arena_team_names_locale','playerbots_guild_names','playerbots_guild_names_locale','playerbots_names','profanity_name','reserved_name','updates','updates_include'],
      'wl_playerbots':['ai_playerbot_texts','ai_playerbot_texts_chance','playerbots_bis_gear','playerbots_custom_strategy','playerbots_dungeon_suggestion_abbrevation','playerbots_dungeon_suggestion_definition','playerbots_dungeon_suggestion_strategy','playerbots_enchants','playerbots_rnditem_cache','playerbots_speech','playerbots_speech_probability','playerbots_travelnode','playerbots_travelnode_link','playerbots_travelnode_path','playerbots_weightscale_data','playerbots_weightscales','updates','updates_include','version_db_playerbots']}
    (OUT/'dump').mkdir(parents=True,exist_ok=True)
    for db,tables in keep.items():
        part=dump(db,[],['--no-data'])
        schema=re.sub(r' AUTO_INCREMENT=\d+','',part.read_text('utf-8'))
        target=OUT/'dump'/f'{db}.sql'; write(target,schema)
        part=dump(db,tables,['--no-create-info'])
        with target.open('ab') as f,part.open('rb') as source: shutil.copyfileobj(source,f)
        part.unlink()
        print('EXPORTED CLEAN',db,flush=True)
    # World contains static content, not account, guild ownership or character state.
    p=dump('wl_world',[]); p.replace(OUT/'dump/wl_world.sql')
    with (OUT/'dump/wl_auth.sql').open('a',encoding='utf-8') as f:
        f.write("\nINSERT INTO realmlist (id,name,address,localAddress,port,icon,flag,timezone,gamebuild) VALUES (1,'와우 에뮬레이터 연구소','127.0.0.1','127.0.0.1',59823,1,0,16,12340);\n")
        f.write((ROOT/'migration_ops/deploy_defaults/auth_admin_account.sql').read_text('utf-8'))
    save(HERE/'export_policy.json',keep)
    print('EXPORTED WORLD + ADMIN',flush=True)

def prepare():
    assert not OUT.exists(), 'Refuse to overwrite existing release'
    OUT.mkdir(parents=True)
    for n in ['authserver.exe','worldserver.exe','legacy.dll']: copy(RT/'run'/n,OUT/n)
    for n in ['libcrypto-4-x64.dll','libssl-4-x64.dll']: copy(Path('C:/Program Files/OpenSSL-Win64')/n,OUT/n)
    copy(ROOT/'migration_ops/build_dependencies/mysql-8.4.9/lib/libmysql.dll',OUT/'libmysql.dll')
    for n in ['libcrypto-3-x64.dll','libssl-3-x64.dll']: copy(MYSQL/'bin'/n,OUT/n)
    for d in ['bin','lib','share']: shutil.copytree(MYSQL/d,OUT/'mysql'/d)
    for n in ['LICENSE','README']:
        p=ROOT/'migration_ops/build_dependencies/mysql-8.4.9'/n
        if p.exists(): copy(p,OUT/'mysql'/n)
    copy(Path('C:/Program Files/OpenSSL-Win64/license.txt'),OUT/'licenses/OpenSSL-4-LICENSE.txt')
    copy(SRC/'LICENSE',OUT/'LICENSE')
    # VC runtime app-local redistribution; no dependency on operator PATH.
    redist=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC')
    crt=sorted(redist.glob('*/x64/Microsoft.VC143.CRT'))[-1]
    for p in crt.glob('*.dll'):
        copy(p,OUT/p.name); copy(p,OUT/'mysql/bin'/p.name)
    (OUT/'logs').mkdir(); (OUT/'scripts').mkdir()
    changes=[]
    for p in (RT/'run/configs').rglob('*.conf'):
        s=p.read_text('utf-8-sig'); lines=[]
        for line in s.splitlines():
            m=re.match(r'^([\w.]+)\s*=\s*(.*?)\s*$',line)
            if m:
                k,v=m.groups(); new=None; reason='portable release'
                if 'DatabaseInfo' in k:
                    db=v.strip('"').split(';')[-1]; new=f'"127.0.0.1;3306;wer;wer-local-only;{db}"'
                elif re.search(r'(ApiKey|AccessToken|AuthToken|MasterSecret|Password)$',k,re.I) and 'RandomPassword' not in k:
                    new='""'; reason='private credential removed'
                elif k=='SourceDirectory': new='"."'
                elif k=='MySQLExecutable': new='"mysql/bin/mysql.exe"'
                elif k=='Console.Enable': new='1'
                elif k=='AiPlayerbot.RandomBotRandomPassword': new='1'
                elif k=='AuctionHouseBot.Account': new='2'
                elif k=='AuctionHouseBot.GUID': new='1'
                if new is not None:
                    if v!=new: changes.append({'file':str(p.relative_to(RT/'run')),'key':k,'before':'[REDACTED]' if 'DatabaseInfo' in k or re.search(r'key|token|secret|password',k,re.I) else v,'after':new if 'DatabaseInfo' not in k else '[local release credentials]','reason':reason})
                    line=f'{k} = {new}'
            lines.append(line)
        write(OUT/'configs'/p.relative_to(RT/'run/configs'),'\n'.join(lines)+'\n')
    save(OUT/'docs/CONFIG_CHANGES.json',changes)
    save(OUT/'scripts/settings.json',dict(MySqlPort=3306,AuthPort=3724,WorldPort=59823,RealmAddress='auto',BindIP='0.0.0.0',DbPassword='wer-local-only',RestartDelaySeconds=15))
    # AH bot is created only on first installation. Random unknown SRP password, no GM permission.
    salt=os.urandom(32); v=verifier('WER_AHBOT',os.urandom(32).hex(),salt)
    write(OUT/'scripts/first_start_ahbot.sql',f"""SET NAMES utf8mb4;
INSERT INTO wl_auth.account (id,username,salt,verifier,expansion,email,reg_mail) VALUES (2,'WER_AHBOT',0x{salt.hex()},0x{v.hex()},2,'','');
INSERT INTO wl_auth.realmcharacters VALUES (1,2,1);
INSERT INTO wl_characters.characters (guid,account,name,race,class,gender,level,money,position_x,position_y,position_z,map,orientation,taximask,innTriggerId,health) VALUES (1,2,'경매관리인',1,1,0,80,100000000,-8833.38,628.62,94,0,1,'',0,100);
INSERT INTO wl_characters.character_homebind (guid,mapId,zoneId,posX,posY,posZ) VALUES (1,0,1519,-8833.38,628.62,94);
""")
    print('COPYING DATA',flush=True)
    shutil.copytree(RT/'run/data',OUT/'data')
    # Runtime SQL trees, no source build or historical backups in module directory.
    for mod in (SRC/'modules').iterdir():
        if mod.is_dir():
            for name in ['data','sql']:
                if (mod/name).is_dir(): shutil.copytree(mod/name,OUT/'modules'/mod.name/name)
            for p in mod.glob('*'):
                if p.is_file() and (p.name.lower().startswith(('license','readme'))): copy(p,OUT/'modules'/mod.name/p.name)
    addon=ROOT.parent/'REPACT/260912_WOW_Legends_Repack1.5.3/전용애드온'
    shutil.copytree(addon,OUT/'전용애드온')
    print('BASE PREPARED',flush=True)

if __name__=='__main__':
    {'prepare':prepare,'exports':exports}[sys.argv[1]]()
