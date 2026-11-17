# 원격 push 정책

이 저장소에서는 사용자가 승인한 구현·검증 작업에 필요한 구현 브랜치의 원격 push를 사전 승인한다. `implement-dev-plan` 스킬의 “Do not push or delete the feature branch or worktree automatically.” 지침 중 구현 브랜치 자동 push 금지 부분은 이 정책으로 대체한다.

- 작업용 구현 브랜치는 커밋 후 별도 사용자 확인 없이 원격에 push하고 CI를 실행·확인한다. CI 수정 후 같은 구현 브랜치에 다시 push할 때도 추가 확인을 요청하지 않는다.
- 원격 `main` 브랜치로의 push는 항상 해당 작업에 대한 사용자의 명시적 승인이 필요하다. 구현·검증·로컬 통합 승인이나 구현 브랜치 push 승인을 `main` push 승인으로 해석하지 않는다. 다른 로컬 브랜치에서 `HEAD:main`처럼 원격 `main`을 갱신하는 경우에도 같은 규칙을 적용한다.
- 이 사전 승인은 일반 push에만 적용한다. 강제 push, 원격 브랜치 삭제, 브랜치·worktree 삭제는 승인 범위에 포함하지 않는다.
