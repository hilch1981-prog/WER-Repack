# WER 1.0.0 릴리즈 노트

## 2026-09-21 현재 실행방식 — 버전 1.0.0 유지 / 단순 콘솔3개

- 사용자 요청으로 개정2의 통합 메뉴/백그라운드 감시/자동재시작을 제거했습니다.
- **1_MYSQL.bat → 2_AUTHSERVER.bat → 3_WORLDSERVER.bat** 세 개만 실행하면 각각 표시되는 콘솔에서 동작합니다.
- 종료는 월드→로그인→MySQL 순서로 각 창 Ctrl+C. 실제 콘솔 표시/접속준비/정상종료를 분리 QA에서 확인했습니다.
- 한글·공백 경로 지원, 첫 DB 설치, 기존 DB 보존, 중복/시작순서/메모리 보호는 유지합니다. MySQL 최초 준비 helper 종료 뒤 콘솔 MySQL로 전환합니다.
- 게임 EXE/DLL/DB/configs/소스ZIP/봇2000/길드40×25/외부키 미포함 정책은 그대로입니다. 운영 서버는 종료 상태를 유지했습니다.
- 현재 근거는 **docs/CONSOLE_LAUNCHER.json**. docs/VERIFICATION.json은 최초 배포 검증 이력으로 그 안의 자동복구/영문경로 제한은 현재 실행방식이 아닙니다.
- 이전 메뉴형 실행기를 사용 중이라면 먼저 기존 서버/감시/DB를 모두 정상 종료하세요. 새 전체 ZIP은 새 폴더에 풀고 사용 중인 DB에 초기덤프를 다시 가져오거나 mysql/data를 삭제하지 마세요. 이전 메뉴용 실행기 보강 ZIP은 사용하지 않습니다.

## 기준

- 2026-09-21 현재 운영 배포와 동일한 최종 Release EXE를 사용. 별도의 미완성 후보 패치나 검토만 한 모듈을 섞지 않았습니다.
- worldserver SHA256: `804c55530d915d633a4c780fddd491ff3167561c2012f01f23c9c0eee575c517`
- authserver SHA256: `fada29cd9eb953bc0d1efe3d35a596a2db43f1c995b4177bc5ade89d51b70aaf`
- 코어 조립 기준 SHA: `06234df3d5ab26c93f4f1f06f3edb828b73ecd3c` + 유지된 WER/Legends 로컬 변경. 이 SHA 단독으로 배포 소스를 재현할 수 없으므로 동봉한 전체 소스를 사용하세요.
- 원본 1.5.3 리팩 구조를 계승하되 운영 데이터·비밀 키는 제외, 첫 실행 초기화 및 상대 경로 실행기를 추가했습니다.

## 실행파일에 연결된 모듈

생성된 정적 ModulesLoader 기준 18개:

| 모듈 | 배포 정책 |
|---|---|
| mod-ah-bot | 경매 기능, 첫 설치용 별도 봇 생성 |
| mod-all-flightpaths | 비행 경로 및 비행/즉시 이동 메뉴 |
| mod-aoe-loot | 광역 루팅, 시체 처리 상한 및 아이템 방어 |
| mod-dungeon-clear | 던전 공략 보조, Dungeon Lead 대신 사용 |
| mod-enhanced-worldchat | 연결되어 있으나 설정상 비활성 |
| mod-guildhouse | 길드 하우스 및 이동 메뉴, 소유권은 초기화 |
| mod-individual-xp | 개인 XP 옵션. Individual Progression과 다름 |
| mod-optimal-bot-raid | 봇 공격대 보조 |
| mod-playerbots | 플레이봇 핵심, 처음 새로 생성 |
| mod-playerbots-artisans | 전문기술 봇, 제작 광고 억제 등 기존 정책 유지 |
| mod-playerbots-city-life | 도시 생활 |
| mod-playerbots-pvp-life | 야외 PvP/도시 결투, 도시 전용 10~80 레벨 예외 |
| mod-playerbots-wintergrasp | 겨울손아귀 |
| mod-quest-loot-party | 퀘스트 파티 전리품 보조 |
| mod-quest-radar | 퀘스트 탐색 |
| mod-rndbot-sync | 랜덤 봇 동기화 |
| mod-transmog | 형상변환 |
| mod-wowlegends | Legends 통합 기능/외부 LLM/동반자 등 |

모듈이 연결되어 있다는 것은 모든 옵션이 켜졌다는 뜻이 아닙니다. 실제 옵션은 동봉된 활성 `.conf`를 기준으로 합니다. 제외: Discord, Dungeon Lead, 독립 Ollama(소스 보존/빌드 제외), 워밴드 캠프 기능 비활성. Individual Progression은 사용자 지시로 시험 준비/단계·배율 적용 보류이며 이 실행파일에 통합된 것으로 표시하지 않습니다.

## 계승한 최근 수정

- 재회 인사/기간/지역명, 비행 NPC 메뉴 fallback 로케일 보완.
- 전문기술 장비 장착 조건 검사와 비정상 기존 장비 보존 처리.
- 전술/대기/장비 시스템 응답 억제, 펫 이름 로케일 및 대사 다양화, 길드/공대 대화 정책.
- 월드 PvP/도시 결투 준비 수정, 실제 봇 경매 이용, 로케일 참조 복원 가능 보관 테이블.
- 운영자 전용 데이터 없이 한글 정적 DB와 현행 서버 옵션 승계.

## 알려진 제한 / 확인 필요

- 기존 startup 오류: 아이템 45280/46104의 DURATION_REAL_TIME/Duration 불일치, GO254605의 `gobject_challenge_modes` 스크립트 누락, NPC16320 waypoint path 0. 이 리팩 제작에서 해결한 것으로 주장하지 않습니다.
- 모든 콘텐츠 완전 한글화 보증 아님. 템플릿 우편 등 잔여 영문 및 LLM의 외부 답변을 전수 검증하지 않았습니다.
- 검은날개 둥지 이동/위층 보스/탱커 위협수준과 LLM 발언→실제 행동 연결은 별도 미완료 사항입니다.
- 사용자 제보 CombatStop/DBC store 패치는 사용자가 취소한 범위여서 추가 적용하지 않았습니다.
- 달라란 클라이언트 M2Shared.cpp 메모리 오류, 재로그인 '무엇인가' 표시는 모든 클라이언트 환경에서 해결 확인된 문제가 아닙니다.
- 길드하우스 메뉴/구매/길드별 분리는 이전 운영 테스트에서 사용자 확인. 새 배포 PC에서의 게임 테스트를 대신하지 않습니다.
- 첫 실행/분리 QA는 적은 봇 수로 시행합니다. 2,000봇 동시 부하·장시간 안정성·외부 PC 로그인·전체 레이드 검증은 별도입니다.
- API 키 미포함. 외부 LLM 실호출은 이 깨끗한 배포본 QA에서 수행하지 않습니다.

## 저작권

루트 및 모듈별 LICENSE/README의 원 저작권을 유지합니다. 소스와 수정된 빌드 입력을 동봉하며 독립 Ollama를 다시 활성화하면 해당 AGPL 조건도 확인해야 합니다. MySQL/OpenSSL/VC Runtime 라이선스 및 재배포 고지는 `licenses`와 `mysql/LICENSE`를 참조하세요. 게임 클라이언트는 별도 소유/설치가 필요합니다.
