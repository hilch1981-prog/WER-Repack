# RC3 옵션·DB 변경 원장 / Option and database ledger

Date:2026-10-10. Baseline:v1.1.1-rc.2.

| 항목 | 이전 | RC3 | 이유 |
|---|---|---|---|
| `MinFreeCommitMB` | 키 없음, 코드 기본8192 | 16384, 코드 하한16384 | 2,000봇 시험의 측정된 여유 부족을 반영 |
| 커밋 여유 측정 | `Win32_OperatingSystem.FreeVirtualMemory` | `Win32_PerfFormattedData_PerfOS_Memory.CommitLimit - CommittedBytes` | 실제 Windows 커밋 용량을 직접 비교 |
| 봇 최소/최대 | 2000/2000 | 변경 없음 | 사용자 목표 보존 |
| `wl_equip_item_notice` | 누락 시 fallback | 누락된 경우에만 DB base+text_loc1 추가 | 한국어 DB 조회 경로 보완 |
| item45280/46104 | 정확히 flagsCustom1,duration0 | flagsCustom0,duration0 | 모순 flag만 교정, 사용자 변경값 보존 |
| creatureGUID82897,id16320 | 경로 없는 MovementType2 | 해당 조건일 때만0 | 없는 경로 요청 방지, idle fallback |

SQL은 해시 고정된 `source/packaging/runtime_rc2/migrations/rc3`에서 로드합니다. 테이블이 InnoDB가 아니면 변경 전 중단합니다. 삽입ID·이전값·이후값은 실행기 `logs/migrations/*.json` 영수증으로 보존합니다. rollback은 이를 대조한 뒤 서버를 종료하고 별도로 실행해야 합니다.

No new enabled module, external paid API invocation, OS/pagefile change, bot-account reset, Spell.dbc translation, or Individual Progression stage/multiplier activation. The standalone Ollama/enhanced-worldchat/warcamp exclusions remain unchanged. Runtime ZIP remains fresh-install-only; existing deployment changes require its own backup and verification.
