# HAT10K Ventura 原生 Wi-Fi 交接文檔（2026-11-21）

## 這是什麼

目標：讓 QCA6174（`168c:003e`，子系統 `11ad:0807`）在 macOS 13.x Ventura 上以 **真正原生** 形式運作——出現在「系統設定 → Wi-Fi」、可開關、可掃描、可 WPA2 連線，且**不用相容層、不用獨立 app、不要求使用者開終端機**。驗證由 agent 自己在 VM 內完成（使用者留在 Ubuntu，只接實體網路線）。

**誠實狀態**：介面已能在 VM 內**建立、註冊並發布到 IORegistry**（驗收條件 A 通過：`Ath10kIfacePhase = "inited"`、`Ath10kStartPhase = "interface-published"`、`ioreg -r -c IO80211Interface` 有 23 行）。**尚未**取得 BSD 介面單元（`ifconfig -l` 仍只有 lo0/gif0/stf0/en0，`networksetup` 沒有 Wi-Fi 埠），因此系統設定看不到 Wi-Fi。硬體驗證（實體 ESP 上的 stage0）尚未重新部署。

分支：`ventura-io80211`（本輪成果 commit 於此分支；介面已發布、BSD 單元未取得）。

## 目前唯一權威狀態

VM：macOS **13.7.8 / 22H730**，`IO80211Family 1200.13`（v1 世代）。kext bundle `br.com.jonasw8.driver.ath10kmac` 0.0.105，OpenCore 1.0.5 DEBUG（`Target=67`、`ShowPicker=false`、`Timeout=0`）。

最近一次開機的實測結果（IORegistry 斷點，權威）：

| 斷點屬性 | 值 | 意義 |
|---|---|---|
| `Ath10kProbeEntry` / `Ath10kProbeMatched` | Yes / Yes | PCI personality 匹配成功 |
| `Ath10kVenturaLogPhase` | `ready` | corecapture 三條 pipe + LogStream + FaultReporter 全部建立成功 |
| `Ath10kStartPhase` | `interface-create-failed` | controller 已啟動，但介面建立失敗 |
| `Ath10kIfacePhase` | `super-init-failed` | 失敗點精確落在 `super::init(controller)`（`IO80211Interface::init(IONetworkController*)`） |
| `ioreg -r -c IO80211Interface` | 0 筆 | 介面未發布 |
| `networksetup -listallhardwareports` | 只有 `serial1`、`en0` | 系統設定沒有 Wi-Fi |

`kmutil showloaded` 可見 `br.com.jonasw8.driver.ath10kmac (0.0.105)`，**無 kernel panic**。

## 不可破壞的規則（每條附一句理由）

1. **Ventura 建置不得引用 `IO80211FaultReporter`** —— 它在 13.x 核心不存在，OpenCore 解析不到 undefined symbol 就會**靜默跳過整個 kext**（現象：kext 完全沒載入、無任何日誌）。
2. **`Ath10kPCIDevice` 必須覆寫 `enable/disable(IO80211SkywalkInterface*)` 與 16 個 `_RESERVEDIO80211ControllerN()`** —— 缺任一個，OpenCore 的 vtable 重建失敗 → `Prelinked injection … Invalid Parameter`。
3. **先 `createWorkLoop()` 再跑 corecapture** —— `IO80211Controller::start()` 之前 work loop 是 null，`CCPipe` 建立會失敗（曾以 `Ath10kVenturaLogPhase = "no-workloop"` 呈現）。
4. **Ventura 分支不要呼叫 `IO80211Controller::start()`** —— 它無條件走 `findAndAttachToFaultReporter()`，取不到 fault reporter 就在 `logDebug()` 內 panic（`"No ivars->_faultReporter" @IO80211Controller.cpp:3604`）。改成呼叫 `IOEthernetController::start(provider)` 繞過。
5. **除錯一律用 IORegistry 斷點，不要靠 IOLog** —— 此 VM 的 kext `IOLog`/`kprintf` 不會出現在 `log show`、`dmesg`（dmesg 有 1303 行、零 att10k）。斷點寫在 `provider`（PCI nub）上才看得到；寫在 driver 物件 `this` 上，start 失敗時節點不存在 → 看不到。
6. **部署前必須 `oc-enable.py`** —— `deploy_v*.sh` 樣板只跑 `oc-auto.py`（只設選單/日誌），不會啟用 kext。漏了就會得到「完全沒有斷點」的假象。

## 已知陷阱（命令式）

- **不要** 在 VM 正在 panic 循環時跑 `scp` 進 guest —— 會得到 `Connection closed`。先跑 `/tmp/disable_boot.sh`（=`oc-disable.py` + `oc-auto.py` + 開機）取得可登入的 guest，再編譯。
- **不要** 用 `pgrep -q`（這台 Ubuntu 上不支援），也不要 `pkill -f qemu-nbd`（會被 Hermes 安全層擋掉）。改用 `systemctl --user stop qca-vm`，必要時 `kill <PID>`。
- **不要** 用 `ioreg | grep 0x168c` 判斷網卡是否存在 —— ioreg 印的是 byte-swapped 小端（`168c` → `8c160000`）。用 `system_profiler SPPCIDataType` 最穩。
- **不要** 把 sed 產生的部署腳本裡第二個 python 指令留在 `S` 之外 —— 少了 sudo 會 `PermissionError: /mnt/ocv3/EFI/OC/config.plist`。
- **不要** 用 otool/capstone 以「符號名子字串」比對 `Ath10kPCIDevice5start` —— 會撞到 `__ZZN15Ath10kPCIDevice5startEP9IOServiceE6states`；且 capstone 需 `md.skipdata=True` 才不會提早停。
- **解析 kernel collection**：抽出單一 kext 的 Mach-O 後不能直接解析它的 `LC_SYMTAB`（offset 是整個 KC 的絕對位置），要對完整 `.kc` 解析。
- **qemu-nbd**：VM 還開著時掛載會 `Failed to get write lock`；先停 VM。掛載後要等 `/dev/nbd0p1` 出現（最多 15 秒）。
- **此 VM 的 IO80211 內部字串**（`IO80211_kexts-1132.58`、`1150.2`）不是實際版本，實際是 `1200.13`（v1）。

## 檔案地圖

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | `probe()` / `start()`：Ventura 分支繞過 Apple start、建 work loop、備 corecapture、手動 `createInterface()`+`attachInterface(&iface, true)`；所有 `Ath10kStartPhase` 斷點 |
| `src/kext/ventura/Ath10kVentura.cpp` | `prepareVenturaLogs()`（3 pipe + LogStream + FaultReporter，`Ath10kVenturaLogPhase` 斷點）、`releaseVenturaLogs()`、`getLogPipes()`、`createInterface()`、`apple80211Request` 分派 |
| `src/kext/ventura/Ath10kVenturaInterface.cpp/.hpp` | `Ath10kVenturaInterface : IO80211Interface`，`init()` 內有 `Ath10kIfacePhase` 斷點；`inputPacket()` 把 EAPOL 交給 IO80211 supplicant |
| `src/kext/ventura/Ath10kVenturaDecl.inc` | Ventura 專屬宣告（Skywalk enable/disable、16 個 reserved slot、`getLogPipes`、corecapture 成員） |
| `src/kext/ventura/Ath10kVenturaABI.hpp` | `__IO80211_TARGET __MAC_13_0` + 引入 `CCPipe.h`/`CCStream.h`/`CCLogStream.h`/`FaultReporter.h` |
| `vendor/itlwm-v1/Airport/FaultReporter.h` | 補上 v1 樹缺的 `CCDataStream`/`CCFaultReporter`/`CommonFaultReporter` 宣告 |
| `vendor/itlwm-v1/Airport/IO80211Controller.h`、`CCLogStream.h` | `enum CCStreamLogLevel` 已加 `ATH10K_CCSTREAMLOGLEVEL_DEFINED` 防重複定義 |
| `src/kext/native/*` | Sequoia（v2）可參考的對照實作：`getLogPipes()`、`getFaultReporterFromDriver()`、`prepareNativeLogs()` |
| `scripts/prepare-kext-info.py`、`Makefile` | `VENTURA=1` 建置路徑；personality `IOPCIMatch 0x003e168c` / `IOPCISecondaryMatch 0x080711ad` / `MinKernel 22.0.0` / `MaxKernel 23.99.99` |
| `.github/workflows/ventura-kext.yml` | CI 產出 `Ath10kMac-Ventura`（macos-15-intel、Xcode 16.4、macOS 13 SDK） |
| `docs/hardware-verification.md` | 實機 stage0/stage3 陷阱與 hash 比對流程 |

### 重跑指令（Acer `100.64.0.152`）

```bash
# 1) 同步原始碼到 guest（guest 內 macOS 才能編譯）
cd ~/Data/Dev/qca6174-native && tar czf /tmp/qca-src.tgz --exclude=build --exclude=.git --exclude=artifacts-ci* .
scp -q /tmp/qca-src.tgz 100.64.0.152:/tmp/
ssh 100.64.0.152 'scp -q -P 2223 /tmp/qca-src.tgz user@127.0.0.1:/tmp/ && ssh -p 2223 user@127.0.0.1 \
  "tar xzf /tmp/qca-src.tgz -C ~/qca && cd ~/qca && make VENTURA=1 -j2 kext"'

# 2) 部署 + 開機（腳本在 Acer 的 /tmp）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_v13.sh'    # 已含 oc-enable.py + oc-auto.py

# 3) 讀斷點
ssh 100.64.0.152 'ssh -p 2223 user@127.0.0.1 \
  "ioreg -l -w0 | grep -E \"Ath10kIfacePhase|Ath10kStartPhase|Ath10kVenturaLogPhase\""'
```

VM 若在 panic 循環：`cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/disable_boot.sh'`（停用 kext 後開機，取得可登入 guest）。

推送：`git add -A && git commit -m "…" && git push origin ventura-io80211`

## 下一個焦點（含驗收條件）

**已解決（本輪，勿重查）**：`IO80211Interface::init()` 之所以回傳 false，根因在它尾巴呼叫的 `IOEthernetInterface::init(IONetworkController*)`：

1. 該函式**要求 controller 上有 `IOPacketFilters`（OSDictionary）屬性**，內含 `IONetworkFilterGroup` / `IOEthernetWakeOnLANFilterGroup` / `IOEthernetDisabledWakeOnLANFilterGroup`（`IOEthernetDisabledWakeOnLANFilterGroup` 必須是 OSNumber）。屬性不存在或型別不對 → 直接 `return false`。
2. `attachInterface()` 內部的 `configureInterface()` **要求 controller 有 output queue**（`getOutputQueue()` 非 null），bypass 路徑不會建立它。
3. `attachInterface()` 會**自己再呼叫一次 `createInterface()`**，並把結果寫回 call-by-ref 指標；直接呼叫時要自己補 `configureInterface()` → `iface->attach(this)` → `registerService()` → `attachToDataLinkLayer()`，並把介面寫進 `IO80211Controller` 的 ivar（offset `0x200`）。
4. 呼叫底層實作必須用 mangled 名稱（`extern "C" ... __asm("__ZN19IOEthernetInterface4initEP19IONetworkController")`）。用 `IOEthernetInterface::init(controller)` 這種 qualified C++ 寫法會被編成 **virtual dispatch**，又繞回 `IO80211Interface::init`。

**當前唯一缺口：BSD 介面單元**。`iface->attachToDataLinkLayer(0, nullptr)` 回傳 true，但 `ifconfig -l` 沒有新介面，`ioreg -r -c IO80211VirtualInterface` / `IO80211InfraInterface` 也都是 0 筆——Ventura 的 BSD 介面屬於 `IO80211InfraInterface`（`IO80211Interface` → `IO80211VirtualInterface` → `IO80211InfraInterface`）那一層，而 virtual interface 是 Apple 的 `IO80211Controller::start()` 建立的，bypass 路徑跳過它。

**兩個已試過但失敗的路（勿重試）**：

1. `super::start(provider)`（= `IO80211Controller::start`）→ kernel panic `"No ivars->_faultReporter" @IO80211Controller.cpp:3604`。
2. 手動把 vtable 的 `+0xd20`（`getLogPipes`）/`+0xd58`（`getFaultReporterFromDriver`）改成我們的實作 → 一樣 panic，而且 panic 訊息引用的字串是 **`IO80211_kexts-1132.58`**，不是 `kmutil` 看到的 `IO80211Family 1200.13`。也就是說「實際承載我們 controller 的那份 IO80211 是舊世代」，`/tmp/vt_both.txt` 裡 0x20d86f8 那份（Skywalk 世代）不是它。程式碼保留在 `alignVenturaVtable()` 並標註為負面結果，**不要呼叫**。

**下一步（重新校正方向）**：先用正確的 vptr 慣例（物件 vptr 指向 `&vtable[2]`，所以 kernel 端 `[vptr+0xNNN]` 等於 `vtable + 0xNNN + 0x10`）把「實際世代」的 `findAndAttachToFaultReporter` 呼叫鏈逐一解出來，再決定要補哪個 virtual；在那之前不要再改任何 slot。

下一步要做的事：

1. **回頭讓 `IO80211Controller::start()` 真正跑成功**（架構上正確的路），而不是繼續在 bypass 上補洞。缺的最後一塊是 `getFaultReporterFromDriver()` override：`findAndAttachToFaultReporter()` 的流程是 `[vptr+0xd10]`（= `getLogPipes`）→ 結果存進 `ivars+0x50`，接著 `[vptr+0xd48]`（= `getFaultReporterFromDriver`）→ 存進 `ivars+0x68`。我們已經有 `getLogPipes` override，卻沒有第二個；Sequoia 分支的對照實作在 `src/kext/native/Ath10kControllerDecl.inc:15`。**注意**：`vendor/itlwm-v1/Airport/IO80211Controller.h` 沒有宣告這個 virtual，直接加 override 會排到 vtable 尾端導致錯位，必須先把它插進 header 的正確位置（v1 順序：`getLogPipes` 之後）。
2. 成功後把 Ventura 分支改回 `super::start(provider)`（`IO80211Controller::start`），並移除 `start()` 裡手動 publish packet filters / output queue / create+configure+attach / `attachToDataLinkLayer` 那一整段（Apple 會自己做，含 virtual interface 與 BSD 單元）。
3. 驗收條件 B–E 照原順序推進。

驗收條件（依序）：

- A：`Ath10kIfacePhase = "inited"` 且 `Ath10kStartPhase = "interface-published"`。
- B：`ioreg -r -c IO80211Interface` ≥ 1 筆，`networksetup -listallhardwareports` 出現 Wi-Fi 介面。
- C：「系統設定 → Wi-Fi」可見且開關可操作（截圖為證）。
- D：可掃到 AP 並以 WPA2 連線、取得 IP（`ifconfig` + ping 為證）。
- E：以上全程不修改系統設定以外的東西、不需終端機、無獨立 app；最後才把 kext 部署回實體 ESP 並在真機重驗。

## 建議技能

- `osx-kvm-headless-install`：VM 生命週期、qemu-nbd、SSH 進 guest 的操作細節。
- `macos-kext-hardware-bringup`：kext 建置/注入、OpenCore 設定與硬體驗證流程。
- `handoff`：下次再交接時沿用本文件結構。
