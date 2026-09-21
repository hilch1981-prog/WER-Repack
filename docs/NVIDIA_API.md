# NVIDIA API 키 발급 및 입력 — 외부 LLM 봇 대화 필수

> [!WARNING]
> **🟨 WER 핵심 기능 — NVIDIA LLM 봇 대화를 꼭 사용해 보세요!**
>
> **공개 채널·귓속말·파티·공대·길드에서 봇과 자연어로 대화하는 기능을 꼭 체험해 보세요.** 본인 NVIDIA API 키를 발급·입력해야 외부 LLM 대화가 작동합니다. AI 추론은 NVIDIA 클라우드에서 처리하므로, 이 기능을 위해 로컬에 대형 모델이나 별도 추론용 GPU를 준비할 필요가 없습니다.
>
> **개발·시험용 무료 API로 시작할 수 있습니다.** NVIDIA는 현재 기본 모델의 무료 엔드포인트를 제공합니다. 무료 제공은 무제한 동시 요청이나 상시 서버 운영 보장이 아니며, 계정·모델별 한도와 약관이 적용됩니다. 운영용 서비스는 별도 이용 조건을 확인하세요.
>
> **[지금 NVIDIA 키 발급·입력하고 봇과 대화해 보기](https://github.com/hilch1981-prog/WER-Repack/blob/main/docs/NVIDIA_API.md)** · [공식 무료 모델 안내](https://build.nvidia.com/nvidia/nemotron-3-ultra-550b-a55b) · [NVIDIA 이용 약관](https://assets.ngc.nvidia.com/products/api-catalog/legal/NVIDIA%20API%20Trial%20Terms%20of%20Service.pdf)

## 무료 제공과 실제 운영의 차이

2026-09-21 확인 기준 [NVIDIA NIM 개발자 안내](https://developer.nvidia.com/nim)는 개발·시험용 무료 접근을 제공하며, 기본 모델 페이지도 Free Endpoint를 표시합니다. [시험용 약관](https://assets.ngc.nvidia.com/products/api-catalog/legal/NVIDIA%20API%20Trial%20Terms%20of%20Service.pdf) 1.1~1.4는 이용 제한 및 시험/운영 용도를 구분합니다. 무료 시험 API가 공개 게임 서버의 운영용 이용까지 허용한다고 가정하지 마세요. 계정에서 동의하는 최신 약관과 필요한 운영용 계약을 확인해야 합니다.

WER의 2,000봇 목표 수량은 API 처리량 보장이 아닙니다. 봇의 대화 빈도·동시 요청·모델 응답시간에 따라 429나 시간 초과가 발생할 수 있습니다. 먼저 소규모 대화 시험으로 응답시간과 실패율을 확인하고 빈도를 조정하세요. 이 배포에서 계정별 한도나 2,000봇 LLM 부하 시험을 검증하지 않았습니다.

특정 GPU 수량(예: RTX 5080 24개)과 동등한 성능이라는 공식 비교 근거는 확인하지 못했습니다. 클라우드에서 대형 모델을 사용한다는 사실을 사용자에게 그만큼의 GPU가 전용 할당되거나 동일한 처리량을 보장한다는 의미로 해석하면 안 됩니다.

**LLM 봇 대화를 사용하려면 본인의 유효한 NVIDIA API 키를 발급받아 입력해야 합니다.** 리팩에는 키가 없으며 제작자가 공유 키를 제공하지 않습니다. 기본 서버 기동·일반 봇 AI까지 NVIDIA 키가 필수인 것은 아닙니다.

이 안내는 NVIDIA 호스팅 API를 사용합니다. 내 PC에서 Ollama나 대형 AI 모델을 실행하는 방식이 아니며, 독립 `mod-ollama-chat` 모듈은 이 리팩에서 빌드 제외 상태입니다.

## 1. 본인 계정으로 키 발급

1. [NVIDIA Build](https://build.nvidia.com/)에 접속합니다.
2. [기본 모델 페이지](https://build.nvidia.com/nvidia/nemotron-3-ultra-550b-a55b)를 엽니다. 모델이나 이용 가능 여부는 공급자 정책에 따라 달라질 수 있습니다.
3. 모델의 API/코드 예제 영역에서 **Get API Key**를 선택합니다.
4. 본인의 NVIDIA 계정으로 로그인하거나 계정을 만들고, 표시되는 이용 조건·인증 절차를 직접 확인합니다.
5. 발급된 키를 안전한 곳에 보관합니다. GitHub, 공개 게시물, Discord, 스크린샷에 올리지 마세요.

발급 화면은 변경될 수 있으므로 [NVIDIA 공식 API Quickstart](https://docs.api.nvidia.com/nim/docs/api-quickstart)를 우선하세요. 이용 한도·크레딧·가격·모델별 권한은 본인 계정에서 확인해야 하며 영구 무료나 무제한 이용을 보장하지 않습니다.

## 2. 정확한 설정 파일 열기

리팩 폴더의 아래 파일을 텍스트 편집기로 엽니다.

```text
configs\modules\mod_wowlegends.conf
```

다음 키를 찾아 **기존 줄의 값만** 수정하세요. 중복 줄을 추가하지 않습니다.

```ini
WowLegends.AiChat.Enabled = 1
WowLegends.AiChat.Provider = "openai"
WowLegends.AiChat.ApiUrl = "https://integrate.api.nvidia.com/v1/chat/completions"
WowLegends.AiChat.ApiKey = "여기에_본인_NVIDIA_API_키_입력"
WowLegends.AiChat.Model = "nvidia/nemotron-3-ultra-550b-a55b"
```

- `Provider = "openai"`는 **OpenAI 호환 통신 형식**이라는 뜻입니다. 위 URL에서는 NVIDIA API를 이용하며 OpenAI 키를 넣는 것이 아닙니다.
- 키 앞에 `Bearer `를 붙이지 말고 발급된 키 원문만 큰따옴표 안에 입력합니다. 코드가 인증 헤더를 구성합니다.
- `mod_WER.conf`, `WER.AiChat.ApiKey`는 실제 읽는 이름이 아닙니다. 표시 브랜드와 내부 키는 다릅니다.
- 기본 모델이 계정에 제공되지 않으면 NVIDIA가 현재 제공하는 호환 텍스트 채팅 모델의 **정확한 API 모델 ID**를 사용합니다. 웹 화면의 표시 이름과 API ID가 다를 수 있습니다.
- 키가 적힌 파일을 이 GitHub 저장소나 다른 사람에게 업로드하지 마세요.

## 3. 적용 및 대화 확인

처음 실행 전에는 저장 후 MySQL → 로그인 → 월드 순서로 시작합니다. 이미 운영 중이면 이용자에게 알리고 월드 서버를 정상 종료한 뒤 `3_WORLDSERVER.bat`로 다시 시작합니다. 작업관리자로 강제 종료하지 마세요.

봇이 있는 지역에서 기본 공개 채널 `/1`, 일반 대화 `/s`, 귓속말 또는 파티/공대/길드로 짧게 말을 걸어 확인합니다. 별도의 `.world`나 ‘월드 대화 모드’는 사용하지 않습니다. 봇 존재·거리·채널·그룹·쿨다운·요청 대기열에 따라 응답이 달라질 수 있으므로 모든 발언에 즉시 응답한다는 보장은 없습니다.

기본 `WowLegends.AiChat.MaxTokens = 0`은 이 구현에서 요청의 출력 토큰 상한을 생략하는 정책입니다. 공급자의 모델 한도·시간 제한·이용량까지 무제한으로 만드는 설정은 아닙니다. 기본 TimeoutMs=12000인 만큼 응답이 오래 걸리면 시간 초과할 수 있습니다. ModelFast를 임의로 다른 공급자 모델로 바꾸지 말고 기본 단일 모델 구성을 먼저 확인하세요.

## 개인정보 및 비용

외부 LLM을 켜면 해당 기능이 구성한 **대화 내용과 캐릭터/상황 문맥이 NVIDIA로 전송**됩니다. 운영자는 참가자에게 이를 알리고 공급자의 데이터 처리·이용 조건을 확인하세요. 비밀번호·민감 개인정보를 게임 대화로 보내지 마세요. 봇끼리 대화나 공대 대화도 요청량을 늘릴 수 있습니다.

키 유출이 의심되면 공급자에서 폐기·재발급하고 로컬 파일을 갱신하세요. 제작자에게 키를 보내거나 Issues에 붙여 넣지 마세요. 키 발급이나 시험 호출을 이 배포 과정에서 대신 수행하지 않았습니다.

## 오류 점검

| 증상/응답 | 확인할 항목 |
|---|---|
| 401/403 | 키 유효성, 복사 오류, 모델 이용 권한 |
| 404/모델 오류 | ApiUrl 및 정확한 모델 ID |
| 429 | 요청량·크레딧·공급자 이용 한도 |
| 시간 초과/연결 실패 | 인터넷, 공급자 장애/대기 시간, DNS·프록시·TLS 환경 |
| 응답 없음 | Enabled/키 입력, 월드 재시작, 봇이 듣는 채널/거리/그룹 조건, 대기열 |
| 영문·부정확한 답변 | 한국어 프롬프트/로케일 정책과 모델 특성. 완전한 한국어·공략 정확성을 보장하지 않음 |

콘솔과 `logs/Errors.log`를 확인하되 로그를 공유할 때 키·개인 대화·계정·IP를 제거하세요. TLS 오류를 해결하려고 인증서 검증을 임의로 끄지 마세요.
