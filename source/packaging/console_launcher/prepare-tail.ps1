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
    Write-Host ('[오류] '+$_.Exception.Message) -ForegroundColor Red
    Write-Host '기존 DB를 삭제하지 마세요. 종료 순서는 월드 → 로그인 → MySQL입니다.'
    exit 1
}
