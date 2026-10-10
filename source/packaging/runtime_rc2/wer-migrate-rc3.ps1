# Called after the launcher's owned-process/listener/datadir guard is installed.
# Sql must be the verified caller's guarded function; never open another database here.
function Get-WerRc3FileHash([string]$Path) {
    # Do not depend on Get-FileHash module autoload across PS7 -> Windows PS5.1 launches.
    $hash=[Security.Cryptography.SHA256]::Create()
    $stream=[IO.File]::OpenRead($Path)
    try{return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','')}
    finally{$stream.Dispose();$hash.Dispose()}
}
function Invoke-WerRc3Migration {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$MigrationDirectory,
          [Parameter(Mandatory=$true)][string]$BackupDirectory)
    $encoding=New-Object Text.UTF8Encoding($false)
    $files=@('01_equipment_notice_koKR.sql','02_duration_custom_flags.sql','03_eye_missing_path_idle.sql')
    $expectedHashes=@(
        '0A600C16CD3DCA590255B0AE2234271DE621454C52AAAA0924FAF68F033C745D',
        '68D9C92B86CFAF9A83434FBABE199AD1899368AB8D2C312B3D657C9C8ABC7D1D',
        '76D74F5F5C6D5312D4BD2699228058E1B4E407F41C3961B1D8149E42F048B800'
    )
    $sqlParts=@();$hashes=@()
    foreach($file in $files){
        $path=Join-Path $MigrationDirectory $file
        if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw ('RC3 migration file missing: '+$file)}
        $fileHash=Get-WerRc3FileHash $path
        if($fileHash -ne $expectedHashes[$sqlParts.Count]){throw ('RC3 migration hash mismatch: '+$file)}
        $sqlParts+=([IO.File]::ReadAllText($path,$encoding))
        $hashes+=@{file=$file;sha256=$fileHash}
    }
    $lockKey=[IO.Path]::GetFullPath($BackupDirectory).ToLowerInvariant()
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$lockName='Local\WER-Rc3-'+[BitConverter]::ToString($sha.ComputeHash($encoding.GetBytes($lockKey))).Replace('-','').Substring(0,24)}finally{$sha.Dispose()}
    $mutex=New-Object Threading.Mutex($false,$lockName)
    $locked=$false;$receipt=$null;$receiptPath=$null
    try{
        try{$locked=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
        if(!$locked){throw 'Another RC3 migration is already running for this runtime.'}
        $engines=[string](Sql "SELECT COUNT(*) FROM information_schema.tables WHERE ENGINE='InnoDB' AND ((TABLE_SCHEMA='wl_playerbots' AND TABLE_NAME='ai_playerbot_texts') OR (TABLE_SCHEMA='wl_world' AND TABLE_NAME IN ('item_template','creature')));")
        if($engines.Trim() -ne '3'){throw 'RC3 requires the three existing target tables to use InnoDB.'}
        $snapshotSql=@'
SELECT JSON_OBJECT(
 'equipment_texts', COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('id',id,'name',name,'text',text,'say_type',say_type,'reply_type',reply_type,'text_loc1',text_loc1,'text_loc2',text_loc2,'text_loc3',text_loc3,'text_loc4',text_loc4,'text_loc5',text_loc5,'text_loc6',text_loc6,'text_loc7',text_loc7,'text_loc8',text_loc8)) FROM wl_playerbots.ai_playerbot_texts WHERE name='wl_equip_item_notice'),JSON_ARRAY()),
 'items',COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('entry',entry,'flagsCustom',flagsCustom,'duration',duration)) FROM wl_world.item_template WHERE entry IN (45280,46104)),JSON_ARRAY()),
 'npc',COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('guid',guid,'id',id,'MovementType',MovementType)) FROM wl_world.creature WHERE guid=82897),JSON_ARRAY()));
'@
        $before=([string](Sql $snapshotSql))|ConvertFrom-Json
        [void][IO.Directory]::CreateDirectory($BackupDirectory)
        $receiptPath=Join-Path $BackupDirectory ('rc3-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)+'.json')
        $receipt=[ordered]@{release='v1.1.1-rc.3';started_kst=[DateTimeOffset]::Now.ToString('o');status='PENDING';files=$hashes;before=$before;after=$null;changes=$null}
        [IO.File]::WriteAllText($receiptPath,($receipt|ConvertTo-Json -Depth 12),$encoding)
        $batch="START TRANSACTION;`n"+$sqlParts[0]+"`nSET @wer_rc3_equipment_rows=ROW_COUNT();`nSET @wer_rc3_equipment_id=IF(@wer_rc3_equipment_rows=1,LAST_INSERT_ID(),NULL);`n"+$sqlParts[1]+"`nSET @wer_rc3_duration_rows=ROW_COUNT();`n"+$sqlParts[2]+"`nSET @wer_rc3_npc_rows=ROW_COUNT();`nCOMMIT;`nSELECT JSON_OBJECT('equipment_rows',@wer_rc3_equipment_rows,'equipment_inserted_id',@wer_rc3_equipment_id,'duration_rows',@wer_rc3_duration_rows,'npc_rows',@wer_rc3_npc_rows);"
        $changes=([string](Sql $batch))|ConvertFrom-Json
        $receipt.changes=$changes
        $receipt.after=([string](Sql $snapshotSql))|ConvertFrom-Json
        $receipt.status='APPLIED';$receipt.finished_kst=[DateTimeOffset]::Now.ToString('o')
        [IO.File]::WriteAllText($receiptPath,($receipt|ConvertTo-Json -Depth 12),$encoding)
        Write-Host ('[RC3 DB] Korean equipment text='+$changes.equipment_rows+', duration flags='+$changes.duration_rows+', NPC idle='+$changes.npc_rows)
        return [pscustomobject]@{status='APPLIED';receipt=$receiptPath;changes=$changes}
    }catch{
        if($receipt -and $receiptPath){
            $receipt.status='FAILED_OR_COMMIT_OUTCOME_UNKNOWN';$receipt.error=$_.Exception.Message
            [IO.File]::WriteAllText($receiptPath,($receipt|ConvertTo-Json -Depth 12),$encoding)
        }
        throw
    }finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
}
