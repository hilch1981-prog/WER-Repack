# Mocked migration failure-path and commit-capacity tests. No real database/process changes.
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$package=Join-Path $repo 'source/packaging/runtime_rc2'
. (Join-Path $package 'wer-migrate-rc3.ps1')
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('wer-rc3-tests-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($temporary)
$script:engine='3';$script:batchCalls=0;$script:mode='success'
function Sql($sql){
    if($sql.Contains('information_schema.tables')){return $script:engine}
    if($sql.StartsWith('START TRANSACTION;')){
        $script:batchCalls++
        if(!$sql.Contains('COMMIT;') -or !$sql.Contains('@wer_rc3_equipment_rows')){throw 'Batch is missing transaction/receipt metadata'}
        if($script:mode -eq 'fail'){throw 'Simulated connection loss during commit'}
        return '{"equipment_rows":1,"equipment_inserted_id":99999,"duration_rows":2,"npc_rows":1}'
    }
    return '{"equipment_texts":[],"items":[],"npc":[]}'
}
$passes=0
try {
    $result=Invoke-WerRc3Migration -MigrationDirectory (Join-Path $package 'migrations/rc3') -BackupDirectory (Join-Path $temporary 'good')
    $receipt=Get-Content -LiteralPath $result.receipt -Raw -Encoding UTF8|ConvertFrom-Json
    if($receipt.status -ne 'APPLIED' -or $script:batchCalls -ne 1 -or $receipt.changes.equipment_inserted_id -ne 99999 -or !$receipt.before -or !$receipt.after){throw 'Successful migration receipt regression'}
    $passes++;Write-Output 'PASS transaction and before/after receipt'
    $script:engine='2';$script:batchCalls=0;$rejected=$false
    try{$null=Invoke-WerRc3Migration -MigrationDirectory (Join-Path $package 'migrations/rc3') -BackupDirectory (Join-Path $temporary 'engine')}catch{$rejected=$true}
    if(!$rejected -or $script:batchCalls -or (Test-Path -LiteralPath (Join-Path $temporary 'engine'))){throw 'Engine mismatch mutated DB or wrote success receipt'}
    $passes++;Write-Output 'PASS unsupported storage engine rejected before mutation'
    $script:engine='3';$script:mode='fail';$script:batchCalls=0;$rejected=$false
    try{$null=Invoke-WerRc3Migration -MigrationDirectory (Join-Path $package 'migrations/rc3') -BackupDirectory (Join-Path $temporary 'failed')}catch{$rejected=$true}
    $failed=Get-ChildItem -LiteralPath (Join-Path $temporary 'failed') -Filter '*.json' | Select-Object -First 1
    $receipt=Get-Content -LiteralPath $failed.FullName -Raw|ConvertFrom-Json
    if(!$rejected -or $receipt.status -ne 'FAILED_OR_COMMIT_OUTCOME_UNKNOWN' -or $script:batchCalls -ne 1){throw 'Commit ambiguity was hidden'}
    $passes++;Write-Output 'PASS ambiguous commit retained as unknown outcome'
    Copy-Item -LiteralPath (Join-Path $package 'migrations/rc3') -Destination (Join-Path $temporary 'tampered') -Recurse
    [IO.File]::AppendAllText((Join-Path $temporary 'tampered/01_equipment_notice_koKR.sql'),'-- altered fixture')
    $script:batchCalls=0;$rejected=$false
    try{$null=Invoke-WerRc3Migration -MigrationDirectory (Join-Path $temporary 'tampered') -BackupDirectory (Join-Path $temporary 'hash')}catch{$rejected=$true}
    if(!$rejected -or $script:batchCalls){throw 'Tampered SQL was executed'}
    $passes++;Write-Output 'PASS hash mismatch rejected before mutation'
    $rejected=$false
    try{$null=Invoke-WerRc3Migration -MigrationDirectory (Join-Path $temporary 'missing') -BackupDirectory (Join-Path $temporary 'missing-receipt')}catch{$rejected=$true}
    if(!$rejected -or $script:batchCalls){throw 'Missing SQL file was accepted'}
    $passes++;Write-Output 'PASS missing SQL rejected before mutation'
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $package 'wer-console-prepare.ps1'),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'Console helper syntax failure'}
    $node=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'FreeCommitMiB'},$true)
    Invoke-Expression $node.Extent.Text
    function Get-CimInstance($ClassName){return $script:memory}
    foreach($case in @(@{limit=32GB;used=20GB;expected=12288},@{limit=32GB;used=16GB;expected=16384},@{limit=16GB;used=(16GB-1023);expected=0})){
        $script:memory=[pscustomobject]@{CommitLimit=$case.limit;CommittedBytes=$case.used}
        if((FreeCommitMiB) -ne $case.expected){throw 'Commit headroom arithmetic regression'}
        $passes++
    }
    $script:memory=[pscustomobject]@{CommitLimit=0;CommittedBytes=0};$rejected=$false
    try{$null=FreeCommitMiB}catch{$rejected=$true}
    if(!$rejected){throw 'Missing counters allowed startup'}
    $passes++;Write-Output 'PASS exact commit capacity and fail-closed missing counters'
    Write-Output ('PASS '+$passes+' RC3 guard cases (mocked; not database integration proof)')
}finally{
    $resolved=[IO.Path]::GetFullPath($temporary)
    $expectedParent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if((Split-Path $resolved -Parent) -eq $expectedParent -and (Split-Path $resolved -Leaf).StartsWith('wer-rc3-tests-')){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
