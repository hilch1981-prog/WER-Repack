param([string]$Action='status')
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
try { & "$PSScriptRoot/wer-server.ps1" -Action $Action; exit 0 }
catch { Write-Host ("`n[오류] "+$_.Exception.Message) -ForegroundColor Red; Write-Host '서버 상태/오류 로그를 확인하세요. 기존 DB를 삭제하거나 다시 초기화하지 마세요.'; exit 1 }
