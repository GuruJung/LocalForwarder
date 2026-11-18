---
feature_id: 20261118-about-license
title: About 개발자 표시를 MIT 라이선스 링크로 교체
feature_type: standard
current_spec_path: docs/dev-plans/current-spec.md
---

## Tracked Feature Spec

### 요구사항

macOS와 Windows의 About에서 `개발자 GuruJung` 행을 제거하고, 버전 바로 아래에 클릭 가능한 `MIT License`를 표시한다.

표시 순서는 **앱 아이콘 → LocalForwarder → 버전 → MIT License → GitHub URL → 닫기**다. 라이선스 링크는 기본 브라우저에서 다음 주소를 연다.

`https://github.com/GuruJung/LocalForwarder/blob/main/LICENSE`

About을 여는 것만으로 네트워크 요청이나 브라우저 실행이 발생하지 않는다. 링크를 클릭해도 About은 유지된다. 기존 모달 차단, 중복 열기 처리, 닫기·Escape·창 닫기 동작과 SSH 연결·설정 초안 보존을 유지한다.

변경 범위는 About 표시다. LICENSE의 저작권자, GitHub 주소에 포함된 소유자명, 배포물의 라이선스 파일은 유지한다. 제품 버전 변경과 릴리스 발행은 포함하지 않는다.

### Current Spec Impact

`replace`: 현재 명세의 About 표시 항목에서 개발자 이름을 MIT 라이선스 링크로 교체하고, 표시 위치와 클릭 대상 URL을 명시한다. 전체 현재 명세는 한국어를 유지하며 다른 제품 의도와 검증 공백은 보존한다.

### 사용자 결정

- **개발자 표시:** About에서 개발자 이름을 제거하도록 요청했다. 별도 이유는 제시하지 않았다.
- **라이선스 표시:** `MIT License`를 클릭하면 기본 브라우저에서 저장소의 LICENSE 전문을 여는 방식을 선택했다. 별도 이유는 제시하지 않았다.

### 수용 기준

- 두 플랫폼에서 개발자 행 없이 버전 아래에 `MIT License` 링크가 표시되며 내용이 잘리거나 겹치지 않는다.
- 라이선스와 GitHub 링크가 각각 정확한 주소를 연다.
- About 표시만으로 외부 주소가 열리지 않으며, 기존 모달·연결·초안 보존 동작이 유지된다.
- 한국어·영어 사용 안내가 변경된 표시 내용과 일치한다.
