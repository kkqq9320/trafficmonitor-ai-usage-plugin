# 설치 설명서

언어: [English](install.md) | 한국어

릴리스 zip으로 플러그인을 설치하는 방법입니다. 소스에서 직접 빌드하려면 [build.md](build.md)를 봅니다.

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)는 네트워크 속도, CPU, 메모리를 떠 있는 창이나 작업 표시줄에 보여 주는 Windows 모니터 프로그램입니다. 릴리스 zip에는 플러그인만 들어 있으므로 TrafficMonitor는 따로 설치합니다.

## 필요한 것

- Windows x64와 TrafficMonitor x64판. 플러그인은 x86판을 릴리스하지 않습니다.
- 표준 위치(`%ProgramFiles%\nodejs` 등)에 설치한 Node.js 22 이상. 두 helper는 PATH를 보지 않습니다. 다른 위치에 있으면 [helper 설정](runtime.md#helper-settings)의 `node_path`에 적습니다.
- Claude 첫 로그인용 Microsoft Edge 또는 Google Chrome.
- Codex 값을 보려면 같은 PC에서 로그인한 Codex(데스크톱 앱 또는 CLI).

## 처음 설치

### 1. TrafficMonitor 설치

1. [TrafficMonitor 릴리스](https://github.com/zhongyang219/TrafficMonitor/releases)에서 x64 패키지를 받습니다. 온도 표시가 필요 없으면 Lite 패키지로 충분합니다.
2. 원하는 폴더에 풉니다. 예: `D:\TrafficMonitor`
3. `TrafficMonitor.exe`를 한 번 실행해 켜지는지 확인하고 종료합니다(트레이 아이콘 우클릭 → `Exit`).

### 2. Node.js 설치

`node --version`이 22 이상을 보여 주면 건너뜁니다.

```powershell
winget install OpenJS.NodeJS.LTS
```

### 3. 플러그인 풀기

1. [최신 릴리스](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/releases/latest)에서 `TrafficMonitorAIUsageLimits_v<버전>_x64.zip`을 받습니다.
2. TrafficMonitor가 꺼져 있는지 확인합니다.
3. `TrafficMonitor.exe`가 있는 폴더에 zip을 풉니다.

풀고 나면 폴더가 이렇게 됩니다.

```text
TrafficMonitor
├─ TrafficMonitor.exe
└─ plugins
   ├─ ClaudeUsagePlugin.dll
   └─ ClaudeUsagePlugin
      ├─ claude-web-helper.ps1
      ├─ codex-usage-helper.ps1
      ├─ helper-common.ps1
      ├─ LICENSE, NOTICE.md, PRIVACY.md
      └─ helper
         ├─ claude-web-helper   (index.mjs, package.json, package-lock.json)
         └─ codex-usage-helper  (index.mjs, lib.mjs, package.json)
```

### 4. TrafficMonitor 실행과 작업 표시줄 창 켜기

`TrafficMonitor.exe`를 실행합니다. 플러그인이 두 helper를 백그라운드에서 알아서 시작합니다.

<p align="center">
  <img src="images/trafficmonitor-tray-icon.png" alt="TrafficMonitor 떠 있는 창과 트레이 아이콘" />
</p>

작업 표시줄 창이 안 보이면 트레이 아이콘이나 떠 있는 창을 우클릭해 `Show Taskbar Window`를 누릅니다.

<p align="center">
  <img src="images/trafficmonitor-tray-menu-show-taskbar.png" alt="Show Taskbar Window가 있는 TrafficMonitor 트레이 메뉴" />
</p>

### 5. 항목 켜기

작업 표시줄 창을 우클릭해 `Display Settings...`를 엽니다.

<p align="center">
  <img src="images/trafficmonitor-taskbar-menu-display-settings.png" alt="Display Settings가 있는 TrafficMonitor 작업 표시줄 메뉴" />
</p>

원하는 항목을 체크하고 `OK`를 누릅니다.

- `Claude 5h`, `Claude 7d`, `Codex 5h`, `Codex 7d`: 한도별 사용률
- 선택: `Claude Fable 7d`(Fable 주간 한도), `Codex credits`(크레딧 잔액), `Claude resets`·`Codex resets`(초기화권 개수)

<p align="center">
  <img src="images/trafficmonitor-display-settings.png" alt="Claude와 Codex 항목을 켠 TrafficMonitor Display settings" />
</p>

### 6. Claude 한 번 로그인

TrafficMonitor 폴더에서 PowerShell을 열고(예: `cd D:\TrafficMonitor`) 실행합니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 login
```

helper 전용 프로필로 브라우저 창이 열립니다. 거기서 Claude에 로그인하고 창을 닫습니다. 실행 중인 helper가 다음 갱신 때(5분 안) 로그인을 읽어 값을 보여 주고, 로그인은 이후 업데이트에도 유지됩니다.

### 7. Codex가 다른 폴더에 있을 때

Codex 상태는 `%USERPROFILE%\.codex`에서 읽습니다. 다른 곳에 있으면 TrafficMonitor를 실행하기 전에 Windows 환경 변수 `CODEX_HOME`을 설정합니다.

## 새 버전으로 업데이트

로그인, 설정, 켜 둔 항목은 그대로 유지됩니다.

1. TrafficMonitor를 종료합니다(트레이 아이콘 우클릭 → `Exit`). 실행 중에는 플러그인 DLL을 바꿀 수 없습니다.
2. 두 helper를 멈춥니다. helper는 TrafficMonitor를 종료해도 계속 돌아갑니다. PowerShell에서 TrafficMonitor 폴더로 이동한 뒤 실행합니다.

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 stop
   powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\codex-usage-helper.ps1 stop
   ```

3. 새 zip을 TrafficMonitor 폴더에 풀고, 기존 파일을 덮어씁니다.
4. TrafficMonitor를 실행합니다. helper가 새 파일로 다시 시작됩니다.

## 설치 확인

- TrafficMonitor의 플러그인 관리 화면에 `AI Usage Limits`가 zip 이름과 같은 버전으로 나옵니다.
- 작업 표시줄 항목에 `--` 대신 숫자가 나옵니다. Claude는 로그인이 필요하고, Codex는 최근 Codex 사용 기록이나 서버 응답이 있어야 합니다.
- 작업 표시줄 창에 마우스를 올리면 Claude, Codex, Reset Credits 구역이 나옵니다.
- helper 상태 명령이 실행 중인 watcher와 최근 스냅숏을 보여 줍니다.

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 status
  powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\codex-usage-helper.ps1 status
  ```

문제가 있으면 [troubleshooting.md](troubleshooting.md)를 봅니다.

## 권장 TrafficMonitor 설정

- `General settings`에서 `Auto run when Windows starts`를 켜 두면 로그인한 뒤 작업 표시줄 항목이 다시 나옵니다.
- `Show Taskbar Window`를 켜 둡니다.

## 제거

1. [새 버전으로 업데이트](#새-버전으로-업데이트)의 1·2단계대로 TrafficMonitor를 종료하고 두 helper를 멈춥니다.
2. `plugins\ClaudeUsagePlugin.dll`과 `plugins\ClaudeUsagePlugin` 폴더를 지웁니다.
3. Claude 로그인 쿠키를 포함한 로컬 데이터까지 지우려면 `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin`을 지웁니다([PRIVACY.md](../PRIVACY.md)).
