# Pure mocked tests: no database, sockets, credentials or server processes are used.
$ErrorActionPreference='Stop'
$repoRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$helper=Join-Path $repoRoot 'source/packaging/runtime_rc2/wer-console-prepare.ps1'
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($helper,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Helper syntax errors'}
foreach($name in @('DecodeMysqlPath','AssertPrivateMysql','Sql')){
    $node=$ast.Find({param($item) $item -is [Management.Automation.Language.FunctionDefinitionAst] -and $item.Name -eq $name},$true)
    if(!$node){throw ('Missing guard function '+$name)}
    Invoke-Expression $node.Extent.Text
}
function Owned($Name){ ,@($script:mockProcesses) }
function Get-NetTCPConnection($State,$LocalPort,$ErrorAction){$script:mockListeners}
function GetNativeFileCodepage {return $script:mockCodepage}
function InvokePrivateSql($Text,[switch]$Initial){
    if($Text -eq 'SELECT HEX(@@datadir),@@port;'){return $script:mockIdentity}
    $script:mutations++
    return 'OK'
}
$Cfg=[pscustomobject]@{MySqlPort=13309}
$Root='C:\WER_QA'
$cases=@(
    @{name='matching_private_database';identity="C:/WER_QA/mysql/data/`t13309";pid=9001;address='127.0.0.1';pass=$true},
    @{name='wrong_listener_pid';identity="C:/WER_QA/mysql/data/`t13309";pid=9002;address='127.0.0.1';pass=$false},
    @{name='missing_listener';identity="C:/WER_QA/mysql/data/`t13309";pid=0;address='127.0.0.1';pass=$false},
    @{name='wrong_data_directory';identity="C:/OTHER_SERVER/mysql/data/`t13309";pid=9001;address='127.0.0.1';pass=$false},
    @{name='wrong_sql_port';identity="C:/WER_QA/mysql/data/`t3306";pid=9001;address='127.0.0.1';pass=$false},
    @{name='non_loopback_listener';identity="C:/WER_QA/mysql/data/`t13309";pid=9001;address='0.0.0.0';pass=$false},
    @{name='missing_owned_process';identity="C:/WER_QA/mysql/data/`t13309";pid=9001;address='127.0.0.1';no_process=$true;pass=$false},
    @{name='foreign_second_listener';identity="C:/WER_QA/mysql/data/`t13309";pid=9001;address='127.0.0.1';foreign_second=$true;pass=$false},
    @{name='canonical_slashes_case';identity="c:\wer_qa\mysql\data\`t13309";pid=9001;address='::1';pass=$true},
    @{name='invalid_identity_columns';identity='invalid';pid=9001;address='127.0.0.1';pass=$false}
)
# Build Unicode fixture text from code points, keeping the test portable in Windows PS5.1.
$unicodeFolder=([char]0xd55c).ToString()+[char]0xae00+' folder'
$cases+=@{name='unicode_and_space_directory';root=('C:\'+$unicodeFolder);identity=('C:/'+$unicodeFolder+"/mysql/data/`t13309");pid=9001;address='127.0.0.1';pass=$true}
$cases+=@{name='utf8_system_codepage';root=('C:\'+$unicodeFolder);identity=('C:/'+$unicodeFolder+"/mysql/data/`t13309");pid=9001;address='127.0.0.1';codepage=65001;pass=$true}
$cases+=@{name='invalid_hex_path';identity="GG`t13309";raw_hex=$true;pid=9001;address='127.0.0.1';pass=$false}
foreach($case in $cases){
    $Root=if($case.root){$case.root}else{'C:\WER_QA'}
    $script:mockProcesses=if($case.no_process){@()}else{@([pscustomobject]@{ProcessId=9001})}
    $script:mockListeners=@(if($case.pid){[pscustomobject]@{OwningProcess=$case.pid;LocalAddress=$case.address}})
    if($case.foreign_second){$script:mockListeners+=([pscustomobject]@{OwningProcess=9002;LocalAddress='::1'})}
    $script:mockCodepage=if($case.codepage){$case.codepage}else{949}
    $identityFields=$case.identity.Split("`t")
    if($identityFields.Count-eq 2 -and !$case.raw_hex){
        $pathHex=([Text.Encoding]::GetEncoding($script:mockCodepage).GetBytes($identityFields[0])|ForEach-Object {$_.ToString('X2')}) -join ''
        $script:mockIdentity=$pathHex+"`t"+$identityFields[1]
    }else{$script:mockIdentity=$case.identity}
    $script:mutations=0;$accepted=$false
    try{$null=Sql 'UPDATE fixture SET value=1;';$accepted=$true}catch{}
    if($accepted-ne $case.pass -or $script:mutations-ne [int]$case.pass){throw ('Guard regression '+$case.name)}
    Write-Output ('PASS '+$case.name)
}
Write-Output ('PASS: '+$cases.Count+' private SQL guard cases; no real DB/process mutations')
