# WER REPACK VER1.1.0

**와우 리치왕 3.3.5a · 빌드 12340 · 한국어(koKR) · Windows x64**  
**와우 에뮬레이터 연구소 — WOW Emulator Research (WER) / 제작일: 2026년 9월 21일**

**WER는 WOW Emulator Research의 약자이며, 리팩의 이름은 WER Repack입니다.**

AzerothCore, Playerbots, WOW Legends 기반 소스와 여러 커뮤니티 모듈을 통합·보완한 한국어 서버 연구용 리팩입니다. 원 코어·모듈 저작자를 대체하거나 Blizzard의 공식 제품임을 뜻하지 않습니다.

> **비상업적 연구·학습용 배포입니다. 유료 판매·유료 운영을 권장하지 않습니다.** 각 구성요소의 사용·수정·배포 권리는 해당 원본 라이선스에 따르며, 이 안내는 GPL/AGPL이 허용하는 권리를 제한하는 추가 라이선스 조항이 아닙니다. [라이선스와 출처](LICENSES.md)를 읽어 주세요.

## 다운로드와 필수 안내

- **게임 서버 실행용:** [v1.1.0 Releases](https://github.com/hilch1981-prog/WER-Repack/releases/tag/v1.1.0)의 **`WER_REPACK_VER.1.1.0.zip` 하나**를 받아 해제하세요. 선택용 분할 파일 `.zip.001`/`.002`도 같은 내용입니다. GitHub의 자동 생성 **Source code ZIP은 실행용 리팩이 아닙니다.**
- **소스:** 이 저장소의 [source/WER_Source](source/WER_Source) 또는 Releases의 `WER_Source.zip`. 소스는 배포 실행파일에 대응하는 통합 스냅샷입니다.
- **설치/접속/종료:** [설치 및 실행 가이드](docs/INSTALL.md).
- **LLM 봇 대화를 사용하려면 NVIDIA API 키 발급·입력이 필수입니다:** [NVIDIA API 설정 가이드](docs/NVIDIA_API.md). API 키는 배포하지 않습니다. 키 없이도 기본 서버·일반 봇 기능은 실행할 수 있지만 외부 LLM 대화는 사용할 수 없습니다.
- **처음 설치용입니다.** 기존 서버 폴더나 `mysql/data`에 덮어쓰지 마세요. 운영 데이터 이관은 별도 작업입니다.

## 실행은 3개만

압축을 모두 푼 뒤 순서대로 더블클릭하세요.

1. **`1_MYSQL.bat`** — 최초 DB 설치 후 MySQL 시작. `ready for connections`를 기다립니다.
2. **`2_AUTHSERVER.bat`** — 로그인 서버 시작.
3. **`3_WORLDSERVER.bat`** — 월드 서버 시작. 처음에는 봇 생성 때문에 시간이 걸립니다.

세 콘솔 창을 열어 두세요. 별도 Python 실행, 통합 메뉴, 백그라운드 감시는 필요하지 않습니다. `authserver.exe`/`worldserver.exe`를 직접 실행하기보다 배치를 사용해야 경로와 설치 준비가 맞습니다.

**기본 계정: `admin / admin` (GM 3). 외부 접속을 허용하기 전에 반드시 비밀번호를 변경하세요.**

종료는 **월드 → 로그인 → MySQL** 순서로 각 콘솔에서 `Ctrl+C`를 누르고 종료를 확인합니다. 월드 저장 전에 MySQL을 끄거나 작업관리자로 강제 종료하지 마세요.

## 주요 기능

| 분야 | 제공 내용 |
|---|---|
| 한국어 환경 | koKR DB, 봇·길드 이름 및 대사 보완, 아이템/지역/펫 로케일 처리, 한국어 시작 배너와 콘솔 출력 |
| 플레이봇 | 첫 기동 시 새 랜덤봇 생성, 목표 온라인 2,000명 설정, 파티/공격대 및 동반자 기능 |
| 봇 생태계 | 도시 생활, 야외 PvP·도시 결투, 겨울손아귀, 랜덤봇 동기화, 일부 봇의 길드 가입 정책 |
| LLM 대화 | NVIDIA 등 OpenAI 호환 외부 API 연동, 기본 공개 채널 `/1`·일반 대화 `/s`·귓속말·파티·공대·길드 대화 정책, 한국어 응답 지침 |
| 던전/공격대 | Dungeon Clear 던전 진행 보조, Optimal Bot Raid 공격대 보조 |
| 길드하우스 | 길드별 하우스 구매·이용, 비행 조련사 NPC를 통한 이동 메뉴. 길드 미가입자는 이용 제한 |
| 이동 편의 | 비행 경로 해금, 비행/즉시 이동 선택 관련 메뉴 |
| 전리품/퀘스트 | 광역 루팅, 파티 퀘스트 전리품 보조, 퀘스트 탐색 |
| 경제 | 경매장 봇, 플레이봇 경매 이용 및 전문기술 봇 |
| 외형/성장 | 형상변환, 개인 경험치 옵션 |

설정된 봇 수는 목표값이며 접속 즉시 2,000명이 모두 나타난다는 뜻은 아닙니다. 길드는 최대 40개×25명 정책이며 무소속 봇도 남습니다. 첫 설치 DB에는 운영자의 기존 캐릭터·대화 기억·경매·길드 소유권을 넣지 않았습니다.

## 포함 모듈과 제외 기능

실행파일 등록 목록은 다음 **18개**입니다. 등록됐다는 사실과 모든 옵션이 활성화됐다는 사실은 다릅니다.

| 모듈 | 역할 / 정책 |
|---|---|
| `mod-playerbots` | 플레이봇 핵심 |
| `mod-wowlegends` | 기반 통합 기능·동반자·외부 LLM·월드 PvP. 내부 식별자는 호환성을 위해 유지 |
| `mod-ah-bot` | 경매장 서비스 |
| `mod-all-flightpaths` | 비행 경로·이동 메뉴 |
| `mod-aoe-loot` | 광역 루팅 |
| `mod-dungeon-clear` | 던전 진행 보조 |
| `mod-guildhouse` | 길드하우스 |
| `mod-individual-xp` | 개인 경험치 옵션 |
| `mod-optimal-bot-raid` | 공격대 봇 보조 |
| `mod-playerbots-artisans` | 전문기술 봇·불필요한 제작 광고 억제 |
| `mod-playerbots-city-life` | 도시 생활 |
| `mod-playerbots-pvp-life` | 야외 PvP·도시 결투 |
| `mod-playerbots-wintergrasp` | 겨울손아귀 |
| `mod-quest-loot-party` | 퀘스트 파티 전리품 보조 |
| `mod-quest-radar` | 퀘스트 탐색 |
| `mod-rndbot-sync` | 랜덤봇 동기화 |
| `mod-transmog` | 형상변환 |
| `mod-enhanced-worldchat` | 실행파일에는 등록됐지만 **설정상 비활성**. 기본 WoW 채널을 사용 |

**제외/보류:** Discord Chat, Dungeon Lead, 독립 Ollama 빌드, 워밴드 캠프 기능은 사용하지 않습니다. `Individual Progression`은 시험 준비/단계·배율 적용 보류이며 포함된 `Individual XP`와 다른 기능입니다. 미완료 후보 모듈을 설치 완료로 표시하지 않습니다.

## 시스템 준비

- Windows x64, 적법하게 보유한 **한국어 WoW 3.3.5a(12340) 클라이언트**. 클라이언트 본체는 포함하지 않습니다.
- 로컬 쓰기 가능한 폴더와 충분한 디스크 공간. 압축·해제·백업용 여유 공간을 함께 확보하세요.
- 월드 시작 전 **Windows 커밋 여유 약 8GB 이상** 보호검사가 있습니다. 이는 2,000봇 장시간 구동의 충분 조건이나 전체 RAM 권장량을 보증하지 않습니다.
- MySQL 기본 포트 3306을 사용하는 다른 서버가 있다면 충돌을 먼저 해결하세요. 임의로 기존 DB를 종료·삭제하지 마세요.
- NVIDIA 호스팅 API를 사용할 때에는 인터넷 연결과 본인 API 이용 권한이 필요합니다. 로컬에 해당 AI 모델이나 고용량 GPU를 설치하는 방식이 아닙니다.

## 검증 범위와 알려진 제한

배포 제작 과정에서 최초 DB 설치, 축소 봇 수 기동, 세 콘솔 표시, 한국어 출력, 정상 종료 및 패키지 무결성을 확인했습니다. **2,000봇 장시간 부하, 모든 던전·레이드, 모든 새 PC의 외부 접속 또는 NVIDIA 실호출을 이 배포판의 검증 완료로 주장하지 않습니다.**

- 잔여 영문/외부 LLM 응답의 완전 한글화는 보증하지 않습니다.
- 검은날개 둥지 이동·위층 보스·탱커 위협수준과 LLM 발언→실제 행동 연결은 추가 검증이 필요합니다.
- 달라란 `M2Shared.cpp` 메모리 오류, 재접속 시 이름이 ‘무엇인가’로 보이는 현상은 모든 클라이언트에서 해결 확인된 문제가 아닙니다.
- 아이템 45280/46104 기간 속성 경고, GO254605 스크립트 누락, NPC16320 경로 관련 시작 로그는 잔여 항목입니다.
- 길드하우스 메뉴/구매/길드별 분리는 이전 운영 테스트에서 사용자 확인을 받았지만 새 PC 검증을 대신하지 않습니다.

버그 제보는 [Issues](https://github.com/hilch1981-prog/WER-Repack/issues)에 버전, 재현 순서, 관련 로그를 첨부하세요. **API 키·비밀번호·개인 IP·계정 데이터는 반드시 제거하세요.**

## 소스와 출처

기반 코어 SHA는 `06234df3d5ab26c93f4f1f06f3edb828b73ecd3c`이며 로컬 통합 수정이 더해져 있습니다. 해당 SHA만 받으면 동일한 리팩이 되는 것이 아닙니다. [대응 소스 및 빌드 안내](source/README.md), 파일별 `SOURCE_MANIFEST.json`, WER 1.1.0 패치를 함께 제공합니다. 이번 GitHub 게시를 위해 게임 코드를 재컴파일하거나 운영 DB를 변경하지 않았습니다.

원 저작권·GPL/AGPL 등 라이선스는 각 구성요소에 보존합니다. [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk), [Playerbots](https://github.com/mod-playerbots/mod-playerbots), [WOW Legends](https://github.com/WOWLegendsHQ/wow-legends-community)와 각 모듈 기여자에게 감사드립니다. 위 링크는 프로젝트 소개이며 정확한 파일 출처와 버전은 동봉 소스·매니페스트·각 모듈 고지를 우선합니다.
