# macOS 반영 대기·차이 기록

## 상태와 기준

- 2026-11-17: 사용자 요청으로 Windows 동결과 macOS 우선 개발 제한을 해제했다. 양쪽 플랫폼 모두 수정할 수 있다. 새 macOS 반영 대기는 이 문서에, Windows 반영 대기는 [Windows 반영 대기](windows-catchup.md)에 기록한다.
- 아래 기존 차이는 이전 macOS 동결 시 작성한 기록이다. 2026-11-13 macOS 동결 해제와 이번 양쪽 개발 허용으로 기존 차이를 해결·검증 완료 처리하지 않는다. 당시의 수정 금지와 별도 예외 승인 지시는 더 이상 적용하지 않는다.
- 동결 시작 시점의 코드 기준: `b1db15e9e1e5d697fec39d1754344d9b7ba6fc26`.
- 정책: [AGENTS.md](../../AGENTS.md). 목표 동작: [현재 기능 명세](current-spec.md).
- 기존 macOS CI는 유지한다. CI 통과는 해당 테스트 범위의 결과이며, Windows와의 기능 동등성이나 실제 Mac에서의 수동 검증을 보장하지 않는다.
- 초기 기록은 코드와 테스트 정의의 제한적인 정적 비교 결과다. 이 작업에서 앱 빌드, 테스트 실행, 실제 OS에서의 재현, 최신 CI 결과 확인은 수행하지 않았다. 아래 목록은 전체 차이 목록이 아니다.

## 이전 Windows 기준 개발 당시의 반영 대기

이전 macOS 동결 기간에 새로 확정된 플랫폼 공통 변경은 등록되지 않았다. 아래 기존 차이는 목표 동작을 확정한 변경과 구분하여 검토한다. 대기 항목이 없다는 사실은 기능 동등성을 뜻하지 않는다.

## 동결 시작 시 확인된 차이 — 검토 대기

### MAC-001: 설정 입력 검증의 차이

- 상태: 코드상 차이 확인 / 공통 정책 결정 및 실행 검증 대기.
- 변경 이유·영향: 같은 버전 1 JSON도 플랫폼별 허용 입력이 달라 로드 결과가 다를 수 있다. 설정 호환성 검토가 필요하다.
- 근거: Windows의 `validate`는 호스트 이름이 빈 문자열인지만 검사하고, 선택 필드 `user`와 `identityFile`의 빈 문자열을 허용한다. macOS의 `Host.validate`는 공백뿐인 이름과 명시적인 빈 `user`·`identityFile`을 거부한다. Windows의 `writeConfig`는 빈 선택 필드를 생략하므로, 모든 Windows 저장 파일이 macOS에서 거부된다는 의미는 아니다. 토큰의 공백·제어문자 검사 범위도 Windows의 바이트 검사와 macOS의 Unicode 문자 집합 검사로 다르다.
- 기대 동작: 공통 설정의 허용·거부 기준을 먼저 확정한다. Windows에서 허용한다는 이유만으로 macOS 검증을 완화하지 않는다. 현재 명세의 손상 파일 보존 요구는 유지한다.
- 관련 코드: [Windows core.cpp](../../windows/src/core.cpp)의 `stringValue`, `validToken`, `validate`, `parseConfig`, `writeConfig`; [macOS Configuration.swift](../../macos/Sources/LocalForwarderCore/Configuration.swift)의 `Host.validate`, `validToken`, `ConfigurationStore.load`.
- 관련 테스트: [Windows core_tests.cpp](../../windows/tests/core_tests.cpp)의 설정 round trip·입력 검증; [macOS CoreTests.swift](../../macos/Tests/LocalForwarderCoreTests/CoreTests.swift)의 `testValidationRejectsInjectionAndDuplicateActivePorts`, `testDiskRereadPreservesOtherHostsAndDamagedFiles`. 나열한 경계값 전체를 양쪽에서 검증하는 테스트는 이번 확인 범위에서 확인되지 않았다.
- macOS 수용 기준: 빈 문자열·공백뿐인 이름·선택 필드의 생략/null/빈 문자열·Unicode 공백과 제어문자를 포함한 공통 JSON 사례에 대해 확정된 정책대로 양쪽 결과를 검증한다. 거부된 기존 파일이 저장 시 덮어써지지 않는지 확인한다.
- 영향과 대응: 호환성 차이를 기록한다. 이 정적 비교만으로 보안 취약점이나 실제 데이터 손실을 확정하지 않는다. 공통 정책과 양쪽 영향을 검토해 필요한 구현·테스트를 수정할 수 있다.

### MAC-002: SSH 로그를 통한 인증 도움·연결 판정 조건의 차이

- 상태: 코드상 차이 확인 / 실제 OS 로그 재현 및 공통 정책 결정 대기.
- 변경 이유·영향: 같은 로그 문자열에 대한 판정이 달라 인증 도움 분류와 연결 준비 판정이 달라질 수 있다. 실제 SSH 버전·실행 흐름에서의 사용자 영향은 미확인이다.
- 근거: Windows의 `authenticationFailure`는 대소문자를 구분하며 `passphrase`를 포함한다. macOS의 `needsAuthenticationHelp`는 소문자로 변환하고 `no such identity`, `sign_and_send_pubkey`, `load key` 등을 검사하지만 같은 `passphrase` 조건은 없다. 준비 판정에서 macOS는 `Authentication succeeded`도 인증 증거로 인정한다. 실패 표식 중 Windows는 `cannot listen to port`, macOS는 `Could not request remote forwarding`을 각각 포함한다.
- 기대 동작: 각 OS에서 실제 생성되는 로그를 확보해 인증 복구가 필요한 실패를 안내하고, 포워딩 개설 실패를 연결 성공으로 표시하지 않는 공통 요구를 검증한다. 어느 한쪽의 문자열 목록을 그대로 복사하는 것으로 완료하지 않는다.
- 관련 코드: [Windows core.cpp](../../windows/src/core.cpp)의 `authenticationFailure`, `Readiness::feed`; [macOS SSHCommand.swift](../../macos/Sources/LocalForwarderCore/SSHCommand.swift)의 `needsAuthenticationHelp`, `Readiness.consume`.
- 관련 테스트: [Windows core_tests.cpp](../../windows/tests/core_tests.cpp)의 readiness·ProxyJump 사례; [macOS CoreTests.swift](../../macos/Tests/LocalForwarderCoreTests/CoreTests.swift)의 readiness·ProxyJump 사례; [공통 통합 테스트](../../tests/integration.py). 위 문자열 차이 각각의 양쪽 실행 검증은 미수행이다.
- macOS 수용 기준: 인증 실패·키 로드 실패·호스트 키 문제·로컬 및 원격 포워딩 실패·ProxyJump 로그로 기대 분류와 준비 판정을 확인한다. 실제 SSH 종료·오류 처리까지 포함해 잘못된 connected 상태가 없는지 검증한다.
- 영향과 대응: 실제 오판정 및 보안 영향은 미확인이다. 양쪽 실제 로그와 동작을 검증하고 필요한 구현·테스트를 수정할 수 있다.

### MAC-003: 서버가 제공한 로그 문자열과 상태 판정 분리

- 상태: macOS 반영 대기. Windows 코어 수정·회귀 테스트 완료, 최종 네이티브 검증 대기. macOS 실제 SSH 공격 시나리오는 미검증이다.
- 변경 이유·영향: OpenSSH의 `kex_exchange_identification` 로그는 서버 배너와 소프트웨어 식별자를 포함한다. `Authenticated to ... Entering interactive session.` 같은 배너를 상태 메시지로 해석하면 인증 전 연결됨을 표시할 수 있다. 채널 열기 실패의 서버 제공 설명에 원격 포워딩 실패 문자열이 있으면 다른 규칙 상태도 잘못 바뀔 수 있다. 이는 SSH 인증 우회가 아니라 앱 상태 표시의 신뢰성 문제다.
- 기대 동작: 서버가 제공한 텍스트는 진단으로 보존하되 연결·포워딩 상태 증거로 사용하지 않는다. 정식 상태 메시지의 시작 형식과 출처를 검증한다. [현재 명세](current-spec.md)의 상태 판정 요구를 따른다.
- 관련 코드·테스트: Windows `core.cpp`의 `targetDiagnosticLine`, `Readiness::consume`, `ForwardTracker::consume`와 `core_tests.cpp`의 배너·채널 오류 설명 회귀 테스트. macOS `SSHCommand.swift`의 `Readiness.consume`는 부분 문자열을, `ForwardRuntime.swift`의 `consume`는 시작 위치를 제한하지 않는 정규식·부분 문자열을 사용한다. 해당 macOS 코드의 정적 비교를 수행했으며 이번 기능의 macOS 소스 변경은 없다.
- macOS 수용 기준: 인증 전 배너·서버 식별자에 인증/세션/리스너/원격 포워딩 성공 문자열을 넣어도 연결됨·준비됨이 되지 않아야 한다. 채널 실패 설명에 원격 포워딩 실패 문구가 있어도 다른 규칙의 개설 상태를 변경하지 않아야 한다. 정상 인증·Local/Remote/DynamicForward·ProxyJump와 목적지 최근 오류 표시는 유지해야 한다. 코어 회귀와 실제 SSH 통합을 각각 검증한다.
- 보안·호환성: 설정 파일 형식과 저장 데이터에는 영향이 없다. Windows에서 재현한 문자열 주입 오판정을 수정했으며, macOS 동일 영향의 실제 실행 검증과 수정이 남아 있다.

## 추가 확인이 필요한 영역

| 영역 | 이번 확인 범위 | 남은 검증 |
|---|---|---|
| 연결 상태와 재연결 | 양쪽 상태 모델과 단위 테스트 정의를 읽음 | 이벤트 중복·순서, 실제 SSH 수명, 절전 복귀에서의 동등성 미확인 |
| 설정 저장·파일 호환성 | 입력 검증과 macOS 저장 코드, 관련 테스트 정의 일부를 읽음 | 양쪽 저장 결과 교차 로드, 동시 저장·권한·오류 복구 전체 미확인 |
| SSH 실행·인증 복구 | 인자 구성과 로그 판정 일부를 읽음 | 프로세스 정리, 터미널 복구, 공개키 등록 전체 동등성 미확인 |
| UI와 OS 통합 | 이번 비교 대상에서 제외 | 설정 편집, 트레이·메뉴바, 창 표시, 수동 재연결 전체 미확인 |
| 빌드·CI·실사용 | macOS CI와 평가 스크립트 정의 확인 | 양쪽 빌드·실행 결과 및 최신 CI 상태 미확인 |

OS 고유 동작은 별도로 판단한다. 예를 들어 README의 Windows SSH 포트 재사용 설명과 공통 통합 테스트의 `native_duplicate_bind` 관찰 분기는 OS별 결과 차이를 허용한다. 이 동작을 macOS에 복제할 작업으로 등록하지 않는다. 이번 작업에서 실제 포트 재사용 동작은 재검증하지 않았다.

## 항목 관리 방법

기존 MAC-001·MAC-002의 후속 결정·진행 상태는 [Windows 반영 대기](windows-catchup.md#기존-차이의-후속-검토)에서 계속 관리한다. 새 macOS 반영 대기는 이 문서에서 관리한다.

새 macOS 반영 대기 항목은 기존 번호에 이어 `MAC-` 고유 ID와 함께 아래 정보를 기록한다. 방향이 바뀌면 같은 항목의 기대 동작과 수용 기준을 갱신하고 대체된 내용을 현재 요구사항처럼 남기지 않는다.

- 상태: 검토 대기 / 반영 대기 / 구현 완료·검증 대기 / 검증 완료 / 대상 제외.
- 변경 이유와 영향, 확정된 기대 동작 및 명세 위치.
- 관련 Windows·macOS 코드와 테스트, 가능하면 변경 커밋.
- macOS 수용 기준과 실제 검증 결과. 미수행·제외된 검증은 이유와 함께 표시.
- 설정 호환성·보안·데이터 손실 영향과 대응 방법.

Windows 전용 UI·OS 변경은 이식 목록에 추가하지 않는다. 기존 차이를 대상에서 제외할 때는 이유를 남긴다. 구현만 끝났거나 테스트가 제외된 항목을 검증 완료로 표시하지 않는다.
