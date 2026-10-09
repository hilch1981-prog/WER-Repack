param([ValidateSet('mysql','auth','world')][string]$Action)
$ErrorActionPreference='Stop'
$Root=Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $Root
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$Cfg=Get-Content -LiteralPath "$PSScriptRoot/settings.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Cfg.DbPassword -notmatch '^[a-zA-Z0-9_-]{12,80}$') { throw 'DbPassword는 영문·숫자·하이픈·밑줄 12~80자로 설정하세요.' }
$Utf8=New-Object Text.UTF8Encoding($false)
$script:StartedTemporaryMysql=$false
function WriteUtf8($Path,$Value) { [IO.File]::WriteAllText($Path,$Value,$Utf8) }
function PortOpen($Port) {
    $c=New-Object Net.Sockets.TcpClient
    try { $a=$c.BeginConnect('127.0.0.1',[int]$Port,$null,$null); if (!$a.AsyncWaitHandle.WaitOne(200)) {return $false}; $c.EndConnect($a); return $true } catch {return $false} finally {$c.Dispose()}
}
function Owned($Name) { ,@(Get-CimInstance Win32_Process -Filter "Name='$Name'" | Where-Object {$_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq [IO.Path]::GetFullPath((Join-Path $Root $(if($Name -eq 'mysqld.exe'){'mysql/bin/mysqld.exe'}else{$Name})))}) }
function Event($Message) { Add-Content -LiteralPath "$Root/logs/watchdog.log" -Encoding UTF8 -Value ("{0} {1}" -f (Get-Date -Format o),$Message) }
function InvokePrivateSql($Text,[switch]$Initial) {
    $i=New-Object Diagnostics.ProcessStartInfo
    $i.FileName="$Root/mysql/bin/mysql.exe"
    $defaults=if($Initial){'initial-client.cnf'}else{'client.cnf'}
    # Do not inherit another installation's global defaults or login-path credentials.
    $i.Arguments="--defaults-file=mysql/$defaults --no-login-paths --protocol=TCP --host=127.0.0.1 --port=$($Cfg.MySqlPort) --default-character-set=utf8mb4 --batch --raw --skip-column-names"
    $i.WorkingDirectory=$Root; $i.UseShellExecute=$false; $i.CreateNoWindow=$true
    $i.RedirectStandardInput=$true; $i.RedirectStandardOutput=$true; $i.RedirectStandardError=$true
    $i.StandardOutputEncoding=$Utf8; $i.StandardErrorEncoding=$Utf8
    $p=New-Object Diagnostics.Process; $p.StartInfo=$i; [void]$p.Start()
    $outTask=$p.StandardOutput.ReadToEndAsync(); $errTask=$p.StandardError.ReadToEndAsync()
    $bytes=$Utf8.GetBytes($Text+"`n"); $p.StandardInput.BaseStream.Write($bytes,0,$bytes.Length); $p.StandardInput.BaseStream.Flush(); $p.StandardInput.Close(); $p.WaitForExit()
    $out=$outTask.Result; $err=$errTask.Result; $code=$p.ExitCode; $p.Dispose()
    if($code -ne 0) {throw "MySQL 처리 실패 ($code): $err"}; return $out.Trim()
}
function GetNativeFileCodepage {
    if(-not ('WER.Repack.NativeCodepage' -as [type])){
        Add-Type -TypeDefinition 'using System.Runtime.InteropServices; namespace WER.Repack { public static class NativeCodepage { [DllImport("kernel32.dll")] public static extern uint GetACP(); } }'
    }
    return [int][WER.Repack.NativeCodepage]::GetACP()
}
function DecodeMysqlPath($Hex) {
    if(!$Hex.Length -or $Hex.Length % 2 -ne 0 -or $Hex -notmatch '\A[0-9A-Fa-f]+\z'){throw 'MySQL 경로 바이트 형식이 올바르지 않습니다.'}
    $bytes=New-Object byte[] ($Hex.Length/2)
    for($index=0;$index -lt $bytes.Length;$index++){$bytes[$index]=[Convert]::ToByte($Hex.Substring($index*2,2),16)}
    # Windows MySQL exposes native path bytes; do not misdecode Korean ACP bytes as UTF-8.
    $encoding=[Text.Encoding]::GetEncoding((GetNativeFileCodepage),[Text.EncoderFallback]::ExceptionFallback,[Text.DecoderFallback]::ExceptionFallback)
    return $encoding.GetString($bytes)
}
function AssertPrivateMysql([switch]$Initial) {
    $processes=Owned 'mysqld.exe'
    $listeners=@(Get-NetTCPConnection -State Listen -LocalPort ([int]$Cfg.MySqlPort) -ErrorAction SilentlyContinue)
    if(!$processes.Count -or !$listeners.Count){throw '이 리팩의 MySQL 프로세스/포트가 확인되지 않습니다.'}
    foreach($listener in $listeners){
        if($listener.OwningProcess -notin $processes.ProcessId -or $listener.LocalAddress -notin @('127.0.0.1','::1')){throw 'MySQL 포트가 이 리팩의 로컬 전용 프로세스와 다릅니다. SQL을 실행하지 않습니다.'}
    }
    $identity=InvokePrivateSql 'SELECT HEX(@@datadir),@@port;' -Initial:$Initial
    $fields=$identity.Split("`t")
    if($fields.Count -ne 2){throw 'MySQL 데이터 경로/포트 확인에 실패했습니다.'}
    $expected=[IO.Path]::GetFullPath((Join-Path $Root 'mysql/data')).TrimEnd([char]'\')
    $actual=[IO.Path]::GetFullPath((DecodeMysqlPath $fields[0]).Replace('/','\')).TrimEnd([char]'\')
    if(![string]::Equals($actual,$expected,[StringComparison]::OrdinalIgnoreCase) -or $fields[1] -ne [string]$Cfg.MySqlPort){throw 'MySQL 데이터 폴더/포트가 이 리팩과 다릅니다. SQL을 실행하지 않습니다.'}
}
function Sql($Text,[switch]$Initial) {
    AssertPrivateMysql -Initial:$Initial
    return InvokePrivateSql $Text -Initial:$Initial
}
function SetConf($Path,$Key,$Value) {
    $s=[IO.File]::ReadAllText($Path,$Utf8)
    $pattern='(?m)^'+[regex]::Escape($Key)+'\s*=.*$'
    if($s -match $pattern){$s=[regex]::Replace($s,$pattern,($Key+' = '+$Value))}else{$s+="`n$Key = $Value`n"}
    WriteUtf8 $Path $s
}
function Configure {
    foreach($f in Get-ChildItem "$Root/configs" -Filter *.conf -Recurse) {
        $s=[IO.File]::ReadAllText($f.FullName,$Utf8)
        $s=[regex]::Replace($s,'(?m)^([\w.]*DatabaseInfo)\s*=\s*"[^"\r\n]*;(wl_\w+)"\s*$',{param($m) $m.Groups[1].Value+' = "127.0.0.1;'+$Cfg.MySqlPort+';wer;'+$Cfg.DbPassword+';'+$m.Groups[2].Value+'"'})
        WriteUtf8 $f.FullName $s
    }
    SetConf "$Root/configs/authserver.conf" 'RealmServerPort' $Cfg.AuthPort
    SetConf "$Root/configs/worldserver.conf" 'WorldServerPort' $Cfg.WorldPort
    foreach($file in 'worldserver','authserver'){SetConf "$Root/configs/$file.conf" 'BindIP' ('"'+$Cfg.BindIP+'"')}
    $address=$Cfg.RealmAddress
    if($address -eq 'auto') {
        $a=@(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {$_.InterfaceAlias -match 'Radmin' -and $_.IPAddress -match '^26\.'})
        $address=if($a.Count){$a[0].IPAddress}else{'127.0.0.1'}
    }
    if($address -notmatch '^[a-zA-Z0-9.:-]+$'){throw 'RealmAddress 주소 형식이 올바르지 않습니다.'}
    [void](Sql "UPDATE wl_auth.realmlist SET name='와우 에뮬레이터 연구소',address='$address',localAddress='$address',port=$($Cfg.WorldPort) WHERE id=1;")
    Event "Realm address=$address auth=$($Cfg.AuthPort) world=$($Cfg.WorldPort) mysql=localhost:$($Cfg.MySqlPort)"
}
function StartMysql {
    if((Owned 'mysqld.exe').Count){if(!(Test-Path "$Root/mysql/wer-ready")){throw 'DB 초기화가 완료되지 않았습니다. 로그를 확인하세요. 자동 재초기화하지 않습니다.'};return}
    if(PortOpen $Cfg.MySqlPort){throw 'MySQL 포트를 다른 서버가 사용 중입니다. 첫 실행 전에 scripts/settings.json 포트를 조정하세요.'}
    $fresh=!(Test-Path "$Root/mysql/data")
    if(!$fresh -and !(Test-Path "$Root/mysql/wer-ready")){throw '기존 mysql/data의 초기화가 완료되지 않았습니다. 덮어쓰지 말고 로그 확인 또는 새 폴더에 압축 해제 후 시험하세요.'}
    # Keep native MySQL options and source commands ASCII; cwd may contain Korean/spaces.
    WriteUtf8 "$Root/mysql/my.ini" "[mysqld]`nbasedir=./mysql`ndatadir=./mysql/data`nport=$($Cfg.MySqlPort)`nbind-address=127.0.0.1`nmysqlx=0`ncharacter-set-server=utf8mb4`ncollation-server=utf8mb4_unicode_ci`nmax_allowed_packet=128M`ninnodb_buffer_pool_size=512M`nlog-error=../../logs/mysql-error.log`n"
    WriteUtf8 "$Root/mysql/client.cnf" "[client]`nhost=127.0.0.1`nport=$($Cfg.MySqlPort)`nuser=root`npassword=$($Cfg.DbPassword)`nprotocol=tcp`n"
    if($fresh) {
        Event 'Initializing NEW private MySQL data directory'
        $init=Start-Process -FilePath "$Root/mysql/bin/mysqld.exe" -ArgumentList @('--defaults-file=mysql/my.ini','--initialize-insecure') -WorkingDirectory $Root -WindowStyle Hidden -PassThru -Wait
        if($init.ExitCode -ne 0){throw 'MySQL 초기화 실패: logs/mysql-error.log를 확인하세요.'}
    }
    [void](Start-Process -FilePath "$Root/mysql/bin/mysqld.exe" -ArgumentList '--defaults-file=mysql/my.ini' -WorkingDirectory $Root -WindowStyle Hidden -PassThru)
    $script:StartedTemporaryMysql=$true
    $limit=(Get-Date).AddSeconds(60)
    while(!(PortOpen $Cfg.MySqlPort)){if((Get-Date) -gt $limit){throw 'MySQL 시작 대기 시간 초과: 오류 로그를 확인하세요.'};Start-Sleep -Milliseconds 500}
    if($fresh) {
        WriteUtf8 "$Root/mysql/initial-client.cnf" "[client]`nhost=127.0.0.1`nport=$($Cfg.MySqlPort)`nuser=root`nprotocol=tcp`n"
        foreach($db in 'wl_auth','wl_characters','wl_playerbots','wl_world') {
            Write-Host "[초기 설치] $db 가져오는 중... 처음 한 번만 실행됩니다."; Event "Import $db started"
            [void](Sql "CREATE DATABASE $db CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;`nUSE $db;`nsource dump/$db.sql;" -Initial)
            Event "Import $db completed"
        }
        [void](Sql "source scripts/first_start_ahbot.sql;" -Initial)
        [void](Sql "CREATE USER 'wer'@'localhost' IDENTIFIED BY '$($Cfg.DbPassword)'; GRANT ALL ON wl_auth.* TO 'wer'@'localhost'; GRANT ALL ON wl_characters.* TO 'wer'@'localhost'; GRANT ALL ON wl_world.* TO 'wer'@'localhost'; GRANT ALL ON wl_playerbots.* TO 'wer'@'localhost'; ALTER USER 'root'@'localhost' IDENTIFIED BY '$($Cfg.DbPassword)';" -Initial)
        Remove-Item -LiteralPath "$Root/mysql/initial-client.cnf"
        WriteUtf8 "$Root/mysql/wer-ready" (Get-Date -Format o)
        Event 'Clean installation completed; ADMIN + first-start AH service bot; random bots generated by worldserver'
    }
}
try {
    New-Item -ItemType Directory -Force -Path "$Root/logs" | Out-Null
    if($Action -eq 'mysql') {
        if((Owned 'mysqld.exe').Count -or (PortOpen $Cfg.MySqlPort)){throw 'MySQL이 이미 실행 중이거나 포트가 사용 중입니다. 중복 실행하지 않습니다.'}
        if((Owned 'authserver.exe').Count -or (Owned 'worldserver.exe').Count){throw '게임 서버가 실행 중입니다. 먼저 정상 종료하세요.'}
        StartMysql
        Configure
        [void](Sql 'SHUTDOWN;')
        $until=(Get-Date).AddSeconds(60)
        while((Owned 'mysqld.exe').Count){if((Get-Date) -gt $until){throw '초기 준비용 MySQL 종료가 지연됩니다. 로그를 확인하세요.'};Start-Sleep -Milliseconds 500}
        $script:StartedTemporaryMysql=$false
        Write-Host '[준비 완료] 이제 이 창에서 MySQL을 실행합니다. 창을 열어 두세요.'
    } else {
        if(!(Owned 'mysqld.exe').Count -or !(PortOpen $Cfg.MySqlPort)){throw '먼저 1_MYSQL.bat을 실행하세요.'}
        if(!(Test-Path "$Root/mysql/wer-ready")){throw 'DB 설치가 완료되지 않았습니다. MySQL 창/로그를 확인하세요.'}
        if($Action -eq 'auth') {
            if((Owned 'authserver.exe').Count -or (PortOpen $Cfg.AuthPort)){throw '로그인 서버가 이미 실행 중이거나 포트가 사용 중입니다.'}
            if((Owned 'worldserver.exe').Count){throw '월드가 실행 중입니다. 월드 종료 후 로그인→월드 순서로 시작하세요.'}
            Configure
        } else {
            if(!(Owned 'authserver.exe').Count -or !(PortOpen $Cfg.AuthPort)){throw '먼저 2_AUTHSERVER.bat을 실행하세요.'}
            if((Owned 'worldserver.exe').Count -or (PortOpen $Cfg.WorldPort)){throw '월드 서버가 이미 실행 중이거나 포트가 사용 중입니다.'}
            $freeMB=[int]((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory/1024);$need=8192
            if($Cfg.PSObject.Properties.Name -contains 'MinFreeCommitMB'){$need=[math]::Max(6144,[int]$Cfg.MinFreeCommitMB)}
            if($freeMB -lt $need){throw "월드 시작에 커밋 여유 $need MB가 필요합니다. 현재 $freeMB MB입니다."}
        }
    }
    exit 0
} catch {
    $preparationError=$_.Exception.Message
    if($script:StartedTemporaryMysql -and (Owned 'mysqld.exe').Count){
        $stopped=$false
        try{[void](Sql 'SHUTDOWN;');$stopped=$true}catch{}
        if(!$stopped -and (Test-Path "$Root/mysql/initial-client.cnf")){
            try{[void](Sql 'SHUTDOWN;' -Initial);$stopped=$true}catch{}
        }
        if($stopped){Event 'Preparation failed; private temporary MySQL received graceful shutdown'}
        else{Write-Host '준비용 MySQL을 안전하게 종료하지 못했습니다. 이 리팩의 로그/프로세스를 확인하세요. 다른 서버는 종료하지 않습니다.' -ForegroundColor Yellow}
    }
    Write-Host ('[오류] '+$preparationError) -ForegroundColor Red
    Write-Host '기존 DB를 삭제하지 마세요. 종료 순서는 월드 → 로그인 → MySQL입니다.'
    exit 1
}
