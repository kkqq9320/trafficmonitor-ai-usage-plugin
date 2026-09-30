# TrafficMonitor AI Usage Limits

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4)
![TrafficMonitor](https://img.shields.io/badge/TrafficMonitor-plugin-2EA043)
![Usage Sources](https://img.shields.io/badge/usage-Claude%20%2B%20Codex-0A7F5A)

언어: [English](README.md) | 한국어

Claude와 Codex 사용량 한도를 Windows 작업 표시줄에 띄워 두는 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 플러그인입니다. TrafficMonitor 안에서는 `AI Usage Limits`로 보입니다.

이 저장소는 [bemaru/trafficmonitor-ai-usage-plugin](https://github.com/bemaru/trafficmonitor-ai-usage-plugin)의 fork이며 따로 릴리스합니다. fork에서 더한 기능은 [CHANGELOG.md](CHANGELOG.md)에 있습니다.

<p align="center">
  <img src="docs/images/trafficmonitor-taskbar-compact.png" alt="Claude와 Codex 사용량 막대를 보여 주는 TrafficMonitor 작업 표시줄" />
</p>

## 표시하는 값

| 표시 설정 이름 | 라벨 | 값 |
|---|---|---|
| `Claude 5h`, `Claude 7d` | `C5h`, `C7d` | Claude 5시간·7일 한도 사용률 |
| `Codex 5h`, `Codex 7d` | `X5h`, `X7d` | Codex 5시간·7일 한도 사용률 |
| `Claude Fable 7d` (선택) | `CF7d` | Claude Fable 주간 한도 사용률 |
| `Codex credits` (선택) | `Xcr` | Codex 크레딧 잔액 |
| `Claude resets`, `Codex resets` (선택) | `Crs`, `Xrs` | 계정별 사용량 초기화권 개수 |

작업 표시줄 창에 마우스를 올리면 남은 비율, 초기화까지 남은 시간, 요금제, 크레딧, 초기화권이 툴팁으로 나옵니다.

```text
📊 Claude left 5h 83% · 7d 17% · Fable 91%
5h: 17% (3h 42m) at 2026-10-01 04:19 목요일
7d: 83% (2d 22h 22m) at 2026-10-03 22:59 토요일
Fable 7d: 9% (2d 22h 22m) at 2026-10-03 22:59 토요일
Updated: just now, Claude web helper, Plan Max (5x)

📊 Codex left 7d 14%
7d: 86% (3d 2h 43m) at 2026-10-04 03:19 일요일
Credits: 62,500
Updated: 4m ago, Codex API, Plan Pro

🎟 Reset Credits
1 · Claude (Expires : 2026-10-23 01:00 금요일)
2 · Codex  (Expires : 2026-10-23 05:39 금요일)
```

값을 읽는 방법은 [docs/runtime.md](docs/runtime.md)에 있습니다.

## 빠른 설치

필요한 것: Windows x64, TrafficMonitor x64판, Node.js 22 이상, Claude 첫 로그인용 Microsoft Edge 또는 Google Chrome. Codex 값은 같은 PC에서 로그인한 Codex에서 읽습니다.

1. Node.js가 없으면 설치합니다: `winget install OpenJS.NodeJS.LTS`
2. [최신 릴리스](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/releases/latest)에서 `TrafficMonitorAIUsageLimits_v<버전>_x64.zip`을 받습니다.
3. TrafficMonitor를 종료하고, `TrafficMonitor.exe`가 있는 폴더에 zip을 풉니다.
4. TrafficMonitor를 실행하고 작업 표시줄 창을 우클릭해 `Display Settings...`에서 원하는 항목을 체크합니다.
5. Claude에 한 번 로그인합니다. PowerShell에서 TrafficMonitor 폴더로 이동한 뒤 실행합니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 login
```

이미 설치한 것을 새 버전으로 바꿀 때는 [새 버전으로 업데이트](docs/install.ko.md#새-버전으로-업데이트)를 따릅니다. 화면 사진이 있는 전체 설명은 [docs/install.ko.md](docs/install.ko.md)에 있습니다.

## 문서

- [설치·업데이트·제거](docs/install.ko.md) ([English](docs/install.md))
- [동작 방식, 툴팁, helper 설정](docs/runtime.md)
- [문제 해결](docs/troubleshooting.md)
- [빌드와 테스트](docs/build.md), [릴리스 체크리스트](docs/release-checklist.md)
- [개인정보와 로컬 데이터](PRIVACY.md)
- [변경 기록](CHANGELOG.md), [라이선스](LICENSE), [고지](NOTICE.md)

## 만든 사람

원래 플러그인은 [bemaru](https://github.com/bemaru)가 만들었고, [GitHub Sponsors](https://github.com/sponsors/bemaru)로 후원을 받습니다. 이 fork는 [kkqq9320](https://github.com/kkqq9320)이 관리하며, Anthropic·OpenAI·TrafficMonitor의 공식 프로젝트가 아닙니다.
