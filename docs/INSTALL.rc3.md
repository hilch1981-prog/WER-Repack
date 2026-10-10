# WER REPACK 1.1.1-rc.3 — 빠른 실행 / Quick start

Windows10/11 x64 · WotLK3.3.5a12340 · koKR · **새 폴더 설치용**. 필요한 네이티브 DLL은 함께 제공하며 Visual Studio·Python·Go는 실행에 필요하지 않습니다.

1. 실행 ZIP을 쓰기 가능한 새 로컬 폴더에 완전히 풉니다. 소스 ZIP은 별도입니다.
2. [본인 클라이언트 데이터 추출](DATA_EXTRACTION.md) 후 `data/`에 준비합니다. 지도·DBC·MMAP은 배포하지 않습니다.
3. **`1_MYSQL.bat` → `2_AUTHSERVER.bat` → `3_WORLDSERVER.bat`** 순서로 실행하고 콘솔3개를 열어 둡니다.
4. 처음 DB 가져오기·봇 생성은 기다리세요. 이후 재시작은 DB 재설치가 아닙니다. 오류가 나도 `mysql/data`를 지우지 마세요.
5. 종료는 **월드 → 로그인 → MySQL** 순서로 Ctrl+C 후 저장·종료를 확인합니다.

기본 게임 계정은 **admin/admin(GM3)**입니다. 외부 접속 전에 비밀번호를 바꾸세요. 게임 암호는 월드 콘솔에서 `account set password admin NEW_PASSWORD NEW_PASSWORD`로 변경합니다. 예시 암호를 그대로 쓰지 마세요.

| 설정 | 기본값 |
|---|---|
| MySQL | localhost3306, 외부 공개 금지 |
| 로그인 / 월드 | 3724 / 59823 |
| 접속 주소 | Radmin26.* 자동 감지, 없으면127.0.0.1 |
| 봇 목표 | 2,000, 일부 길드 가입 |
| 월드 시작 보호 | 실제 Windows 커밋 여유16,384MiB 이상 |
| 일반 동시 사용자 | 기존 PlayerLimit=1 유지, GM 제외 |

커밋 여유는 물리 RAM 여유와 다릅니다. 이 기준도 2,000봇 용량 보증은 아닙니다. 부족하면 실행을 거부하며 OS·페이지파일·다른 프로그램을 변경하지 않습니다. 보호 기준을 낮춰 우회하지 마세요.

DB 암호는 **첫 실행 전** `scripts/settings.json`의 `DbPassword`를 본인 값(영문·숫자·밑줄·하이픈12~80자)으로 바꿉니다. 설치 후 JSON만 바꾸는 것은 실제 MySQL 암호 변경이 아닙니다. 실제 계정과 설정을 함께 변경해야 합니다. `RealmAddress`·포트도 이 파일에서 정할 수 있습니다. 다른 DB를 강제로 종료하지 마세요.

RC3의 세 가지 DB 보완은 실행 준비 시 자동 적용됩니다. 대상DB/파일해시/스토리지엔진을 확인하며 대상 행 백업·적용 기록은 `logs/migrations`에 남습니다. 로그를 공유하기 전 개인정보를 확인하세요. 기존 사용자 문구는 보존합니다. **기존 운영 서버 전체 마이그레이션 도구는 아닙니다.**

🟨 **NVIDIA 외부 LLM 대화**: [API 키 설정](NVIDIA_API.md)을 따라 `configs/modules/mod_wowlegends.conf`의 `WowLegends.AiChat.ApiKey`에 본인 키를 입력합니다. 기본 서버/일반 봇에는 필수가 아니며 무료 제공·한도는 공급자 조건에 따릅니다. 내부 키 이름은 호환성을 위해 유지합니다.

문제는 콘솔과 `logs/Errors.log`, `AuthErrors.log`, `mysql-error.log`, `watchdog.log`를 확인하세요. 파일명이 watchdog인 준비 기록은 자동 백그라운드 감시기가 아닙니다. 기존 설치에 덮어쓰지 말고 운영 계정·캐릭터·설정을 별도 백업하세요.

## English

Extract the runnable ZIP into a **new writable folder**, supply your own extracted client data, and run **1_MYSQL → 2_AUTHSERVER → 3_WORLDSERVER**. Keep all three foreground consoles open. Stop in reverse order with Ctrl+C and wait for saves. Restarting does not recreate the database.

Change the public game default **admin/admin** before remote access. Choose your own database password in `scripts/settings.json` **before first startup**. Editing that JSON after initialization does not rotate MySQL passwords. Never delete `mysql/data` to fix errors.

MySQL binds to loopback3306. Auth/world use3724/59823; realm address auto-detects Radmin or falls back to127.0.0.1. The inherited ordinary-player limit is1(GMs exempt). The bot target remains2,000, **not a verified concurrent-load guarantee**. World startup requires at least16,384MiB of free commit capacity (not free physical RAM); no OS/pagefile change is made.

RC3 migrations run during MySQL/auth preparation after database ownership, hash and storage-engine checks. Before/after receipts are saved under `logs/migrations`. They are not a complete migration tool for arbitrary existing servers.

NVIDIA cloud chat needs your own key; basic server and ordinary bots do not. See [English NVIDIA guide](NVIDIA_API.en.md). Original licenses/credits apply. Game data, private databases and API keys are not distributed.
