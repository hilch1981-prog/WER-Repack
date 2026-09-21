param([ValidateSet('start','mysql','server','auth','world','stop','stopall','stopauth','stopworld','stopmysql','status','logs','check','watch')][string]$Action='status')
$ErrorActionPreference='Stop'
$Root=Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $Root
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$Cfg=Get-Content -LiteralPath "$PSScriptRoot/settings.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Cfg.DbPassword -notmatch '^[a-zA-Z0-9_-]{12,80}$') { throw 'DbPassword는 영문·숫자·하이픈·밑줄 12~80자로 설정하세요.' }
$Utf8=New-Object Text.UTF8Encoding($false)
function WriteUtf8($Path,$Value) { [IO.File]::WriteAllText($Path,$Value,$Utf8) }
function PortOpen($Port) {
    $c=New-Object Net.Sockets.TcpClient
    try { $a=$c.BeginConnect('127.0.0.1',[int]$Port,$null,$null); if (!$a.AsyncWaitHandle.WaitOne(200)) {return $false}; $c.EndConnect($a); return $true } catch {return $false} finally {$c.Dispose()}
}
function Owned($Name) { ,@(Get-CimInstance Win32_Process -Filter "Name='$Name'" | Where-Object {$_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq [IO.Path]::GetFullPath((Join-Path $Root $(if($Name -eq 'mysqld.exe'){'mysql/bin/mysqld.exe'}else{$Name})))}) }
function Event($Message) { Add-Content -LiteralPath "$Root/logs/watchdog.log" -Encoding UTF8 -Value ("{0} {1}" -f (Get-Date -Format o),$Message) }
function Sql($Text,[switch]$Initial) {
    $i=New-Object Diagnostics.ProcessStartInfo
    $i.FileName="$Root/mysql/bin/mysql.exe"
    $defaults=if($Initial){'initial-client.cnf'}else{'client.cnf'}
    $i.Arguments="--defaults-extra-file=mysql/$defaults --default-character-set=utf8mb4 --batch --skip-column-names"
    $i.WorkingDirectory=$Root; $i.UseShellExecute=$false; $i.CreateNoWindow=$true
    $i.RedirectStandardInput=$true; $i.RedirectStandardOutput=$true; $i.RedirectStandardError=$true
    $i.StandardOutputEncoding=$Utf8; $i.StandardErrorEncoding=$Utf8
    $p=New-Object Diagnostics.Process; $p.StartInfo=$i; [void]$p.Start()
    $outTask=$p.StandardOutput.ReadToEndAsync(); $errTask=$p.StandardError.ReadToEndAsync()
    $bytes=$Utf8.GetBytes($Text+"`n"); $p.StandardInput.BaseStream.Write($bytes,0,$bytes.Length); $p.StandardInput.BaseStream.Flush(); $p.StandardInput.Close(); $p.WaitForExit()
    $out=$outTask.Result; $err=$errTask.Result; $code=$p.ExitCode; $p.Dispose()
    if($code -ne 0) {throw "MySQL 처리 실패 ($code): $err"}; return $out.Trim()
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
    [void](Sql "UPDATE wl_auth.realmlist SET address='$address',localAddress='127.0.0.1',port=$($Cfg.WorldPort) WHERE id=1;")
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
function IsWatchRunning {
    if(!(Test-Path "$Root/logs/watchdog.pid")){return $false}
    $watchId=[int]([IO.File]::ReadAllText("$Root/logs/watchdog.pid"))
    $w=Get-CimInstance Win32_Process -Filter "ProcessId=$watchId" -ErrorAction SilentlyContinue
    return ($w -and $w.CommandLine -and $w.CommandLine.Contains($PSScriptRoot) -and $w.CommandLine.Contains('watch'))
}
function Launch($Name) {
    $i=New-Object Diagnostics.ProcessStartInfo
    $i.FileName="$Root/$Name.exe"; $i.Arguments="-c configs/$Name.conf"; $i.WorkingDirectory=$Root
    $i.UseShellExecute=$false; $i.CreateNoWindow=$true; $i.RedirectStandardInput=$true
    $p=New-Object Diagnostics.Process; $p.StartInfo=$i; [void]$p.Start()
    Event "$Name started pid=$($p.Id)"; return $p
}
function Desired {
    if(Test-Path "$Root/logs/desired.json"){return (Get-Content "$Root/logs/desired.json" -Raw -Encoding UTF8|ConvertFrom-Json)}
    return [pscustomobject]@{Auth=$false;World=$false}
}
function SetDesired([bool]$Auth,[bool]$World) {
    $dest="$Root/logs/desired.json";$tmp="$dest.$PID.tmp"
    WriteUtf8 $tmp (@{Auth=$Auth;World=$World}|ConvertTo-Json -Compress)
    if(Test-Path $dest){[IO.File]::Replace($tmp,$dest,($dest+'.previous'))}else{[IO.File]::Move($tmp,$dest)}
}
function WaitUntil([scriptblock]$Test,[int]$Seconds,[string]$ErrorText) {
    $until=(Get-Date).AddSeconds($Seconds)
    while(!(& $Test)){if((Get-Date) -gt $until){throw $ErrorText};Start-Sleep -Milliseconds 500}
}
function StopWorld($Process) {
    if($Process -and !$Process.HasExited){
        Event 'Graceful world shutdown requested'
        $bytes=[Text.Encoding]::ASCII.GetBytes("server shutdown 0`n")
        $Process.StandardInput.BaseStream.Write($bytes,0,$bytes.Length);$Process.StandardInput.BaseStream.Flush();$Process.StandardInput.Close()
        if(!$Process.WaitForExit(120000)){throw '월드 서버가 저장/종료 중입니다. 강제 종료하지 않았습니다. DB를 유지합니다.'}
    }
}
function StopMysql {
    if((Owned 'worldserver.exe').Count -or (Owned 'authserver.exe').Count -or (IsWatchRunning)){throw '게임 서버가 실행 중입니다. 먼저 전체 종료 또는 월드→로그인 순서로 종료하세요.'}
    if((Owned 'mysqld.exe').Count){[void](Sql 'SHUTDOWN;');WaitUntil {!(Owned 'mysqld.exe').Count} 60 'MySQL 종료가 지연 중입니다. 강제 종료하지 않았습니다.';Event 'Private MySQL stopped'}
    Write-Host '[완료] MySQL 종료 상태입니다.'
}
function ShowStatus {
    Write-Host "`n=== WER 1.0.0 서버 상태 ===`n경로: $Root"
    foreach($r in @(@('MySQL','mysqld.exe',$Cfg.MySqlPort),@('로그인 서버','authserver.exe',$Cfg.AuthPort),@('월드 서버','worldserver.exe',$Cfg.WorldPort))) {
        $owned=Owned $r[1];$state=if($owned.Count){if(PortOpen $r[2]){'실행 중'}else{'시작/종료 중'}}else{'종료'}
        Write-Host ("{0,-12}: {1} / 포트 {2} / PID {3}" -f $r[0],$state,$r[2],($owned.ProcessId -join ','))
    }
    Write-Host ('자동 복구 감시: '+$(if(IsWatchRunning){'실행 중'}else{'종료'}))
    if((Owned 'worldserver.exe').Count -and (PortOpen $Cfg.MySqlPort)) {try{Write-Host ('렐름: '+$(if((Sql 'SELECT flag & 3 FROM wl_auth.realmlist WHERE id=1;') -eq '0'){'접속 준비 완료'}else{'준비 중'}))}catch{Write-Host '렐름 확인 대기'}}
}
function ValidateFiles {
    foreach($n in 'authserver.exe','worldserver.exe','mysql/bin/mysql.exe','mysql/bin/mysqld.exe','configs/authserver.conf','configs/worldserver.conf','dump/wl_auth.sql','dump/wl_characters.sql','dump/wl_world.sql','dump/wl_playerbots.sql','scripts/first_start_ahbot.sql') {
        if(!(Test-Path -LiteralPath "$Root/$n" -PathType Leaf)){throw "필수 파일이 없습니다: $n"}
    }
    $ports=@($Cfg.MySqlPort,$Cfg.AuthPort,$Cfg.WorldPort)
    if(@($ports|Sort-Object -Unique).Count -ne 3 -or @($ports|Where-Object {$_ -lt 1 -or $_ -gt 65535}).Count){throw '설정 포트는 1~65535의 서로 다른 숫자여야 합니다.'}
}
function BeginServers([bool]$WantWorld) {
    ValidateFiles
    foreach($r in @(@('authserver.exe',$Cfg.AuthPort),@('worldserver.exe',$Cfg.WorldPort))) {
        if((PortOpen $r[1]) -and !(Owned $r[0]).Count){throw "포트 $($r[1]) 를 다른 서버가 사용 중입니다. 다른 서버는 변경하지 않았습니다."}
    }
    if($WantWorld -and !(Owned 'worldserver.exe').Count){
        $freeMB=[int]((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory/1024);$need=8192
        if($Cfg.PSObject.Properties.Name -contains 'MinFreeCommitMB'){$need=[math]::Max(6144,[int]$Cfg.MinFreeCommitMB)}
        if($freeMB -lt $need){throw "메모리 커밋 여유가 $freeMB MB입니다. 최소 $need MB를 확보하세요."}
    }
    StartMysql
    if(!(IsWatchRunning)) {
        if((Owned 'worldserver.exe').Count -or (Owned 'authserver.exe').Count){throw '관리 감시 없이 서버가 실행 중입니다. 상태/로그를 확인하세요. 새 서버를 중복 시작하지 않습니다.'}
        Configure;SetDesired $true $WantWorld
        $stamp=Get-Date -Format yyyyMMdd_HHmmss_fff
        $p=Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" watch" -WorkingDirectory $Root -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Root/logs/console-$stamp.log" -RedirectStandardError "$Root/logs/console-$stamp.err.log"
        WriteUtf8 "$Root/logs/watchdog.pid" $p.Id
    }else{$d=Desired;SetDesired $true ($WantWorld -or $d.World)}
    WaitUntil {(Owned 'authserver.exe').Count -and (PortOpen $Cfg.AuthPort)} 45 '로그인 서버 시작에 실패했습니다. 로그 확인 메뉴를 이용하세요.'
    Write-Host '[완료] MySQL·로그인 서버 준비 완료.'
    if($WantWorld){Write-Host '[시작] 월드 서버를 불러옵니다. 첫 봇 생성에는 시간이 걸립니다. 상태 확인에서 접속 준비 완료를 확인하세요.'}
}
function StopServers([bool]$All,[bool]$WorldOnly) {
    if(!(IsWatchRunning)) {
        if((Owned 'worldserver.exe').Count -or (Owned 'authserver.exe').Count){throw '이 실행기로 관리되지 않는 서버입니다. 강제 종료하지 않았습니다.'}
        Write-Host '[확인] 게임 서버는 이미 종료되어 있습니다.';return
    }
    $d=Desired
    if(!$All -and !$WorldOnly -and ($d.World -or (Owned 'worldserver.exe').Count)){throw '월드 서버가 실행 중입니다. 먼저 월드 서버를 종료하세요.'}
    if($WorldOnly){SetDesired ([bool]$d.Auth) $false;Write-Host '[종료] 월드 서버 데이터 저장을 기다립니다...';WaitUntil {!(Owned 'worldserver.exe').Count} 130 '월드 종료 대기 시간 초과. DB는 유지됩니다.'}
    else{SetDesired $false $false;Write-Host '[종료] 게임 서버 정상 종료를 기다립니다...';WaitUntil {!(IsWatchRunning) -and !(Owned 'worldserver.exe').Count -and !(Owned 'authserver.exe').Count} 140 '서버 종료 대기 시간 초과. DB는 유지됩니다.'}
    Write-Host '[완료] 요청한 게임 서버 종료를 확인했습니다.'
}
if($Action -eq 'check'){ValidateFiles;Write-Host '[검사 완료] 필수 파일·포트 설정 정상. 서버/DB를 시작하거나 변경하지 않았습니다.';return}
New-Item -ItemType Directory -Force -Path "$Root/logs" | Out-Null
$hash=[Security.Cryptography.SHA256]::Create();$tag=[BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Root.ToLowerInvariant()))).Replace('-','');$hash.Dispose()
if($Action -eq 'watch') {
    $mutex=New-Object Threading.Mutex($false,('Local\WER-watch-'+$tag))
    if(!$mutex.WaitOne(0)){throw '다른 관리 감시가 이미 실행 중입니다.'}
    $world=$null;$auth=$null;$fails=0;$authFails=0;$realmPending=$false
    try {
        WriteUtf8 "$Root/logs/watchdog.pid" $PID
        while($true){
            $d=Desired
            if(!$d.World -and $world){StopWorld $world;$world.Dispose();$world=$null;$fails=0}
            if(!$d.Auth -and $auth){if(!$auth.HasExited){$auth.Kill();$auth.WaitForExit()};$auth.Dispose();$auth=$null;$authFails=0}
            if(!$d.Auth -and !$d.World){break}
            if(!(Owned 'mysqld.exe').Count){StartMysql}
            if($d.Auth -and (!$auth -or $auth.HasExited)) {
                if($auth){Event "authserver exited code=$($auth.ExitCode)";$auth.Dispose();$auth=$null;$authFails++;Start-Sleep -Seconds $Cfg.RestartDelaySeconds}
                if($authFails -ge 5){throw '로그인 서버 반복 오류 5회: 로그 확인 후 다시 시작하세요.'}
                if(!(Desired).Auth){$auth=$null;continue}
                if(!$world -or $world.HasExited){[void](Sql 'UPDATE wl_auth.realmlist SET flag=flag & ~3 WHERE id=1;')}
                $auth=Launch 'authserver';$realmPending=$true
            }
            if(!(PortOpen $Cfg.AuthPort)){Start-Sleep -Seconds 1;continue}
            if($d.World -and (!$world -or $world.HasExited)) {
                if($world){Event "worldserver exited code=$($world.ExitCode)";$world.Dispose();$world=$null;$fails++;Start-Sleep -Seconds $Cfg.RestartDelaySeconds}
                if($fails -ge 5){throw '월드 서버 반복 오류 5회: 로그 확인 후 다시 시작하세요.'}
                if(!(Desired).World){$world=$null;continue}
                if(Test-Path "$Root/logs/Server.log"){Copy-Item "$Root/logs/Server.log" ("$Root/logs/Server-before-"+(Get-Date -Format yyyyMMdd_HHmmss)+'.log')}
                $world=Launch 'worldserver';$realmPending=$true
            }
            if($realmPending -and $world -and !$world.HasExited -and (PortOpen $Cfg.WorldPort)) {
                [void](Sql 'UPDATE wl_auth.realmlist SET flag=flag & ~2 WHERE id=1 AND (flag & 1)=0;')
                if((Sql 'SELECT flag & 3 FROM wl_auth.realmlist WHERE id=1;') -eq '0'){$realmPending=$false;Event 'Realm online confirmed'}
            }
            Start-Sleep -Seconds 1
        }
    }finally{
        StopWorld $world
        if($auth -and !$auth.HasExited){$auth.Kill();$auth.WaitForExit()}
        Event 'Watchdog stopped';$mutex.ReleaseMutex();$mutex.Dispose()
    }
    return
}
$commandLock=New-Object Threading.Mutex($false,('Local\WER-command-'+$tag))
if(!$commandLock.WaitOne(0)){throw '다른 시작/종료 작업이 진행 중입니다. 완료 후 다시 실행하세요.'}
try {
    switch($Action){
        'status'{ShowStatus}
        'logs'{foreach($n in 'Errors.log','AuthErrors.log','mysql-error.log','watchdog.log'){Write-Host "`n=== $n (최근 25줄) ===";if(Test-Path "$Root/logs/$n"){Get-Content "$Root/logs/$n" -Tail 25 -Encoding UTF8}else{Write-Host '아직 로그가 없습니다.'}}}
        'mysql'{ValidateFiles;StartMysql;if(!(IsWatchRunning)){Configure};Write-Host '[완료] MySQL 준비 완료. 다음으로 로그인 서버를 시작할 수 있습니다.'}
        'auth'{BeginServers $false}
        {$_ -in 'start','server','world'}{BeginServers $true}
        'stopworld'{StopServers $false $true}
        'stopauth'{StopServers $false $false}
        'stop'{StopServers $true $false}
        'stopall'{StopServers $true $false;StopMysql}
        'stopmysql'{StopMysql}
    }
}finally{$commandLock.ReleaseMutex();$commandLock.Dispose()}
