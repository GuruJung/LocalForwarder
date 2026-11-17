# 로그인 자동 시작 검증

[기능 명세](../dev-plans/specs/20261117-launch-at-login/spec.md)를 기준으로 기존 SSH 정책을 유지하면서 메뉴와 사용자별 OS 등록을 검증한다.

## 자동 검사

- macOS: `bash scripts/eval-macos.sh`
- Windows x64/ARM64: `pwsh -NoProfile -File scripts/eval-windows.ps1 -Architecture x64` 및 `-Architecture arm64`
- 상태 모델: 미등록·등록·승인 필요·조회 실패·등록/해제 실패·재조회·중복 클릭·외부 삭제를 검사한다.
- Windows: 실제 Run 대신 주입한 임시 레지스트리 키에서 공백·한글·길이 경계·명령 인수 부재·다른 값 보존·반복 등록/해제를 검사한다.
- macOS: 별도 bundle ID의 ad-hoc 서명 테스트 앱을 일회성 CI에서 등록·조회·해제한다. 실행 가능과 승인 필요를 구분하고 테스트 등록을 정리한다. 일반 로컬 테스트는 실제 로그인 등록을 변경하지 않는다.

## 실기기 확인과 제한

Windows 메뉴에서 설정 다음에 체크 항목이 있고, 상태 재조회·등록·해제가 현재 연결을 건드리지 않는지 확인한다. 실제 Run 값을 검사할 경우 기존 값을 기록하고 시험 후 복원한다. 로그아웃·재부팅·다른 앱의 등록 변경은 하지 않는다.

실제 재로그인 실행과 macOS 승인 화면의 실기기 검증은 자동 검사와 별개다. 미수행이면 미수행으로 명시한다. CI 등록 성공을 로그인 완료 증거로 사용하지 않는다.

## 실행 결과

- 2026-11-17 Windows 11 x64 호스트에서 상태 모델 및 네이티브 메뉴·임시 레지스트리 등록/해제 검사를 통과했다. 실제 Run 값은 변경하지 않았다.
- Linux에서 공통 CTest 4개를 통과하고 Windows x64·ARM64 교차 빌드를 완료했다.
- 코드 커밋 `7989264`의 [네이티브 CI 44030256265](https://github.com/GuruJung/LocalForwarder/actions/runs/44030256265)에서 macOS 15 ARM64, Windows x64·ARM64 빌드·테스트·패키징이 모두 성공했다.
- macOS 단위 검사 44개와 실제 로그인 항목 등록·해제·정리를 통과했다. 최초 `notPresent` → `enabled` → `notRegistered`를 기록했고 테스트 항목을 제거했다. 승인 필요 분기는 가짜 서비스로 검증했으며 실제 CI 등록은 승인이 필요하지 않았다.
- Windows는 각 아키텍처에서 CTest 7개를 통과했다. SSH 통합 시나리오는 각 플랫폼 16개 중 macOS 15개, Windows 14개를 실행·통과했고 기존 플랫폼 전용 제외 1개·2개는 그대로 유지했다. 새 로그인 검사는 제외하지 않았다.
- 실제 재로그인, macOS 승인 화면, 제품 트레이를 직접 클릭하는 수동 검증은 수행하지 않았다.

macOS 15의 새로운 앱은 `SMAppService.mainApp.status`가 `.notFound`를 반환할 수 있다. CI의 유효한 ad-hoc 앱에서 시스템 로그의 `record not found`와 실제 등록 후 `.enabled`, 해제 후 `.notRegistered` 전이를 확인했다. 이 알려진 기록 부재 상태는 최초 등록 대상으로 표시하고, 알 수 없는 상태·조회 오류는 비활성 상태로 구분한다.
