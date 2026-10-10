# WER 버전별 대응 소스 및 빌드

[한국어](README.md) · [English](README.en.md)

**v1.1.0 태그**의 `WER_Source/`는 배포 실행파일에 대응하는 전체 통합 소스 스냅샷입니다. 후속 소스에는 2026-10-09 안전성 개선이 포함되며 **기존 릴리즈 실행파일·소스 ZIP과 동일하지 않습니다.** 저장소 첫 등록 커밋은 원본 개발 이력을 대신하지 않습니다. WER는 **WOW Emulator Research(와우 에뮬레이터 연구소)**의 약자입니다. 원 AUTHORS/LICENSE와 모듈별 저작권은 보존했습니다.

- 코어 조립 기준: `06234df3d5ab26c93f4f1f06f3edb828b73ecd3c` + 기존 Legends/WER 로컬 수정.
- `SOURCE_MANIFEST.json`: WER_Source의 파일별 SHA256. upstream SHA 하나만으로 이 스냅샷을 재현할 수 없습니다.
- `manifests/WER_1.1.0_SOURCE_MANIFEST.json`: 후속 변경 전 릴리즈 소스 매니페스트 보존본. 현재 `SOURCE_MANIFEST.json`과 혼동하지 않습니다.
- `manifests/WER_20261009_SAFETY_DELTA.json`: 후속 변경 19파일의 이전/현재/빌드 입력 해시 및 검증 범위.
- `python tools/verify-source-manifest.py`를 **저장소 루트에서** 실행하면 현재 소스의 파일별 해시·크기·누락·추가 파일을 읽기 전용으로 검사합니다. 소스를 수정하거나 빌드·DB·서버를 실행하지 않습니다.
- `WER_1.1.0.patch`: 배너·Unicode 콘솔·로그 접두사 수정 3파일의 기록. **현재 소스에는 이미 적용돼 있으므로 중복 적용하지 않습니다.**
- Releases의 `WER_Source.zip`은 **v1.1.0 원본** 12,736개 소스 파일을 제공합니다. 후속 검토 브랜치의 19개 변경을 포함한 새 ZIP으로 교체하지 않았습니다.
- 소스 ZIP SHA256: `8b16f079d4aa7c5ca3a0636f8a94502d0193280318d739c53d28a4dcf458147b`.
- 전체 원본 트리에는 `deps/acore/mysql-tools/bin/dump-parser`(Linux ELF,13KB)와 `dump-parser-mac`(macOS Mach-O,50KB)도 포함됩니다. 대응 `dump-parser.c`와 `build-dump-parser.sh`를 함께 보존합니다. 따라서 **Windows 서버 EXE/DLL과 게임 데이터는 제외**하되 모든 파일이 텍스트라고 표현하지 않습니다. 이 두 원본 도구는 Windows 실행 리팩에 넣거나 실행하지 않습니다.
- `packaging/`은 제작 이력 참고용입니다. 예전 로컬 경로/과거 실행기 정책을 포함할 수 있으므로 일반 이용자가 실행하는 도구가 아닙니다. 운영 DB를 이 스크립트의 대상으로 지정하지 마세요. GitHub 게시 시 예전 운영 IP가 포함된 검사문 하나를 일반 패턴으로 바꾸었으며 게임 소스/동작은 바꾸지 않았습니다.

## 빌드 환경

**최신 실행 rc.3(2026-10-10)**은 아래 rc.2 변경을 계승하고 길드 이름 캐시·펫 기본 이동 C++2개와 버전 배너를 추가 수정했습니다. 현재 전체 manifest를 실제 빌드 입력과 대조하고6종을 링크했습니다. 자동 DB이관 SQL·실행기 변경은 `packaging/runtime_rc2`에 있으며 코어 manifest가 아닌 저장소 Git 이력으로 관리합니다. [RC3 검증 범위](../docs/releases/v1.1.1-rc.3.md), [옵션 원장](../docs/releases/OPTIONS_rc3.md)을 확인하세요.

실행 `v1.1.1-rc.2`는 19파일 안전성 수정과 갱신된 `Banner.cpp`를 포함합니다. [실행·검증 설명](../docs/releases/v1.1.1-rc.2.md)을 확인하세요. `WER_20261009_SAFETY_DELTA.json`은 이전 19파일 검토 기록이며 버전 배너 변경은 별도입니다. 현재 전체 원장은 `SOURCE_MANIFEST.json`입니다. 새 실행 ZIP에 전체 소스·게임 데이터를 넣지 않습니다.

Visual Studio 2022 C++ x64 도구/MSVC 14.44, Windows SDK, CMake, Boost 1.84.0(msvc14.3), OpenSSL 4.x, MySQL 8.4.9 개발 헤더/라이브러리가 기준입니다. 정확한 소스는 현재 폴더를 사용합니다.

```powershell
.\build-source.ps1 -BoostRoot 'C:\SDK\boost_1_84_0' `
  -OpenSSLRoot 'C:\SDK\OpenSSL4' `
  -MySQLRoot 'C:\SDK\mysql-8.4.9' `
  -MySQLExecutable 'C:\SDK\mysql-8.4.9\bin\mysql.exe'
```

예제 경로는 직접 설치한 SDK 위치로 바꾸세요. 스크립트가 도구를 자동 설치하지는 않습니다. 전체 빌드는 CPU·메모리·시간을 소모하므로 게임 서버 테스트 중에 무단 실행하지 마세요.
`TOOLS_BUILD=maps-only`로 authserver·worldserver·네 추출기를 x64 Release로 빌드하며 모든 컴파일/빌드 병렬도를1로 제한합니다. 실행 릴리즈와 같은 대상 목록입니다. 빌드 성공은 실제 게임이나 전체 MPQ 추출 성공과 구분합니다.

독립 Ollama는 빌드 제외 옵션을 유지합니다. `Individual Progression` 시험 후보를 임의로 더하지 않습니다. 기본 DB를 직접 구축하는 경우와 실행 리팩의 적용 완료 `dump/`를 가져오는 경우는 구분해야 합니다. 운영 DB에 초기 SQL을 덮어쓰지 마세요.

OpenSSL 3으로 바꿔 빌드하면 서버 DLL도 해당 ABI에 맞춰야 합니다. MySQL 클라이언트 DLL이 요구하는 OpenSSL ABI도 별도로 확인하세요. DLL 이름만 바꾸어 대체하면 안 됩니다.

초기 v1.1.0 GitHub 게시 과정에서는 게임 코드를 수정·재컴파일하지 않았습니다. [초기 안전성 기록](../docs/updates/2026-10-09-safety-review.md)은 당시 링크와 DB/로그인 dry-run 결과입니다. 후속 월드/프로토콜 및 최종 rc.2 실행 파일의 결과는 [rc.2 검증 원장](../docs/releases/verification_rc2.json)에서 구분합니다. 예전 실행 파일을 최종 rc.2 빌드로 혼동하지 마세요. 사용자의 SDK 조합·그래픽 게임·2,000봇 부하까지 보증하지 않습니다.
