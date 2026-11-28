<div align="center">

# Ath10kMac

**Native Qualcomm Atheros QCA6174 / QCA9377 Wi-Fi for macOS on x86_64 — as a plain kext.**

No companion app · No OpenCore Legacy Patcher · No system-volume changes · Wi-Fi from the menu bar and System Settings

[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%2013%20Ventura%20%C2%B7%20x86__64-lightgrey.svg)](#compatibility)
[![PCI](https://img.shields.io/badge/PCI-168c%3A003e%20%C2%B7%20168c%3A0042-informational.svg)](#compatibility)
[![Kext](https://img.shields.io/badge/kext-Ath10kMac.kext-success.svg)](#install)
[![First](https://img.shields.io/badge/first-QCA6174%20scans%20in%20the%20native%20Wi--Fi%20UI-critical.svg)](#what-is-new-here)
[![Docs](https://img.shields.io/badge/docs-Apple%20join%20admission%20conditions-orange.svg)](docs/handoff-apple-join-conditions.md)

[English](#english) · [正體中文](#正體中文)

</div>

> **Branch: `ventura-io80211`** — the active development line (macOS 13 Ventura, QCA6174 hw3.x), and the
> repository's **default branch**: `git clone` lands here. Everything in this README describes this
> branch. `main` is a frozen earlier milestone tree — see [Branches](#branches).

---

## English

### What this is

Ath10kMac is a clean-room IOKit driver that makes a **Qualcomm Atheros QCA6174 / QCA9377** work as the
primary Wi-Fi interface of macOS, using the firmware protocol of the Linux `ath10k` driver as its
reference. It ships as one kext injected by OpenCore; macOS talks to it through Apple's own
`IO80211Family`, so the join UI, the menu-bar network list and `networksetup` all behave natively.

Association, the WPA2 four-way handshake and key installation run **inside the kernel**. There is no
user-space supplicant, no login agent and no privileged helper to install.

### Branches

There is more than one tree in this repository, and they are at very different stages. Pick the branch
by what you want, not by what the default branch happens to be.

| Branch | What it is | State | Last activity |
|---|---|---|---|
| **`ventura-io80211`** ← *you are here, and the default* | Active line: QCA6174 on macOS 13 Ventura. Native UI scanning, in-kernel WPA2, the reverse engineering in [`docs/`](docs/README.md), the `tools/` measurement suite. | **most advanced** — scan + WPA2 + reconnect working in the test VM; click-to-join blocked in airportd | 2026-11-27 |
| `main` | Earlier staged-milestone tree (M0/M1: PCI probe, firmware/board parsing, host-side models), frozen and kept for history. Its README describes that stage, not this one. | superseded | 2026-10-29 |
| `native-qca6174-dev` | Pre-Ventura development line for the native adapter. | historical | 2026-10-29 |
| `native-qca6174-11ad-0807` | Same line pinned to the Lite-On `11ad:0807` subsystem, packaged as a deployable bundle. | historical | 2026-10-29 |
| `full-driver-11ad-0807` | Full-driver snapshot for that subsystem (ASPM/Link-Control work). | historical | 2026-10-29 |

A plain clone already lands on the active line; the explicit form just makes it obvious:

```sh
git clone -b ventura-io80211 https://github.com/duckducktw/qca6174-macos.git
```

`upstream-sadfrozz/main` and `upstream-sadfrozz/native-sequoia-development` are the upstream remotes
this work derives from; they are not maintained here.

### What is new here

**First public driver to put QCA6174 scan results into macOS's own Wi-Fi UI.**
Not "a list the driver prints" — the real thing: the scan sweeps that this kext performs are served
through Apple's `IO80211ScanManager` to `airportd`, and `airportd` hands them to CoreWLAN, so the
networks appear in the menu-bar list and in System Settings, with SSID, BSSID, channel, band, RSSI,
security and RSN element. No other public QCA6174 kext has been shown to do this; it is reproducible
with `tools/cwscan.m` (15–21 BSS per sweep) and `tools/apple80211-scan.c` (raw records:
`QCA-TEST bssid=f0:68:e3:2d:40:22 ch=6 ie=22`).

Getting there meant satisfying the scan half of Apple's contract, which is documented rather than
guessed: one `apple80211_scan_result` per call with a non-zero end-of-list marker, the scan-manager
request validated against `validateScanData`, results served on the slot the manager actually reads
(`(controller, cmd, type, interface, out)`), and `M_SCAN_DONE` posted on both the Skywalk path and the
`kern_event` bridge airportd's reader thread consumes.

**The join flow is the open problem.** Clicking a network — the step after scanning — still fails:
`airportd` refuses before any request reaches the driver, so no `Set(SSID)` and no `ASSOCIATE(20)` is
ever sent. Everything on the driver side of that boundary is aligned and measured; where exactly the
refusal happens, and what Apple requires at each layer, is written down in
**[Apple's join admission conditions](docs/handoff-apple-join-conditions.md)**. This README does not
claim a working click-to-join anywhere.

### Project status

| Area | State | Evidence |
|---|---|---|
| firmware boot (BMI → WMI), PCIe, CE/IRQ runtime | **working** | `Ath10kWmiLive=active`, boot ladder on the serial port |
| scan, 2.4 / 5 GHz, RSN elements | **working** — 15–21 BSS per sweep | `tools/apple80211-scan.c`, `tools/cwscan.m` |
| **scan results in the native Wi-Fi UI** (menu bar, System Settings) | **working — first for this chip** | `cwscan.m` returns 15–21 networks through CoreWLAN → airportd; `Ath10kScanMgrStart=ok`; records served one per call with the end-of-list marker |
| WPA2-PSK connect, in-kernel 4-way, DHCP | **working** | `Ath10kConnectionState=connected-wpa2`, `WPA2 port authorized` |
| reconnect after drop / radio cycle | **working** | auto-join watchdog, `Ath10kAutoJoinPhase` |
| driver-initiated join (boot-arg join list) | **working** | `ath10kjoin=<ssid>:<psk>` |
| **click-to-join — the join flow** | **partly working (v281)** — the admission gate is passed: airportd now sends `ASSOCIATE(20)` itself (`Ath10kAssocWho=pid=<airportd>`) and the driver completes the link by firing its known-good join on that request (`Ath10kAssocKick=watchdog-only`) → `Current Wi-Fi Network: QCA-TEST`, `status: active`, ping OK. Still open: after a **radio power cycle** the client reports `Could not find network` (empty scan cache), and the association that finishes is the driver's, not airportd's own 4-way | `Ath10kAssocKick`, `Ath10kAssocWho`, `Ath10kRoamProfile=set-accepted`, `Ath10kCapsShape=caps-at-0` |
| WPA3 / SAE | **partial** — in-kernel P-256 verified (`Ath10kP256SelfTest=mul-ok`), SAE frame exchange not wired | [WPA3-CONTRACT.md](WPA3-CONTRACT.md) |
| PMF / 802.11w | **partial** — linked and gated, not exercised against a PMF-required AP | [WPA3-CONTRACT.md](WPA3-CONTRACT.md) |
| AWDL / AirDrop | **unsupported** | [COMPATIBILITY.md](COMPATIBILITY.md) |
| QCA9377 (`168c:0042`) | **experimental** — code paths exist, hardware verification pending | [COMPATIBILITY.md](COMPATIBILITY.md) |

Every result above was measured on a **macOS 13.7 Ventura VM with a VFIO-passthrough QCA6174** against a
local `hostapd` AP. Real-hardware verification is the next milestone and is tracked in
[docs/hardware-verification.md](docs/hardware-verification.md). Nothing here is a stability promise for
your machine.

### How it works

```
        System Settings / menu bar / networksetup
                        │  CoreWLAN · XPC
                    airportd
                        │  Apple80211 / IO80211 requests
              IO80211Family  (Apple, unmodified)
                        │  vtable slots: apple80211_ioctl, apple80211SkywalkRequest,
                        │                setAssociateRequest, scan manager
                  Ath10kMac.kext            <- this repository
        ┌───────────────┴───────────────┐
        │ IO80211 personality           │  scan cache, association state machine,
        │                               │  in-kernel WPA2 supplicant (PBKDF2,
        │                               │  4-way, CCMP key install)
        │ ath10k core                   │  BMI/WMI bring-up, CE rings, HTT RX/TX
        └───────────────┬───────────────┘
                QCA6174 firmware
```

The interesting half of this project is not the radio — it is the **admission contract** of Apple's
stack: which requests airportd sends in which order, which of the driver's answers make it continue or
give up, and where each refusal is produced. That reverse engineering is written down:

- **[Apple's join admission conditions](docs/handoff-apple-join-conditions.md)** — the four layers a
  click-to-join must pass, each with its disassembly address and measured value.
- **[Apple vs. ours](docs/apple-vs-ours.md)** — per-API comparison of what the live stack expects and
  what this driver answers.
- **[Reverse-engineering playbook](docs/reverse-engineering-playbook.md)** — toolchain (dyld shared
  cache dumping, chained-fixup decoding, cfstring/selref resolution) and method.
- **[Join flow reversal](docs/join-flow-reverse.md)** — the userspace call chain down to
  `Apple80211Associate2` and its own two `-3903` exits.

### Compatibility

| Chip | PCI ID | macOS | State |
|---|---|---|---|
| QCA6174 hw3.x | `168c:003e` | 13 Ventura (13.7 tested) | scan, WPA2, reconnect working in the test VM; hardware pending |
| QCA9377 hw1.x | `168c:0042` | 13 Ventura | experimental, unverified |
| other Ath10k | — | — | out of scope |

Per-release matrix and ABI caveats: [COMPATIBILITY.md](COMPATIBILITY.md).

### Install

1. Build or download `Ath10kMac.kext`.
2. Copy it to `EFI/OC/Kexts/` and add it under `Kernel → Add` in `config.plist`
   (`BundlePath=Ath10kMac.kext`, `ExecutablePath=Contents/MacOS/Ath10kMac`,
   `PlistPath=Contents/Info.plist`, `MinKernel=22.0.0`).
3. Reboot. The interface appears as `en1`; join it from the menu bar, System Settings or
   `networksetup -setairportnetwork en1 <ssid> <passphrase>`.

For unattended bring-up a join list can be supplied as a boot argument
(`ath10kjoin=<ssid>:<psk>[,<ssid>:<psk>]`). Boot arguments live in NVRAM in clear text — prefer the UI
once click-to-join lands.

### Build

```sh
make -j2 kext                     # → build/native/out/Ath10kMac.kext
make NATIVE=1 DIAGNOSTIC_STAGE=0 -j2 kext
```

`DIAGNOSTIC_STAGE=3` builds the controller-only diagnostic, which stops before radio start-up; it is
not a Wi-Fi driver. `NATIVE=0` keeps the historical Ethernet-backed implementation for development.
Windows cross-compilation: [docs/windows-build.md](docs/windows-build.md).

```sh
python3 scripts/audit-native-callbacks.py --check
python3 tests/run-development-tests.py
```

### Verify a running system

```sh
ioreg -l -w0 | grep -E 'Ath10k(ConnectionState|AutoJoinPhase|SecurityPhase)'   # link state
ioreg -l -w0 | grep -o 'Ath10kClientTrace" = "[^"]*"'                          # client request trace
networksetup -getairportnetwork en1
ifconfig en1 | grep -E 'status|inet '
```

The driver publishes ~120 `Ath10k*` properties. They are this project's instrumentation, and the reason
most of the reverse engineering above could be measured instead of guessed.

### Documentation

| Document | What it answers |
|---|---|
| [docs/handoff-apple-join-conditions.md](docs/handoff-apple-join-conditions.md) | **start here** — every condition Apple requires before admitting a join, per layer, with evidence |
| [docs/apple-vs-ours.md](docs/apple-vs-ours.md) | per-API diff between the live stack's expectations and this driver |
| [docs/reverse-engineering-playbook.md](docs/reverse-engineering-playbook.md) | toolchain, method, pitfalls, community prior art |
| [docs/join-flow-reverse.md](docs/join-flow-reverse.md) | userspace → family → driver call chain of a join |
| [docs/HANDOFF.md](docs/HANDOFF.md) | full engineering log, round by round, with measurements |
| [COMPATIBILITY.md](COMPATIBILITY.md) | macOS release matrix and ABI caveats |
| [NATIVE-CONTRACT.md](NATIVE-CONTRACT.md) | the contract this driver implements towards IO80211Family |
| [WPA3-CONTRACT.md](WPA3-CONTRACT.md) · [SAE-CRYPTO-BACKEND.md](SAE-CRYPTO-BACKEND.md) | WPA3/PMF state, SAE crypto backend investigation |
| [INITIAL-POWER-CONTRACT.md](INITIAL-POWER-CONTRACT.md) | power-on ordering and recovery |
| [NATIVE-CONNECTION-AUDIT.md](NATIVE-CONNECTION-AUDIT.md) · [OFFLINE-VALIDATION-ERRATUM.md](OFFLINE-VALIDATION-ERRATUM.md) | connection audit, limits of offline validation |
| [docs/hardware-verification.md](docs/hardware-verification.md) | what was verified on which machine, and how |
| [docs/legacy-background.md](docs/legacy-background.md) · [docs/native-sequoia.md](docs/native-sequoia.md) | project history, the Sequoia line |

### Limitations we do not hide

- **Click-to-join does not work yet.** The refusal is inside `airportd`, before any request reaches the
  driver. Every driver-side contract we could find is aligned and documented; the remaining layer is
  being narrowed with `Ath10kClientTrace`.
- **Association budget.** Apple's `Apple80211Associate2` waits 10.2 s; a teardown + rejoin here measures
  9–11 s, so a join that must switch networks can miss the window.
- **Unverified hardware.** Everything is measured in a VM. The development laptop still needs the same
  measurements before any stability claim.
- No WPA3/SAE join, no AWDL/AirDrop, no SoftAP.

### Contributing

Measurements beat opinions here. A useful bug report carries:

```sh
ioreg -l -w0 | grep Ath10k > ioreg.txt
sudo dmesg | grep Ath10kMac > serial.txt      # or the serial console log
log show --last 2m --predicate 'process == "airportd"' --style compact | grep -iE 'join|assoc|error=\('
```

plus the chip ID (`ioreg -l | grep -m1 compatible`) and the macOS build. A new `Ath10k*` breadcrumb is
usually the right first patch — see [docs/reverse-engineering-playbook.md](docs/reverse-engineering-playbook.md) §3.

### Credits

- Apple's `IO80211Family` — the interface this driver implements. No Apple binary is modified or
  redistributed.
- [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) — prior art for this class of
  driver; its AirportItlwm showed which parts of the IO80211 contract matter for a working join
  (comparison: [docs/handoff-apple-join-conditions.md](docs/handoff-apple-join-conditions.md) §5).
- The Linux `ath10k` driver — firmware protocol reference.
- [jonasw8/Ath10kMac](https://github.com/jonasw8/Ath10kMac),
  [thekunalthakur/Ath10kMac](https://github.com/thekunalthakur/Ath10kMac) and
  SadFrozz/Ath10k-airport — earlier trees this work derives from. Private declarations and local ABI
  amendments are recorded in `vendor/itlwm/SOURCE.json`.
- MacKernelSDK, firmware and regulatory data retain their own notices (`MacKernelSDK/`, `firmware/`,
  `regulatory/`).

### License

GPL-3.0 — see [LICENSE](LICENSE). Kernel extensions run with kernel privileges: a broken kext can
prevent a machine from booting, and there is no warranty of fitness.

---

## 正體中文

### 這是什麼

Ath10kMac 是一個以 IOKit 重寫的 Wi-Fi 驅動，讓 **Qualcomm Atheros QCA6174 / QCA9377** 成為 macOS 的
主要無線網卡，韌體協議參考 Linux 的 `ath10k`。整個驅動只有一個 kext，由 OpenCore 注入，透過 Apple
自己的 `IO80211Family` 與系統溝通 —— 連線介面、選單列網路列表、`networksetup` 都是原生行為。

802.11 關聯、WPA2 四次握手、金鑰安裝**全部在核心內完成**：不需安裝任何使用者態 supplicant、
沒有登入代理、沒有特權 helper。

### 分支說明（先看這個）

這個 repo 有不只一條樹，進度差很多。請依「你要什麼」選分支，而不是依「預設分支是哪一條」。

| 分支 | 這是什麼 | 狀態 | 最後活動 |
|---|---|---|---|
| **`ventura-io80211`** ← *你在這裡，也是預設分支* | 主開發線：macOS 13 Ventura + QCA6174。原生介面掃描、核心內 WPA2、[`docs/`](docs/README.md) 的逆向文件、`tools/` 量測工具都在這。 | **最新** — 掃描／WPA2／重連在測試 VM 可用；點選連線受阻於 airportd | 2026-11-27 |
| `main` | 較早的階段性里程碑樹（M0/M1：PCI probe、firmware/board 解析、host 端模型），已凍結、只保留歷史；它的 README 描述的是那個階段。 | 已取代 | 2026-10-29 |
| `native-qca6174-dev` | Ventura 之前的 native adapter 開發線。 | 歷史 | 2026-10-29 |
| `native-qca6174-11ad-0807` | 同上，鎖定 Lite-On `11ad:0807` 子系統、打包成可部署 bundle。 | 歷史 | 2026-10-29 |
| `full-driver-11ad-0807` | 該子系統的 full-driver 快照（ASPM/Link-Control）。 | 歷史 | 2026-10-29 |

`git clone` 現在就會落在主開發線；下面是指定寫法，讓意圖更明確：

```sh
git clone -b ventura-io80211 https://github.com/duckducktw/qca6174-macos.git
```

### 首創：QCA6174 的掃描結果進到 macOS 原生 Wi-Fi 介面

**全球第一個讓 QCA6174 的掃描結果出現在 macOS 自己 Wi-Fi 介面上的公開驅動** ——
不是「驅動自己印一份清單」，而是真的走完整條鏈：本 kext 掃到的 BSS 交給 Apple 的
`IO80211ScanManager`，再由 `airportd` 遞給 CoreWLAN，於是選單列網路列表與「系統設定」裡就看得到
SSID、BSSID、頻道、頻段、RSSI、安全性與 RSN 元素。目前沒有其他公開的 QCA6174 kext 被證明做到這件事；
它可以用 `tools/cwscan.m`（每次 15–21 個 BSS）與 `tools/apple80211-scan.c`（原始紀錄：
`QCA-TEST bssid=f0:68:e3:2d:40:22 ch=6 ie=22`）複驗。

能做到是因為把「掃描那一半」的契約逐條滿足（不是猜的、都有文件）：每次呼叫回一筆
`apple80211_scan_result`、以非 0 回傳當結束標記、scan manager 的請求先過 `validateScanData`、
結果走 manager 真正會讀的那個 slot（`(controller, cmd, type, interface, out)`）、
`M_SCAN_DONE` 同時走 Skywalk 與 airportd 讀者執行緒吃的 `kern_event` 橋。

### join 流程：已經通到「按了會連上」，但最後一哩仍是驅動

v271–v281 這串修掉四個獨立缺陷後，狀態已經改變：

1. `CARD_CAPABILITIES` 改成「capabilities 從 offset 0 開始寫」（不加 version）——family 會把這塊放在
   `ref+0x50`，`Apple80211Associate2` 的門檻讀 `byte[1] & 0x40`；version 在前會讓所有能力位移 4 bytes，
   位元永遠對不上（這就是長久 `-3903` 的來源）。
2. `APPLE80211_IOC_ROAM_PROFILE`（216）補上 handler（set 接受、get 回零）——原本回 unsupported，
   airportd 送完 ASSOCIATE 後會因此放棄。
3. vif 的 BSD name 在 publish 時寫入 `[ivars+0x110]+1`（`getBSDName()` 就是讀這裡）。
4. 每個 client 建立時補 `createEventPipe()`，event pipe 陣列才不是空的（`Ath10kEventPipes` 0 → 24），
   事件投遞才從 `0xe00002c7` 變成 `0`。

**現在**：airportd 會自己送出 `ASSOCIATE(20)`（`Ath10kAssocWho=pid=<airportd>`），驅動收到就立刻打
same-known-good join（`Ath10kAssocKick=watchdog-only`）；實測按下連線後 `Current Wi-Fi Network: QCA-TEST`、
`status: active`、ping 1.1.1.1 通（約 20 秒內）。

**仍未解**（不粉飾）：
- 無線電 `power off → on` 之後，客戶端回 **`Could not find network QCA-TEST`**（掃描快取在該情境是空的）。
- 最後完成 4-way 的是驅動的 watchdog（`AssocWho=kernel_task`），不是 airportd 自己的 handshake；
  若你要的是「完成者必須是 airportd」，這條還沒到。

每一層要求什麼、拒絕在哪一層產生，仍以
**[蘋果放行 join 的四層條件](docs/handoff-apple-join-conditions.md)** 為準；
逐輪量測在 [docs/HANDOFF.md](docs/HANDOFF.md) §6.57–§6.64。

### 目前狀態

| 功能 | 狀態 | 證據 |
|---|---|---|
| 韌體啟動（BMI → WMI）、PCIe、CE/IRQ | **可用** | `Ath10kWmiLive=active` |
| 掃描（2.4G / 5G、含 RSN IE） | **可用**，每次 15–21 個 BSS | `tools/cwscan.m` |
| **掃描結果出現在原生 Wi-Fi 介面**（選單列、系統設定） | **可用 —— 這張卡的首例** | `cwscan.m` 經 CoreWLAN → airportd 取回 15–21 個網路；`Ath10kScanMgrStart=ok` |
| WPA2-PSK 連線（核心內 4-way）＋ DHCP | **可用** | `connected-wpa2`、`WPA2 port authorized` |
| 斷線重連 / 無線重啟 | **可用** | auto-join watchdog |
| 驅動自行連線（boot-arg 清單） | **可用** | `ath10kjoin=<ssid>:<psk>` |
| **點選連線（join 流程）** | **部分可用（v281）**：能力關卡已通過，airportd 會自己送出 `ASSOCIATE(20)`（`Ath10kAssocWho=pid=<airportd>`），驅動收到後立刻用它自己那套可用的 join 完成連線（`Ath10kAssocKick=watchdog-only`）→ `Current Wi-Fi Network: QCA-TEST`、`status: active`、ping 通。仍未解：**無線電 power cycle 之後**客戶端回 `Could not find network`（掃描快取空），且最後完成 4-way 的是驅動、不是 airportd 自己 | `Ath10kAssocKick`、`Ath10kAssocWho`、`Ath10kRoamProfile=set-accepted`、`Ath10kCapsShape=caps-at-0` |
| WPA3 / SAE | **部分**：核心內 P-256 已驗證，SAE 訊框交換未接線 | [WPA3-CONTRACT.md](WPA3-CONTRACT.md) |
| PMF / 802.11w | **部分**：已串接，未對強制 PMF 的 AP 實測 | 同上 |
| AWDL / AirDrop | **不支援** | [COMPATIBILITY.md](COMPATIBILITY.md) |
| QCA9377 (`168c:0042`) | **實驗性**，尚未硬體驗證 | 同上 |

所有數據來自 **macOS 13.7 Ventura VM + VFIO 直通 QCA6174** 與本機 `hostapd` AP；
實機驗證是下一個里程碑（[hardware-verification](docs/hardware-verification.md)）。

### 這個專案真正有價值的地方

不是無線電，而是**蘋果 stack 的「放行契約」**：airportd 依什麼順序問哪些請求、驅動的哪個回答
讓它繼續或放棄、每個拒絕在哪一層產生。這些逆向結果都寫成文件：

- **[蘋果放行 join 的四層條件](docs/handoff-apple-join-conditions.md)**（新接手先讀這份）
- **[AirportItlwm 等社群做法對照](docs/handoff-apple-join-conditions.md)**（§5）
- **[逆向工具鏈與流程](docs/reverse-engineering-playbook.md)**、**[逐 API 對照](docs/apple-vs-ours.md)**

### 建置與安裝

```sh
make -j2 kext        # → build/native/out/Ath10kMac.kext
```

把 kext 放進 `EFI/OC/Kexts/`，在 `config.plist` 的 `Kernel → Add` 加一筆（`MinKernel=22.0.0`），
重開機後介面會以 `en1` 出現。

### 已知限制（不粉飾）

- **點選連線還沒通**：拒絕在 `airportd`，不是驅動回錯值；驅動側可對齊的契約都已對齊並記錄。
- **關聯時間窗**：Apple 等 10.2 秒，我們的 teardown+rejoin 實測 9–11 秒，換網路時可能超窗。
- **實機尚未驗證**：所有量測都在 VM；在開發筆電重跑同一組量測前，不做穩定性宣稱。
- 沒有 WPA3/SAE 連線、沒有 AWDL/AirDrop、沒有 SoftAP。

### 授權

GPL-3.0，見 [LICENSE](LICENSE)。kext 以核心權限執行，壞掉的 kext 可能讓機器無法開機，請自行承擔風險。
