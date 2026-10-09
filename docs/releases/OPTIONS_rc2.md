# rc.2 옵션·동작 변경 원장 / Configuration ledger

운영 서버 옵션은 바꾸지 않는다. 아래는 불변 v1.1.0 입력에서 새 **배포 사본**으로 계승/변경하는 사항이다.

| 대상 | 이전 | rc.2 / 이유 |
|---|---|---|
| 시작 배너 | VER1.1.0 / 한국 에뮬레이터 연구소 / 2026-09-21 | VER1.1.1-rc.2 / 와우 에뮬레이터 연구소 / 2026-10-09 |
| DB realm.name | 이전 배포 SQL 명칭 | 첫 설치/로그인 준비 시 와우 에뮬레이터 연구소 |
| DB realm.address | Radmin 감지 또는 loopback | 동일, settings의 RealmAddress 사용 |
| DB realm.localAddress | 고정127.0.0.1 | 감지/지정한 address와 동일, 정상 Radmin 시험 정책 |
| SQL client defaults | defaults-extra-file 및 외부 기본값 상속 가능 | 자기 defaults-file + no-login-paths + 명시 loopback/설정포트, 타 설치 자격증명 상속 금지 |
| SQL 접속 대상 검증 | 실행파일/포트 존재만 확인 | 모든 SQL 전에 소유 exe의 loopback listener PID 및 실제 HEX(@@datadir)/@@port 대조, 다르면 쓰기/종료 차단 |
| 한글 DB 경로 확인 | UTF-8 문자열 가정으로 Windows CP949 경로 비교 실패 | HEX 경로 바이트를 실제 Windows GetACP로 복원하고 예상 데이터 폴더와 정확히 비교; 검사 생략 없음 |
| 준비 중 실패 | 임시 MySQL 잔존 가능 | 이번 실행이 시작한 임시 DB만 identity 확인 후 정상 SHUTDOWN 시도; 임의 process 강제 종료 금지 |
| 기본 DB 암호 변경 안내 | 일반 변경 권고 | 첫 설치 전에 settings.json에서 선택, 설치 후 JSON만 변경해도 실제 계정 암호는 바뀌지 않음 |
| DB 기본 포트 | 3306 | 동일. 실제 QA만13309 사용, 배포 기본에 시험 포트 넣지 않음 |
| 로그인 / 월드 | 3724 / 59823, bind0.0.0.0 | 동일, 방화벽 자동 변경 없음 |
| AiPlayerbot.MinRandomBots/MaxRandomBots | 2000 / 2000 | 동일 |
| AiPlayerbot.RandomBotAccountCount | 0, 자동 계산 | 동일 |
| AiPlayerbot.AddClassAccountPoolSize | 50 | 동일 |
| AiPlayerbot.DeleteRandomBotAccounts | 0 | 동일, 기존 봇 계정 삭제 정책 추가 없음 |
| AiPlayerbot.EnablePeriodicOnlineOffline | 0 | 동일 |
| 길드 | 부분 가입40길드×25명 상한 정책 | 동일, 초기 DB에는 길드/봇 없음 |
| WorldPvP | 활성 | 동일. 등록/옵션과 전체 게임 검증은 구분 |
| enhanced-worldchat | 비활성 설정 | 동일, 실행파일 등록만 유지 |
| 독립 Ollama | 빌드 제외 | 동일, 실행 ZIP에서 쓰이지 않는 mod_ollama_chat.conf도 제외 |
| warband camp / Individual Progression | unused / 단계·배율 hold | 동일, 임의 복원 없음 |
| WowLegends.AiChat.ApiKey | 사용자 직접 입력 | 배포 사본 공백, 운영키 복사 금지 |
| LLM 활성·모델·빈도·토큰·모든 기타 모듈 옵션 | 기존 공개 배포 설정 | 의미 변경 없음. 실 API 미호출 |
| 월드 커밋 guard | 기본8192MB | 동일, 최소값을 몰래 낮춰2000시험하지 않음 |
| PlayerLimit | 1(일반 계정 동시1명, GM제외) | 현재 계승. 변경 선택을 사용자에게 질문, 미응답 시 임의 확장하지 않음 |

시험 환경만 Min/MaxRandomBots=20, AddClassAccountPoolSize=0, AiChat.Enabled=0/키공백, 루프백 DB13309 및 Radmin realm 주소를 쓴다. 2000봇/실LLM 검증으로 혼동하거나 시험 계정/캐릭터를 배포하지 않는다. 데이터 추출기를 함께 제공하지만 자동 GUI 런처·playermap 구현은 보류다.

## English

This ledger applies to new distribution copies only, not production. Branding and realm/local-address consistency are updated; the SQL client is isolated from global defaults/login paths. Every statement first checks the owned loopback listener PID, actual data directory and port. Native Windows path bytes are retrieved as HEX and decoded using the system ACP, fixing Korean-path checks without bypassing them. Existing bot/guild/PvP/LLM policies, exclusions and the 8192-MB memory guard are retained. Only validation uses 20 bots, a private alternate DB port and disabled live LLM. Test accounts/data are never packaged. The inherited PlayerLimit=1 is explicitly recorded pending the user's concurrency preference. Standard extraction tools are supplied, not a new GUI launcher/playermap implementation.
