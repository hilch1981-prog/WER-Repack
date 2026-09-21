# 설치 · 실행 · 접속

## 1. 실행용 파일 받기 — ZIP 하나면 됩니다

가장 간단한 방법은 [v1.1.0 릴리즈](https://github.com/hilch1981-prog/WER-Repack/releases/tag/v1.1.0)의 **`WER_REPACK_VER.1.1.0.zip` 하나를 받아 일반 ZIP처럼 압축 해제**하는 것입니다. `SHA256SUMS.txt`로 해시를 비교할 수 있습니다. 재압축으로 GitHub 파일 크기 제한 안에 들어왔습니다. 아래 분할 파일은 다운로드를 나누고 싶은 분을 위한 대안이며 단일 ZIP과 내용이 같습니다.

### 선택: 분할 파일로 받기

[v1.1.0 릴리즈](https://github.com/hilch1981-prog/WER-Repack/releases/tag/v1.1.0)에서 다음 파일을 같은 폴더에 받습니다.

- `WER_REPACK_VER.1.1.0.zip.001`
- `WER_REPACK_VER.1.1.0.zip.002`
- `SHA256SUMS.txt`
- 선택: `Merge-Repack.ps1` (ZIP 병합 도우미)

분할 파일 하나만 받으면 설치되지 않습니다. **7-Zip으로 `.001`을 열어 압축 해제**하거나, 다음 병합 도우미로 일반 ZIP을 만든 뒤 Windows에서 해제하세요. 7-Zip은 [공식 사이트](https://www.7-zip.org/)에서 받습니다. 분할 형식 지원 여부는 사용하는 압축 프로그램에 따라 다릅니다.

PowerShell을 다운로드 폴더에서 열고:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Merge-Repack.ps1
```

도우미는 두 조각의 크기/해시 및 합쳐진 ZIP 해시를 확인하고 새 ZIP만 만듭니다. 기존 ZIP은 덮어쓰지 않습니다. 실행 전 스크립트 내용을 확인하세요. 압축 해제가 끝나기 전 조각을 지우지 마세요.

> GitHub의 녹색 Code → Download ZIP 또는 자동 생성 Source code는 개발용 소스입니다. MySQL/실행파일/초기 DB가 필요한 일반 이용자는 위 리팩 조각을 받으세요.

## 2. 새 폴더에 풀기

예: `C:\WER\WER REPACK_VER.1.1.0`. 한글·공백 경로를 지원하지만 짧은 로컬 경로를 권장합니다. ZIP 안에서 배치를 실행하거나 Program Files 같은 쓰기 제한 폴더에 풀지 마세요. 기존 서버 폴더에 덮어쓰지 않습니다.

기본 설정은 다음과 같습니다.

| 항목 | 기본값 |
|---|---|
| 로그인 계정 | admin / admin (GM3) |
| MySQL | 127.0.0.1:3306, 외부 개방 금지 |
| 로그인 포트 | 3724 |
| 월드 포트 | 59823 |
| 주소 선택 | Radmin 26.* 감지 시 사용, 없으면 127.0.0.1 |
| 봇 | 목표 온라인 2,000, 첫 실행 시 생성 |

DB는 첫 설치만 자동으로 가져옵니다. 재실행한다고 초기화하지 않습니다. 오류가 나도 **`mysql/data`를 삭제하지 마세요.**

## 3. NVIDIA API 설정 — LLM 대화에 필수

첫 월드 실행 전에 [NVIDIA API 키 발급·입력](NVIDIA_API.md)을 완료하세요. 키를 생략하면 기본 서버는 실행할 수 있어도 외부 LLM 대화는 사용할 수 없습니다. 원래 동봉 파일명과 키는 `mod_wowlegends.conf` / `WowLegends.AiChat.*`이며 임의로 WER로 바꾸면 읽히지 않습니다.

## 4. 순서대로 시작

1. `1_MYSQL.bat`: 최초 설치가 끝나고 `ready for connections`가 나올 때까지 기다립니다.
2. `2_AUTHSERVER.bat`: 로그인 서버가 열렸는지 확인합니다.
3. `3_WORLDSERVER.bat`: 초기 로딩 및 봇 생성을 기다립니다. 창을 중복 실행하지 마세요.

세 콘솔 창이 보이는 방식이며 백그라운드 자동 재시작은 없습니다. 처음 설치에만 DB 준비용 임시 프로세스가 사용되고 정상 종료 후 콘솔 MySQL로 전환됩니다.

## 5. 게임 연결

한국어 3.3.5a(12340) 클라이언트의 `Data\koKR\realmlist.wtf`를 다음처럼 설정합니다.

```text
set realmlist 127.0.0.1
```

서버와 같은 PC에서 사용하는 예입니다. Radmin으로 연결하는 다른 이용자는 같은 Radmin 네트워크에 가입한 뒤 **서버 운영자의 Radmin IP**를 입력합니다. 다른 사용자의 IP나 제작자 예전 IP를 복사하지 마세요. Radmin은 VPN 접속이며 일반 인터넷 포트포워딩과 같지 않습니다.

필요한 경우 운영자가 로그인/월드 포트만 방화벽에 허용합니다. MySQL 3306은 외부에 열지 마세요. `scripts/settings.json`의 포트·RealmAddress를 바꾸면 관련 클라이언트/방화벽/DB 주소도 일치해야 합니다. 운영 환경 변경은 백업 후 수행하세요.

기본 admin/admin은 누구나 아는 비밀번호입니다. **외부 개방 전에 반드시 변경하고 일반 이용자에게 GM 계정을 공유하지 마세요.**

`전용애드온` 폴더는 선택 사항입니다. 필요한 애드온 폴더만 클라이언트 `Interface\AddOns`로 복사하고 중복 봇 관리 애드온은 피하세요.

## 6. 종료와 백업

**월드 → 로그인 → MySQL** 순서로 각 창에서 Ctrl+C를 누르고 종료를 기다립니다. 배치 종료 확인에는 Y를 입력합니다. 정상 종료 후 리팩 폴더 전체 또는 DB/설정을 별도로 백업하세요. 실행 중인 MySQL 데이터 폴더 단순 복사는 일관된 백업을 보장하지 않습니다.

## 문제 확인

- MySQL이 안 켜짐: 기존 3306 사용 여부, 디스크 여유, 쓰기 권한 및 `logs/mysql-error.log` 확인.
- 월드 시작이 거부됨: Windows 커밋 여유를 확인. 최소 8GB 여유 검사는 2,000봇 부하 보증이 아닙니다.
- 로그인은 되지만 서버 입장이 안 됨: 렐름 주소·월드 포트·Radmin/방화벽 대조.
- AI가 답하지 않음: [NVIDIA API 오류 점검](NVIDIA_API.md#오류-점검). 별도 `.world` 명령은 필요하지 않습니다.
- 서버 종료/오류: 콘솔과 `logs/Errors.log`, `logs/AuthErrors.log`, `logs/mysql-error.log`를 확인하세요. 키·개인 정보가 담긴 로그를 그대로 공개하지 마세요.

이 버전은 신규 설치 패키지입니다. 기존 캐릭터/계정 이관이나 모든 클라이언트 충돌 해결을 자동으로 수행하지 않습니다.
