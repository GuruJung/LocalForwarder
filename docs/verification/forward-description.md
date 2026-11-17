# 포워딩 규칙 설명 검증

[기능 명세](../dev-plans/specs/20261117-forward-description/spec.md)에 따라 저장 호환성과 설정창 편집을 검사한다.

## 자동 검사

- `bash scripts/eval-macos.sh`
- `pwsh -NoProfile -File scripts/eval-windows.ps1 -Architecture x64`
- `pwsh -NoProfile -File scripts/eval-windows.ps1 -Architecture arm64`
- 기존 v1/v2의 필드 누락, null, 빈 문자열, 한글 문자열을 읽고 빈 설명은 저장 시 생략한다. 잘못된 타입과 개행·NUL을 거부하며 읽기 실패 시 원본을 보존한다.
- 선택 저장·다른 호스트 저장·인증 키 갱신에서 설명을 보존하고 v1 최초 저장 백업을 유지한다.
- 규칙 확인·취소·설명 삭제·ID와 순서·미저장 판단을 검사한다. SSH 명령과 실행 설정 비교 및 기존 트레이·연결 상태 표현은 설명에 영향받지 않는다.

## 화면 확인

Windows의 격리된 테스트 설정에서 설명 입력과 목록의 마지막 열, 작은 창과 긴 한글 설명을 확인한다. 실제 사용자 설정·현재 연결은 변경하지 않는다. macOS 자동 검사와 실제 기기 수동 검증을 구분한다.

## 실행 결과

구현 진행·검증 대기. 최종 코드 커밋, 플랫폼별 CI, Windows 화면 확인 결과와 미수행 범위를 기록한다.
