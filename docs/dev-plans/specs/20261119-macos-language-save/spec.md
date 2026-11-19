---
feature_id: 20261119-macos-language-save
title: 언어 저장 오류 수정 및 자동 언어 메뉴 문구 정리
feature_type: standard
current_spec_path: docs/dev-plans/current-spec.md
---

# Tracked Feature Spec

**표시 동작.** macOS와 Windows에서 자동 항목은 현재 앱 언어로 번역한 고정 문구만 표시한다. 한국어 문구는 `자동(시스템 언어)`, 영어 문구는 `Automatic (System Language)`다. 나머지 5개 언어도 같은 의미로 표시한다.

| 사용자 동작 | 자동 항목의 표시 | 체크된 항목 |
|---|---|---|
| OS가 영어일 때 앱에서 한국어 선택 | `자동(시스템 언어)` | `Korean — 한국어` |
| 같은 환경에서 자동 선택 | `Automatic (System Language)` | 자동 항목 |
| 한국어를 수동 선택하고 앱 재시작 | `자동(시스템 언어)` | `Korean — 한국어` |

**사용자 결정.** 현재 앱의 언어명을 자동 항목에 덧붙이면 시스템 언어로 오해할 수 있어, 두 플랫폼에서 언어명을 생략한다. 앞서 검증한 macOS 언어 저장 수정도 함께 a05 로컬 `main`에 반영하고 이 Mac에 설치한다.

**Current Spec Impact — `replace`.** 현재 명세의 「표시 언어」에서 자동 항목에 적용 언어명을 표시한다는 요구를 위 고정 문구로 교체한다. 자동 선택 시 OS 선호 언어를 해석하는 규칙, 수동 선택의 재시작 유지, 실제 저장 실패 시 기존 선택 보존은 그대로 적용한다.
