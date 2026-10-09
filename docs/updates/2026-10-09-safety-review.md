# WER 후속 안전성 개선 — 검토용 소스

기록일: **2026-10-09 KST**. 출처 재확인: **11:30 KST**.
기준 WER 커밋: `235d99f1f55fa742834dfa1fbd21a876cc5b9365`.

**상태: 소스 준비·전체 빌드·격리 DB·auth dry-run 확인 / 월드·게임·운영 미검증.**
기존 `v1.1.0` 태그, 실행 리팩 ZIP, 소스 ZIP은 변경하지 않습니다. 이 문서는 새 실행 리팩 출시나 운영 서버 적용 완료 안내가 아닙니다.

## 무엇을 개선했나요?

| 범위 | 준비한 변경 | 주의점 |
|---|---|---|
| Playerbots 인벤토리 | 아이템 파괴·은행·전달·장비 해제·개봉·충전 소모 전 필요한 ID/수량을 복사 | 일부는 예방 보강. 실제 운영 크래시 감소는 미확인 |
| 은행 실행 결과 | 다중 작업 중 하나라도 실제 성공했으면 성공 반환 | 모든 작업의 원자적 성공을 뜻하지 않음 |
| 장비 선택 | 방문 시 GUID를 수집하고 평가 직전 재조회 | 현재 경로의 실제 무효화는 미입증. 아래 PR #2895 주의 필수 |
| 지연 봇 명령 | 발신자 포인터 대신 GUID 저장, 실행 시 온라인 상태·현재 권한 재검사 | 이후 Event/ActionBasket의 전체 수명 문제까지 해결한 것은 아님 |
| Legends | 같은 master 재선정 때 불필요한 reset·인사 반복 억제 | 새로운 master 선택·기존 BG 정책은 유지 |
| RPG 퀘스트·오라 | 사라진 퀘스트 대상·오라 원 시전자에 대한 null 방어 | 각 원본 PR은 미병합 후보 |
| Dungeon Clear | 대상/공격자가 없는 탱커가 파티 전투에 합류하도록 전투 엔진 연결 | 위협 배율 증가 또는 BWL 전체 수정이 아님 |
| 경매장 | NPC 판매/전리품 중 활성 출처 하나에 해당하면 허용 | 수량·가격·블랙리스트 등 기존 정책은 유지 |
| 광역 루팅 | 주사위·그룹 몫·퀘스트 자격·스키닝 권한 보존, 부분 수납 소실 방어 | 실제 그룹 분배·훅·복제/소실 게임 회귀는 추가 필요 |
| 길드하우스 | 새 오브젝트 생성 시 방향과 quaternion 회전 일치 | 기존 DB 오브젝트 회전을 일괄 수정하지 않음 |

총 **19개 소스 파일: C++ 16개, 헤더 3개**. 기존 한국어 문자열·로케일 처리·WER 브랜딩을 보존했습니다.
새 기능 옵션·SQL·활성 설정·운영 DB·서버 실행파일은 바꾸지 않았습니다.
봇 2,000명 목표 정책, 독립 Ollama 빌드 제외, enhanced-worldchat 비활성, 워밴드 캠프 제외,
Individual Progression 단계·배율 보류도 그대로입니다. 등록 모듈 18개가 모두 활성이라는 뜻은 아닙니다.

공개 PR에는 읽기 전용 소스 무결성 CI도 포함합니다. 고정 SHA의 공식 `actions/checkout`과
GitHub 호스팅 runner의 Python만 사용하며, 검사기 7회귀와 12,736파일 매니페스트를 확인합니다.
권한은 `contents: read`, checkout 자격증명 보존은 끕니다. **게임 빌드·DB·서버·LLM API·배포를 실행하지 않습니다.**
CI의 실제 성공 여부는 PR의 해당 commit checks를 따르며 로컬 PASS만으로 CI PASS라고 표시하지 않습니다.

## 출처와 upstream 상태

원 파일의 저작권·GPL/AGPL 고지를 유지합니다. 이 표는 **명시한 SHA의 선별 이식/보완** 기록이며 최신 upstream 전체 교체가 아닙니다.

| 변경 | 원저자·고정 출처 | 2026-10-09 재확인 상태 |
|---|---|---|
| 아이템 수명 | Terry Raimondo·Keleborn, [Playerbots #2811](https://github.com/mod-playerbots/mod-playerbots/pull/2811), `6e372473ae3dc572f623a8f492acac68f671b0e9`, `bcca361ed16ea52352c46f73b17eefe764c31896`, `197e1ad5ad9ceaee7df03c78f70d0a5f5fe7ea24`, `ba342ba6fee5e5331276961e231329f9aee16334` | test-staging 병합. WER 순서·한글화 보존 및 은행 결과는 별도 보완 |
| 장비 GUID | Avirar, [Playerbots #2895](https://github.com/mod-playerbots/mod-playerbots/pull/2895), `9d784999567e9aebcbf791193ce890216189994d` | **병합 없이 닫힘. 예방 보강만 유지**, 실제 크래시 해결 주장 제외 |
| 지연 명령 | Vezajin, [Playerbots #2880](https://github.com/mod-playerbots/mod-playerbots/pull/2880), `f5934a597fa26c24c7286044b2b8c3ced6495d6e` | test-staging 병합. WER 권한 재검사는 별도 보완 |
| 퀘스트 대상 | Avirar, [Playerbots #2898](https://github.com/mod-playerbots/mod-playerbots/pull/2898), `3a0713aa86cc0e7f4a9bc927c72bb782fa156f9e` | OPEN·미병합 |
| 오라 시전자 | Stroe, [AzerothCore #27961](https://github.com/azerothcore/azerothcore-wotlk/pull/27961), `ce76f33958e1e87b529255fd29d03128000368df` | OPEN·미병합 |
| 동일 master | WOW Legends 기여자, 공개 [v1.6.0](https://github.com/WOWLegendsHQ/wow-legends-community/releases/tag/v1.6.0) 소스 | 아카이브 자료여서 개인 작성자 이력은 확정 불가 |
| 탱커 지원 | Jared Wright, [Dungeon Clear 커밋](https://github.com/jrad7/mod-dungeon-clear/commit/be8f9d0c857c43fc94ea5dd894c014f2c8ea5332), `be8f9d0c857c43fc94ea5dd894c014f2c8ea5332` | 기본 브랜치의 2파일 선별 이식 |
| 경매 출처 | SamuelHenderson, [AH Bot #166](https://github.com/azerothcore/mod-ah-bot/pull/166), `c11d8318cbd8714a9980f9464f78e07d3d48a70a` | 기본 브랜치 반영 |
| 광역 루팅 | EricksOliveira, [AoE Loot #67](https://github.com/azerothcore/mod-aoe-loot/pull/67), `3d1466e122f2d43addb10e64b35e01025a214334` | 기본 브랜치 반영 + WER 부분 수납/원 시체 권한 보완 |
| 새 오브젝트 회전 | Yagz, [Guildhouse #86](https://github.com/azerothcore/mod-guildhouse/pull/86), `5222cfc9df02382867ef9662986764cce4550306` | OPEN·미병합 |

**#2895의 새 근거:** 원 작성자가 현재 test-staging 호출 경로에서는 수집한 가방 아이템이 삭제되지 않고,
과거 크래시가 merge artifact였다고 정정하며 PR을 닫았습니다.
이 WER 방어도 실제 무효화 경로를 게임에서 입증한 것은 아닙니다. 코드 주석을 예방 보강으로 정정했고,
기능 수정·크래시 해결로 계산하지 않습니다. 유지 여부는 실제 WER 호출 경로·비용 검토 후 결정합니다.
가짜 코어가 평가 중 아이템을 삭제하는 테스트는 원 서버의 실제 발생 증거가 아닙니다.

## 검증은 어디까지 했나요?

| 단계 | 결과 | 한계 |
|---|---|---|
| 기준/준비 소스 SHA·패치 적용/역복원 | PASS, 19파일·byte-exact | 실제 게임 입증 아님 |
| 작은 본문 발췌·stub 회귀 시험 | PASS | 실제 core concurrency·서버 ASan·게임 시험 아님 |
| 실제 C++ 번역 단위 구문·타입 검사 | 16/16 PASS | 단독으로 전체 링크 입증 아님 |
| VS2022 x64 Release 전체 auth/world 빌드·링크 | PASS, 2026-10-09 00:32 KST | 별도 작업 사본; 운영 EXE 미교체 |
| 모듈 빌드·등록 대조 | 18개 일치, 독립 Ollama 0개 | 월드 기동 시 활성 옵션 검증 아님 |
| CLI·DLL·OpenSSL provider 진단 | PASS | DLL 실제 경로는 진단 host에서 관찰. 짧은 auth 서비스 DLL 경로 표본은 못 얻음 |
| 새 격리 MySQL 8.4.9 | SQL·UTF-8 roundtrip·한국어 ICU·TLS 암호화·정상 종료 PASS | loopback 진단만. CA/호스트명 인증·외부 LLM TLS 미검증 |
| 깨끗한 v1.1.0 SQL 4개 import | exit 0·538테이블, 신규 캐릭터 0·기본 ADMIN 1 확인 | 운영 데이터 미사용, 비밀번호 로그인·모든 로케일 미검증 |
| 새 auth `--dry-run` | DB pool·한글 realm 로딩·종료 exit 0 PASS | network listen 전 종료. 외부 접속·실제 로그인 PASS 아님 |
| 월드 초기화·게임·2,000봇 부하·실제 LLM | **미실행** | 후속 검증·배포 승인 필요 |

AH 출처 512조합, 권한 guard 4,096조합, 전량 수납 guard 65,536조합, 회전 1,441각도를 확인했습니다.
이는 작은 결정 함수·stub의 조합 수이며 게임을 해당 횟수만큼 실행했다는 뜻이 아닙니다.

빌드 입력 통합 패치 SHA-256:
`bb7c3cb061016757d0f49584bd103c58797ebe97e2eb485499fb4ffa6ffdecd6`.
게시 전 장비 선택 **주석만** 정정했습니다. `WER_20261009_SAFETY_DELTA.json`은 이 한 파일의
빌드 입력 해시와 게시 해시 차이를 명시하며, C++ 동작 변경으로 표시하지 않습니다.

| 별도 빌드 산출물 | SHA-256 |
|---|---|
| authserver.exe | `e2faced9adda3919f77b3028d52f79c7d77e1bd874f12d16631d0792e4ea8e8d` |
| worldserver.exe | `9a184bbddb3e90bb9cfa533fbf54f56af32067eee22c56c1fed567de432a0f98` |

바이너리·DLL·DB·maps/vmaps/mmaps/DBC·클라이언트·개인 설정·API 키를 이 변경에 올리지 않습니다.
초기 DLL/ICU 진단 사본 누락, Windows MySQL monitor PID 분리, DB 미선택 import,
auth INFO 로그 누락에 의한 검사 실패를 보완한 후 위 PASS를 얻었습니다.
이를 기존 운영 서버 결함이 수정됐다는 증거로 바꾸지 않습니다. 자체서명 시험 CA 경고와
최소 auth 설정의 기본값 경고는 남으며, 새 의존성 보안 패치 바이너리를 적용한 것은 아닙니다.

## 다음 검증과 보류

- 인벤토리: merge/분할/파괴/다중 은행/부분 실패/마지막 충전/거래·우편 인접 경로에서 소실·복제·안내 수량 확인.
- 명령: 로그아웃·권한 철회·재접속·맵 전환 및 ownerless 내부 이벤트 회귀. Event/ActionBasket 전체 취소 설계는 별도.
- DC: 전투 flag만 남은 탱커, 원거리 몹, DC off/pause/stay/follow, 리더 교체·층/맵 차이 확인.
- AoE: solo/각 그룹 분배/열린 roll/낙찰/사망 뒤 합류/QLP/스키닝/골드 분배 확인.
  특히 퀘스트 5개·가방 공간 2개는 **0개 지급·원 시체 5개 유지**, 공간 확보 후 재루팅 확인.
- 길드하우스: 새 오브젝트 0/90/180/270도, 재접속·재시작 후 방향·충돌·길드별 공간 확인.
- 별도 보류: strict item-link 충돌, 큰 DC DPS 대기와 시간 wrap·scout 선행 기능, Ollama 통합,
  OpenSSL/MySQL 보안 바이너리 교체. 일반 upstream 최신이라는 이유로 자동 합치지 않음.
- 맥주 축제 CrashGuard, 도시 결투 실패, 달라란 클라이언트 메모리, 잔여 영어·기존 펫·길드 분포는 해결 확인된 것으로 표시하지 않음.

후속 upstream 조사는 **사용 모듈과 Legends 기반 소스 양쪽**을 다룹니다.
이전/현재 SHA·기본 브랜치/시험 브랜치/미병합 구분·한글화/브랜딩/로컬 패치 영향을 기록하고,
공개 upstream이 불명확하면 확인 불가로 남깁니다. 조사만으로 자동 업데이트·운영 재시작·API 호출하지 않습니다.
원 라이선스를 유지하며 비상업적 연구·학습 이용을 권고합니다. 법적 상업 이용 금지 조항을 추가하지 않습니다.
