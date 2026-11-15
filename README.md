# LocalForwarder

시스템 OpenSSH를 사용하는 작은 네이티브 SSH local forwarding GUI입니다. macOS는 Swift/AppKit, Windows는 C++/Win32로 구현합니다. SSH 프로토콜과 실제 데이터 전송은 OS의 `ssh`가 담당합니다.

**개발 상태 (2026-11-13):** macOS 동결을 해제하고, 주말 동안 개발 가능한 macOS를 기준으로 기능 개발·버그 수정·실사용 검증을 진행합니다. Windows 구현은 명시적인 전환·해제 요청까지 동결하고 기존 빌드·테스트·패키징 CI는 유지합니다. 주말 종료만으로 정책이 자동 전환되지는 않습니다. Windows CI 통과가 최신 macOS와의 기능 동등성이나 실제 Windows PC에서의 수동 검증을 의미하지는 않습니다. 플랫폼별 반영 상태는 [Windows 반영 대기](docs/dev-plans/windows-catchup.md)에 기록하며, 이전 [macOS 차이 기록](docs/dev-plans/macos-catchup.md)은 보존합니다.

## 지원 환경과 다운로드

| OS | CPU | 파일 |
|---|---|---|
| macOS 15 이상 | Apple Silicon ARM64 | `LocalForwarder-macos-arm64.zip` |
| Windows 11 | x64 | `LocalForwarder-windows-x64.zip` |
| Windows 11 | ARM64 | `LocalForwarder-windows-arm64.zip` |

[GitHub Actions](https://github.com/GuruJung/LocalForwarder/actions/workflows/build.yml)의 성공한 실행을 열고 **Artifacts**에서 해당 CPU의 파일을 다운로드하세요. Actions 결과물 다운로드에는 GitHub 로그인이 필요하며 결과물은 7일간 보관됩니다. Artifact ZIP 안의 배포 ZIP도 풀어 macOS에서는 `.app`, Windows에서는 `.exe`를 실행합니다.

첫 버전은 무료 배포를 위해 macOS ad-hoc 서명과 Windows 인증서 없는 실행 파일을 사용합니다. macOS가 앱 실행을 차단하면 출처를 확인한 뒤 시스템 설정의 개인정보 보호 및 보안에서 허용할 수 있습니다. Windows에서는 SmartScreen 경고가 나타날 수 있습니다. 조직 정책이 실행을 금지한다면 관리자의 정책을 따르세요.

Windows에는 **OpenSSH Client** 선택적 기능이 필요합니다. 앱은 Windows 시스템 OpenSSH를 사용하며 Git for Windows의 SSH나 별도의 SSH 구현을 설치하지 않습니다. macOS에서는 `/usr/bin/ssh`를 사용합니다.

## macOS 사용 방법

앱 하나에서 여러 호스트를 관리합니다. 실행하면 메뉴 막대 아이콘이 항상 표시됩니다. 설정 파일이 없거나 `--open-config`로 실행하면 먼저 설정 창을 엽니다. 시작 설정의 OK는 저장된 자동 연결 ON 호스트를 연결하고, Cancel은 연결 없이 트레이 모드로 들어갑니다. 기존 설정으로 실행하면 자동 연결 ON 호스트들이 연결됩니다. 중복 실행은 기존 앱의 설정 창을 엽니다.

1. 새 호스트를 만들고 이름과 SSH 별칭 또는 주소를 입력합니다. `~/.ssh/config`의 별칭도 사용할 수 있습니다.
2. 필요한 경우 사용자명, SSH 포트, 개인키 경로와 포워딩 규칙을 지정합니다. 빈 접속 항목은 SSH 설정을 따릅니다.
3. 자동 연결, `ExitOnForwardFailure`, 연결 거부 시 재시도는 기본 ON입니다.
4. 저장은 선택한 호스트만 저장합니다. 현재 연결과 다른 호스트의 미저장 편집은 유지합니다.
5. 재연결은 선택한 호스트의 편집값을 사용합니다. 미저장이 있으면 저장 후 재연결 / 저장 없이 재연결 / 취소 중 선택합니다. OK는 자동 저장하지 않습니다.

`15432 → 127.0.0.1:5432`는 내 컴퓨터의 `127.0.0.1:15432`를 **SSH 서버에서 보는** `127.0.0.1:5432`로 연결합니다. 앱 규칙은 IPv4 루프백에만 바인딩합니다. 기존 SSH의 `LocalForward`, `RemoteForward`, `DynamicForward`는 원래 설정대로 함께 실행합니다.

트레이 메뉴는 설정, 전체 재연결, 호스트별 메뉴, 종료로 구성됩니다. 자동 연결 OFF인 호스트도 수동 연결할 수 있습니다. 수동 끊기는 이번 실행의 연결 의도만 OFF로 바꾸며 자동 연결 설정은 바꾸지 않습니다. 설정 창 닫기와 개별 실패는 앱을 종료하지 않습니다.

저장 없이 적용한 설정은 수동 재연결과 자동 복구에도 사용합니다. 편집을 취소해도 이미 적용한 실행값은 유지합니다. 명시적 저장은 다음 연결부터 저장값을 사용하게 하며 현재 연결은 유지합니다. **전체 재연결**은 임시 실행값을 해제하고 저장된 자동 연결 ON 호스트만 다시 연결합니다. 저장 없이 연결한 신규 호스트는 현재 실행 동안만 유지됩니다.

## macOS 연결과 포워딩 상태

절전·시스템 중단 복귀 또는 네트워크 사용 불가→사용 가능 전환 후 **마지막 이벤트부터 5초 뒤** 복구합니다. 연결 의도 ON이며 실제로 끊긴 호스트만 대상입니다. 정상 연결·연결 시도 중·수동 단절 호스트는 건드리지 않습니다. 대기 중 다시 오프라인이 되면 예약을 취소합니다. 일반 포커스 복귀와 설정 열기는 복구 계기가 아닙니다.

복구 시점 뒤 늦게 감지된 단절은 다음 복구 이벤트나 수동 연결을 기다립니다. 연결 거부 시 재시도 ON이면 연결 전 명시적 접속 거부·조기 종료·리셋에 한해 **1초 뒤 한 번** 재시도합니다. 인증·호스트 키 오류·포트 충돌·일반 타임아웃에는 재시도하지 않습니다. 60초 자동 재연결과 실패 후 자동 종료는 제거되었습니다.

현재 연결 상태 표는 실제 실행 규칙과 상속 규칙의 준비 중 / 준비됨 / 개설 실패 / 연결 끊김 / 확인 불가를 표시합니다. `ExitOnForwardFailure=OFF`이면 포워딩 일부가 실패해도 살아 있는 SSH는 connected로 표시하고 경고합니다. **준비됨은 포워딩 개설 성공이며 목적지 DB·웹 서비스의 정상 동작을 보장하지 않습니다.** 목적지 서비스를 주기적으로 검사하지 않으며 사용 중 관측한 실패는 최근 오류에 남깁니다.

## 설정 파일과 구버전 호환성

두 OS 모두 실제 사용자 홈의 `.LocalForwarder/config.json`을 사용합니다. macOS는 버전 2를 저장합니다.

```json
{
  "version": 2,
  "hosts": [{
    "id": "development",
    "name": "개발 서버",
    "target": "dev-server",
    "autoConnect": true,
    "exitOnForwardFailure": true,
    "retryOnConnectionRefused": true,
    "forwards": [{
      "id": "database",
      "enabled": true,
      "localPort": 15432,
      "remoteHost": "127.0.0.1",
      "remotePort": 5432
    }]
  }]
}
```

선택 필드는 `user`, `port`, `identityFile`입니다. 호스트와 규칙 ID는 고유한 영문·숫자·하이픈·밑줄 식별자입니다. 새 옵션이 없는 기존 호스트는 세 옵션을 모두 ON으로 읽습니다. 개인키 내용과 비밀번호는 JSON에 저장하지 않습니다.

v1은 읽기만 하면 변경하지 않습니다. 최초 저장 때 원본을 같은 폴더의 `config-v1-<고유 ID>.json`에 보존하고 v2로 저장합니다. **동결된 Windows 앱과 구버전 앱은 v2 설정을 읽지 못합니다.** 구버전으로 돌아가려면 모든 앱·인증 도우미를 종료하고, 현재 `config.json`을 별도 보관한 뒤 v1 백업을 `config.json`으로 복사하세요. v2 저장 이후 변경은 v1 백업에 포함되지 않습니다. 버전 숫자만 바꾸는 방식은 새 옵션과 동작을 보존하지 못합니다.

창을 새로 열 때 파일을 읽고 열린 창에서는 편집값을 유지합니다. 저장은 잠금 안에서 최신 파일의 선택 호스트만 원자적으로 갱신합니다. 같은 항목은 마지막 저장을 따릅니다. 손상되거나 지원하지 않는 파일은 덮어쓰지 않으며 복구 후 다시 열어야 합니다.

## 인증 준비와 복구

일반 연결은 콘솔과 입력 없이 실행됩니다. 설정 창에서 선택한 호스트의 인증 준비를 명시적으로 요청하면 외부 터미널 도우미를 엽니다. macOS 앱과 다른 연결은 유지합니다. 완료 후 해당 호스트의 저장 변경만 반영하며 연결은 직접 요청합니다.

도우미는 기존 키를 우선 사용하고 필요하면 `.LocalForwarder/keys/`에 암호 없는 Ed25519 키를 생성합니다. 일반 Linux/macOS 계정의 `~/.ssh/authorized_keys`에 기존 키와 제한 옵션을 보존하며 중복 없이 등록합니다. 비밀번호·기존 키 암호는 터미널에서만 입력합니다. 서버 인증 정책과 변경된 호스트 키 검증을 자동 우회하지 않습니다. 원격 Windows 계정 자동 등록은 지원 범위 밖이며 이미 인증된 Windows SSH 서버에는 연결할 수 있습니다.

## 동결 Windows 동작

Windows는 v1 설정, 실행마다 호스트 선택, 인스턴스당 하나의 호스트·아이콘을 유지합니다. 첫 실패 후 1초 뒤 한 번 재시도하며, 연결 후 단절은 60초 뒤 자동 재연결하고 최종 실패 60초 뒤 종료합니다. 최초 연결 실패나 인증 복구에서도 기존 종료 흐름을 유지합니다. 새 macOS 동작과 설정 v2는 아직 구현·검증하지 않았습니다.

Windows OpenSSH는 포트 재사용을 허용할 수 있으므로 각 인스턴스에 다른 로컬 포트를 지정하세요. 중복 포트의 트래픽 전달 대상은 보장되지 않으며 앱은 시스템 SSH 결과를 따릅니다. [Windows 소켓 재사용 동작](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)

## 빌드와 테스트

GitHub 공개 저장소의 표준 macOS·Windows 러너를 사용합니다. 사용자의 Windows PC에 MSVC나 CMake를 설치할 필요 없이 Actions에서 빌드 결과를 받을 수 있습니다.

Mac에 Xcode가 있다면 저장소 루트에서 다음 명령으로 빌드·테스트합니다.

```sh
bash scripts/eval-macos.sh
```

Windows에서 로컬 빌드를 원한다면 C++ 데스크톱 개발 도구가 포함된 Visual Studio/Build Tools와 CMake, Ninja, Python, 테스트용 OpenSSH Server가 필요합니다. 개인 개발자는 Visual Studio Community를 사용할 수 있습니다.

```powershell
pwsh -NoProfile -File scripts/eval-windows.ps1 -Architecture x64
# ARM64 Windows에서는 -Architecture arm64
```

실제 SSH 통합 테스트는 임시 키·설정·루프백 서버만 사용합니다. macOS에서는 격리된 테스트 sshd 실행을 위해 sudo 권한이 필요할 수 있습니다. Windows CI의 테스트용 SSH 서버 준비 스크립트는 일회성 GitHub 러너에서만 동작하며 앱에 SSH 서버를 포함하지 않습니다.

공통 테스트는 기본 `--behavior legacy-v1`로 기존 Windows 시나리오와 기대값을 유지합니다. macOS 평가만 `--behavior multi-host-v2`를 지정하여 실제 연결 관리자의 다중 호스트·5초 복구·제한 재시도·정리와 65초 유휴 중 무재연결/무종료를 검사합니다. Windows에는 기존 60초 수명 검증이 남습니다. 전체 평가에는 몇 분이 걸립니다. CI의 evidence 결과물에 실행 결과와 측정값을 기록합니다. 실제 데스크톱의 시각적 확인 여부는 자동 테스트 성공과 별도로 보고합니다.

## 라이선스와 명세

[MIT License](LICENSE). Windows JSON 처리는 [yyjson](third_party/yyjson/README.md)을 사용하며 해당 라이선스를 보존합니다.

[현재 기능 명세](docs/dev-plans/current-spec.md) · [승인된 다중 호스트 명세](docs/dev-plans/specs/20261115-multi-host-tray/spec.md) · [이전 v1 명세](docs/dev-plans/specs/20261111-localforwarder-v1/spec.md)
