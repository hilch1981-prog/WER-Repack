# WER 1.1.0 대응 소스 및 빌드

`WER_Source/`는 배포 실행파일에 대응하는 전체 통합 소스 스냅샷입니다. 저장소 첫 등록 커밋은 원본 개발 이력을 대신하지 않습니다. WER는 **WOW Emulator Research(와우 에뮬레이터 연구소)**의 약자입니다. 원 AUTHORS/LICENSE와 모듈별 저작권은 보존했습니다.

- 코어 조립 기준: `06234df3d5ab26c93f4f1f06f3edb828b73ecd3c` + 기존 Legends/WER 로컬 수정.
- `SOURCE_MANIFEST.json`: WER_Source의 파일별 SHA256. upstream SHA 하나만으로 이 스냅샷을 재현할 수 없습니다.
- `WER_1.1.0.patch`: 배너·Unicode 콘솔·로그 접두사 수정 3파일의 기록. **현재 소스에는 이미 적용돼 있으므로 중복 적용하지 않습니다.**
- Releases의 `WER_Source.zip`도 같은 12,736개 소스 파일을 제공합니다.
- 소스 ZIP SHA256: `8b16f079d4aa7c5ca3a0636f8a94502d0193280318d739c53d28a4dcf458147b`.
- `packaging/`은 제작 이력 참고용입니다. 예전 로컬 경로/과거 실행기 정책을 포함할 수 있으므로 일반 이용자가 실행하는 도구가 아닙니다. 운영 DB를 이 스크립트의 대상으로 지정하지 마세요. GitHub 게시 시 예전 운영 IP가 포함된 검사문 하나를 일반 패턴으로 바꾸었으며 게임 소스/동작은 바꾸지 않았습니다.

## 빌드 환경

Visual Studio 2022 C++ x64 도구/MSVC 14.44, Windows SDK, CMake, Boost 1.84.0(msvc14.3), OpenSSL 4.x, MySQL 8.4.9 개발 헤더/라이브러리가 기준입니다. 정확한 소스는 현재 폴더를 사용합니다.

```powershell
.\build-source.ps1 -BoostRoot 'C:\SDK\boost_1_84_0' `
  -OpenSSLRoot 'C:\SDK\OpenSSL4' `
  -MySQLRoot 'C:\SDK\mysql-8.4.9' `
  -MySQLExecutable 'C:\SDK\mysql-8.4.9\bin\mysql.exe'
```

예제 경로는 직접 설치한 SDK 위치로 바꾸세요. 스크립트가 도구를 자동 설치하지는 않습니다. 전체 빌드는 CPU·메모리·시간을 소모하므로 게임 서버 테스트 중에 무단 실행하지 마세요.

독립 Ollama는 빌드 제외 옵션을 유지합니다. `Individual Progression` 시험 후보를 임의로 더하지 않습니다. 기본 DB를 직접 구축하는 경우와 실행 리팩의 적용 완료 `dump/`를 가져오는 경우는 구분해야 합니다. 운영 DB에 초기 SQL을 덮어쓰지 마세요.

OpenSSL 3으로 바꿔 빌드하면 서버 DLL도 해당 ABI에 맞춰야 합니다. MySQL 클라이언트 DLL이 요구하는 OpenSSL ABI도 별도로 확인하세요. DLL 이름만 바꾸어 대체하면 안 됩니다.

GitHub 게시 과정에서는 게임 코드를 수정·재컴파일하지 않았습니다. 사용자의 SDK 조합에서 전체 재빌드가 성공했다는 별도 보증은 아니며, 원래 리팩의 검증 범위는 루트 README에 명시했습니다.
