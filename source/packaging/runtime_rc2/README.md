# v1.1.1-rc.2 실행 패키징

불변 v1.1.0 ZIP의 DB·설정·런타임 라이선스를 입력으로 사용한다. 새 auth/world 및 네 추출기는 공개 소스와 맞춘 별도 빌드 산출물이다.

`wer-console-prepare.ps1`은 기존 세 콘솔 실행기의 DB 준비 코드다. realm 이름을 와우 에뮬레이터 연구소로, `localAddress`도 감지한 Radmin 주소와 동일하게 맞춘다. MySQL 클라이언트는 자기 defaults 파일과 명시한 loopback 포트만 사용하며 다른 설치의 global defaults/login-path 자격증명을 읽지 않게 한다. DB 설치/계정 생성 원본과 메모리 보호는 유지한다. 공개 기본 DB 비밀번호는 사용자가 변경해야 하며 운영 비밀값이 아니다.

`1_MYSQL.bat`, `2_AUTHSERVER.bat`, `3_WORLDSERVER.bat`는 실행 ZIP에서 생성되며 서버를 포그라운드 콘솔로 실행한다. 메뉴/백그라운드 감시/운영 서버 마이그레이션은 추가하지 않는다.

게임 추출 데이터와 전체 소스는 실행 ZIP에서 제외한다. 사용자는 합법적으로 확보한 3.3.5a/12340 클라이언트에서 추출한다. 전체 소스는 동일 태그의 별도 자산으로 제공한다. 기존 배포본의 `mysql/data`, API 키, 운영 설정, 사용자 캐릭터, 로그는 복사하지 않는다.

The three foreground console launchers retain the original first-install flow and commit-memory guard. The realm name/Radmin address are corrected, and the SQL client uses only its own defaults plus an explicit loopback endpoint without inheriting global defaults/login-path credentials. Client data and full source are not bundled in the runnable ZIP; matching source is a separate asset. Do not run this helper directly against production.
