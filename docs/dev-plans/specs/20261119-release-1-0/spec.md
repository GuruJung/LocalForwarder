---
feature_id: 20261119-release-1-0
title: LocalForwarder 1.0.0 정식 릴리스
feature_type: standard
current_spec_path: docs/dev-plans/current-spec.md
---

# Tracked Feature Spec

**LocalForwarder 1.0.0을 첫 정식 GitHub Release로 공개한다.** 사용자는 저장소 첫 화면에서 자신의 OS와 CPU에 맞는 ZIP을 찾아 다운로드하고 실행할 수 있어야 한다.

- 요청한 ‘1.0’은 기존 제품 버전 규칙에 따라 `1.0.0`으로 표기한다. About, macOS 앱 번들, Windows 실행 파일, ZIP 파일명, 태그 `v1.0.0`이 같은 버전을 가리킨다. 사용자 설정 파일의 형식은 기존 v2를 사용한다.
- 영어·한국어 README 상단에 **최신 정식 릴리스** 링크와 macOS 15 이상 Apple Silicon, Windows 11 x64, Windows 11 ARM64용 다운로드 링크를 제공한다. ZIP을 한 번 풀어 `.app` 또는 `.exe`를 실행하는 방법과 기존 서명·OpenSSH 요구사항을 안내한다. Actions 아티팩트 안내는 개발 빌드 절로 옮긴다.
- **수용 기준**은 GitHub의 최신 정식 Release가 `LocalForwarder 1.0.0`으로 공개되고, 세 플랫폼 ZIP을 README에서 다운로드할 수 있으며, 각 패키지의 버전·플랫폼·아키텍처·빌드 커밋이 태그와 일치하는 것이다. 완료 시 공개 Release URL을 제공한다.

**사용자 결정**

| 주제 | 선택과 적용 범위 |
|---|---|
| 배포 방식 | 이번 버전을 1.0 정식 릴리스로 빌드·공개한다. 일반 사용자가 Actions 밖에서 실행 파일을 받을 수 있도록 README도 바꾼다. |
| 원격 반영 | 검증·리뷰 후 다국어 지원, 포트 정렬, 언어 저장 수정 등 기존 로컬 3개 커밋과 릴리스 준비 커밋을 원격 `main`에 일반 push한다. 같은 통합 커밋의 `v1.0.0` 태그 push와 공개 발행까지 승인했다. 별도 선택 이유는 제시하지 않았다. |

**Current Spec Impact — `add` + `replace`.** 현재 명세의 문서 정책에 GitHub Releases를 기본 다운로드 경로로 추가한다. 제품·배포 절에서는 개발 초기값 `0.1.0`과 첫 정식 배포 버전 `1.0.0`을 구분한다. 기존 세 플랫폼 검증, 태그 발행 조건, 설정 호환성 및 실기기 검증 범위는 한국어로 보존한다.
