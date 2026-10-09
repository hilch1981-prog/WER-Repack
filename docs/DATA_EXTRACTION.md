# 게임 데이터 준비 / Client data extraction

실행 ZIP에는 게임 클라이언트, DBC, maps, vmaps, mmaps와 기존 pathway 캐시가 없습니다. 다른 사람이 올린 게임 데이터 다운로드 링크로 대체하지 않습니다. 합법적으로 확보한 **3.3.5a / build 12340 koKR** 클라이언트를 사용하세요.

네 추출기는 서버와 같은 소스에서 빌드했습니다. 지도 `MAPS v9`, vmap `VMAP_4.8`, mmap tile `v20`을 사용합니다. 예전 추출기나 다른 코어의 데이터와 섞지 마세요. `mmap v20`은 리팩 버전 2.0이라는 뜻이 아닙니다.

아래는 PowerShell 예시입니다. **새 리팩 루트**에서 실행하고 클라이언트 경로만 바꾸세요. 서버는 종료 상태여야 합니다. 과정은 콘솔에 표시되며 다음 단계 전에 종료 코드와 완료 메시지를 확인합니다. 전체 mmap 생성은 오래 걸릴 수 있으며 전체 진행률이나 완료 시간을 보장하지 않습니다.

```powershell
$clientPath = 'C:\Games\WoW335a'
$repackPath = (Get-Location).Path
New-Item -ItemType Directory -Path '.\data' -Force
.\map_extractor.exe -i $clientPath -o '.\data'
if ($LASTEXITCODE -ne 0) { throw 'map/DBC/Camera extraction failed' }

# vmap raw output is written under the working directory.
Push-Location '.\data'
try {
    & "$repackPath\vmap4_extractor.exe" -d $clientPath
    if ($LASTEXITCODE -ne 0) { throw 'vmap raw extraction failed' }
    & "$repackPath\vmap4_assembler.exe" '.\Buildings' '.\vmaps'
    if ($LASTEXITCODE -ne 0) { throw 'vmap assembly failed' }
    & "$repackPath\mmaps_generator.exe" --config "$repackPath\mmaps-config.yaml" --threads 2
    if ($LASTEXITCODE -ne 0) { throw 'mmap generation failed' }
} finally {
    Pop-Location
}
```

결과는 `data/dbc` 및 locale 하위 폴더, `data/maps`, `data/Cameras`, `data/vmaps`, `data/mmaps`입니다. 클라이언트에 `Data/koKR` locale MPQ가 있어야 한글 DBC를 추출할 수 있습니다. 실행만으로 누락/손상 MPQ가 복구되거나 클라이언트 버전이 변경되지는 않습니다. `Spell.dbc`를 번역하거나 다른 버전으로 교체하지 마세요.

`data/Buildings`는 raw 중간 산출물입니다. 정상 vmap 생성·서버 검증 전에는 삭제하지 마세요. `.wlp` 캐시는 배포하지 않으며 없을 때 서버는 실제 pathfinding을 사용합니다. 재구축은 별도 운영 선택입니다.

**검증 한계:** 기존 추출 자료의 헤더와 실제 서버 이동, 네 도구의 빌드·도움말을 확인합니다. 모든 사용자 MPQ의 전체 재추출 성공을 뜻하지 않습니다. 전용 GUI 런처·playermap 병합은 구현하지 않았습니다.

## English

No game client or extracted client data is redistributed. Use your lawfully obtained WotLK **3.3.5a / 12340 koKR** client. These four tools match the server source; do not mix formats from other cores. Run the commands above from the new repack root with servers stopped. Inspect each exit status and completion message before proceeding; full mmap generation can take a long time. This is a console workflow, not a new GUI launcher/playermap integration. Tool builds and local server tests do not prove extraction from every user's MPQ set. Preserve locale folders and do not manually translate or replace Spell.dbc.
