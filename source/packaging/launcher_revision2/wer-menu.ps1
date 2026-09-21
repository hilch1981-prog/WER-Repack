param([switch]$Check)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
if($Check){& "$PSScriptRoot/wer-server.ps1" check;return}
while($true){
    Write-Host "`n================================================" -ForegroundColor Cyan
    Write-Host ' WER REPACK_VER.1.0.0 - 서버 실행 메뉴 (개정 2)' -ForegroundColor Cyan
    Write-Host '================================================'
    Write-Host ' 1. MySQL 시작 / 첫 실행 DB 자동 설치'
    Write-Host ' 2. 로그인 서버 시작 (MySQL 자동 확인)'
    Write-Host ' 3. 월드 서버 시작 (로그인 서버 자동 확인)'
    Write-Host ' 4. 전체 시작 (MySQL → 로그인 → 월드)'
    Write-Host ' 5. 실행 상태 / 접속 준비 확인'
    Write-Host ' 6. 오류 로그 확인'
    Write-Host ' 7. 월드 서버 종료 (저장 후 종료)'
    Write-Host ' 8. 로그인 서버 종료 (월드 먼저 종료)'
    Write-Host ' 9. MySQL 종료 (게임 서버 먼저 종료)'
    Write-Host ' A. 전체 종료 (월드 저장 → 로그인 → MySQL)'
    Write-Host ' 0. 메뉴만 닫기 (실행 중인 서버는 유지)'
    Write-Host '------------------------------------------------'
    $choice=Read-Host '번호 선택'
    if($choice -eq '0'){return}
    $commands=@{'1'='mysql';'2'='auth';'3'='world';'4'='start';'5'='status';'6'='logs';'7'='stopworld';'8'='stopauth';'9'='stopmysql';'a'='stopall'}
    if(!$commands.ContainsKey($choice)){Write-Host '목록의 번호를 입력하세요.';continue}
    try{& "$PSScriptRoot/wer-server.ps1" -Action $commands[$choice]}
    catch{Write-Host ('[오류] '+$_.Exception.Message) -ForegroundColor Red}
    [void](Read-Host 'Enter를 누르면 메뉴로 돌아갑니다')
}
