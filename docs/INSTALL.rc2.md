# WER REPACK VER.1.1.1-rc.2 — 빠른 시작 / Quick start

와우 에뮬레이터 연구소(WOW Emulator Research), WotLK **3.3.5a / 12340 / koKR**, Windows x64. **실행 사전 릴리즈**이며 기존 서버에 덮어쓰는 업데이트가 아닙니다.

1. 실행 ZIP을 **새 쓰기 가능한 로컬 폴더**에 완전히 풀어 주세요. 소스 ZIP은 실행에 필요하지 않습니다.
2. `DATA_EXTRACTION.md`에 따라 본인 클라이언트에서 데이터를 추출해 `data/`에 준비하세요. 게임 지도/DBC는 배포본에 없습니다.
3. `1_MYSQL.bat` → `2_AUTHSERVER.bat` → `3_WORLDSERVER.bat` 순서로 실행합니다. **세 콘솔 창**을 열어 둡니다. 메뉴나 백그라운드 감시기가 아닙니다.
4. 첫 DB 설치·봇 생성은 기다려야 합니다. 재시작은 DB 재초기화가 아닙니다. `mysql/data`를 삭제하지 마세요.
5. 종료는 월드 → 로그인 → MySQL 순서로 **Ctrl+C**, 저장·종료 완료를 기다립니다.

기본 게임 계정은 **admin / admin (GM3)**입니다. 일반 유저와 공유하지 말고 외부 접속 전에 바꾸세요. 경매관리인은 별도 서비스 계정, 플레이봇은 첫 기동 때 생성합니다. 운영 계정·캐릭터·길드하우스 소유권을 가져오지 않습니다.

| 항목 | 기본값 / 정책 |
|---|---|
| DB | MySQL 8.4.9, loopback 3306, 외부 공개 금지 |
| 로그인 / 월드 | 3724 / 59823 |
| realm | Radmin 26.* 자동 감지, 없으면 127.0.0.1 |
| 봇 | 목표 2,000, 일부 길드 가입, 기존 기능/제외 정책 계승 |
| 시작 보호 | Windows 커밋 여유 기본 8,192 MB |
| 일반 유저 상한 | 기존 PlayerLimit=1 계승(GM 제외), 필요 시 worldserver.conf에서 운영자가 선택 |

포트 충돌 시 다른 DB를 강제 종료하지 말고 새 사본의 `scripts/settings.json`을 조정하세요. 공개 기본 DB 자격증명도 변경해야 합니다. `RealmAddress`를 직접 지정하면 DB 주소가 같이 맞춰집니다. 정상 Radmin 접속은 서버의 실제 주소를 사용하고 `realmlist.wtf`도 같은 주소로 설정합니다. 방화벽/포트 포워딩은 자동 변경하지 않습니다.

🟨 **NVIDIA 외부 LLM 대화를 꼭 시험해 보세요.** 본인 키를 `NVIDIA_API.md`에 따라 `configs/modules/mod_wowlegends.conf`의 `WowLegends.AiChat.ApiKey`에 입력합니다. 내부 이름은 호환성을 위해 유지합니다. 키는 포함하지 않으며 기본 서버/일반 봇 AI에는 필수가 아닙니다. 무료 접근·한도는 공급자 정책에 따릅니다.

콘솔과 `logs/Errors.log`, `logs/AuthErrors.log`, `logs/mysql-error.log`를 확인하고 키·비밀번호·개인정보를 제거한 후 공유하세요. 기존 서버를 백업·마이그레이션 검토 없이 덮어쓰지 않습니다. 원 라이선스·저작권을 보존하며 비상업적 연구·학습 이용을 권고합니다.

## English

Extract into a **new writable folder**. Full source is a separate asset. Prepare lawfully obtained client data using DATA_EXTRACTION.md, then start **1_MYSQL → 2_AUTHSERVER → 3_WORLDSERVER**, keeping all three foreground consoles open. First installation/bot creation take time. Restarting does not reinstall the DB. Stop **world → auth → MySQL** with Ctrl+C and wait for saves.

Login is **admin / admin (GM3)**; change public defaults before remote access. MySQL is loopback-only. Use the server's detected Radmin address, or configure RealmAddress explicitly. Do not stop another DB to resolve port conflicts. World startup requires approximately 8 GB of free Windows commit capacity. The 2,000-bot target is not a measured sustained-load guarantee.

The inherited PlayerLimit is 1 ordinary simultaneous user (GMs exempt). Operators can explicitly choose another limit in worldserver.conf after considering capacity.

NVIDIA LLM chat is a key optional feature: enter **your own** API key in `WowLegends.AiChat.ApiKey`. It is not required for basic startup; no key is included. Provider availability/limits apply. This fresh-install prerelease is not an in-place migration or proof of graphical client stability/live LLM behavior.
