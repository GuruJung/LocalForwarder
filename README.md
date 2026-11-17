# LocalForwarder

시스템 OpenSSH를 사용하는 작은 네이티브 SSH local forwarding GUI입니다. macOS는 Swift/AppKit, Windows는 C++/Win32로 구현합니다. SSH 프로토콜과 실제 데이터 전송은 OS의 `ssh`가 담당합니다.

**개발 상태 (2026-11-17):** Windows 동결과 macOS 우선 개발 제한을 해제했습니다. macOS·Windows 모두 소스, 빌드 설정, 리소스, 테스트, 스크립트와 CI를 필요에 따라 수정할 수 있습니다. 두 플랫폼의 빌드·테스트·패키징 지원은 유지합니다. 제한 해제가 기존 기능 차이의 해결을 의미하지는 않으며, 플랫폼별 CI 통과·실제 기기 수동 검증·기능 동등성은 구분합니다. 플랫폼별 반영 상태와 차이는 [Windows 반영 대기](docs/dev-plans/windows-catchup.md)와 [macOS 반영 대기·차이 기록](docs/dev-plans/macos-catchup.md)에 기록합니다.

## 지원 환경과 다운로드

| OS | CPU | 파일 |
|---|---|---|
| macOS 15 이상 | Apple Silicon ARM64 | `LocalForwarder-macos-arm64.zip` |
| Windows 11 | x64 | `LocalForwarder-windows-x64.zip` |
| Windows 11 | ARM64 | `LocalForwarder-windows-arm64.zip` |

[GitHub Actions](https://github.com/GuruJung/LocalForwarder/actions/workflows/build.yml)의 성공한 실행을 열고 **Artifacts**에서 해당 CPU의 파일을 다운로드하세요. Actions 결과물 다운로드에는 GitHub 로그인이 필요하며 결과물은 7일간 보관됩니다. Artifact ZIP 안의 배포 ZIP도 풀어 macOS에서는 `.app`, Windows에서는 `.exe`를 실행합니다.

첫 버전은 무료 배포를 위해 macOS ad-hoc 서명과 Windows 인증서 없는 실행 파일을 사용합니다. macOS가 앱 실행을 차단하면 출처를 확인한 뒤 시스템 설정의 개인정보 보호 및 보안에서 허용할 수 있습니다. Windows에서는 SmartScreen 경고가 나타날 수 있습니다. 조직 정책이 실행을 금지한다면 관리자의 정책을 따르세요.

Windows에는 **OpenSSH Client 9.5 이상** 선택적 기능이 필요합니다. 앱은 Windows 시스템 OpenSSH를 사용하며 Git for Windows의 SSH나 별도의 SSH 구현을 설치하지 않습니다. macOS에서는 `/usr/bin/ssh`를 사용합니다.

Windows에서는 시스템 OpenSSH Client **9.5 이상**을 사용합니다. PowerShell에서 `& "$env:WINDIR\System32\OpenSSH\ssh.exe" -V`로 확인할 수 있습니다. 구버전은 Windows Update 또는 [Microsoft의 OpenSSH 업데이트 안내](https://learn.microsoft.com/en-us/troubleshoot/windows-server/system-management-components/upgrade-in-box-openssh-to-latest-openssh-release)에 따라 사용자가 업데이트합니다. 앱은 자동 설치·관리자 권한 요청·서비스 설정 변경을 하지 않습니다. 원격 Linux/macOS 서버에 같은 최소 버전을 요구하지 않습니다.

## 사용 방법

앱 하나에서 여러 호스트를 관리합니다. 실행하면 메뉴 막대 아이콘이 항상 표시됩니다. 설정 파일이 없거나 `--open-config`로 실행하면 먼저 설정 창을 엽니다. 시작 설정의 OK는 저장된 자동 연결 ON 호스트를 연결하고, Cancel은 연결 없이 트레이 모드로 들어갑니다. 기존 설정으로 실행하면 자동 연결 ON 호스트들이 연결됩니다. 중복 실행은 기존 앱의 설정 창을 엽니다.

1. 새 호스트를 만들고 이름과 SSH 별칭 또는 주소를 입력합니다. 이름 아래의 선택 설명란에는 연결 용도를 한 줄로 적을 수 있습니다. `~/.ssh/config`의 별칭도 사용할 수 있습니다.
2. 필요한 경우 사용자명, SSH 포트, 개인키 경로를 지정합니다. 빈 접속 항목은 SSH 설정을 따릅니다. `새 규칙…`에서 포워딩을 작성하고, `규칙 편집…`이나 행 더블클릭으로 수정합니다. 모달 확인은 초안만 바꾸며 취소는 기존 규칙을 유지합니다.
3. 자동 연결, `ExitOnForwardFailure`, 연결 거부 시 재시도는 기본 ON입니다.
4. 저장은 선택한 호스트만 저장합니다. 현재 연결과 다른 호스트의 미저장 편집은 유지합니다.
5. 재연결은 선택한 호스트의 편집값을 사용합니다. 미저장이 있으면 저장 후 재연결 / 저장 없이 재연결 / 취소 중 선택합니다. OK는 자동 저장하지 않습니다.

`15432 → 127.0.0.1:5432`는 내 컴퓨터의 `127.0.0.1:15432`를 **SSH 서버에서 보는** `127.0.0.1:5432`로 연결합니다. 앱 규칙은 IPv4 루프백에만 바인딩합니다. 기존 SSH의 `LocalForward`, `RemoteForward`, `DynamicForward`는 원래 설정대로 함께 실행합니다.

설정창은 약 900×650의 작은 기본 크기로 열리며 작은 화면에서는 폼을 스크롤할 수 있습니다. 하단 상시 메시지 영역은 없고 작업 실패 시 경고가 표시됩니다. 저장 성공은 호스트의 미저장 표시가 사라지는 것으로 확인합니다.

트레이 메뉴는 설정, 로그인 자동 시작, 전체 재연결, 호스트별 메뉴, 종료로 구성됩니다. 호스트 상태는 아이콘으로 표시하며 하위 메뉴에는 SSH 요약, 규칙별 정보와 준비 상태, 연결 동작이 차례로 나타납니다. 비활성 규칙도 별도 아이콘으로 표시합니다. 현재·마지막 실행 규칙을 표시하므로 편집값이나 새 저장값과 다를 수 있습니다. 자세한 오류와 실행 설정 차이는 `연결 상태…`에서 확인합니다. 자동 연결 OFF인 호스트도 수동 연결할 수 있습니다. 수동 끊기는 이번 실행의 연결 의도만 OFF로 바꾸며 자동 연결 설정은 바꾸지 않습니다. 설정 창 닫기와 개별 실패는 앱을 종료하지 않습니다.
