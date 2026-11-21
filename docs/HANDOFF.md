# HAT10K Ventura 原生 Wi-Fi 交接文檔（2026-11-22 01:20 更新）

## 這是什麼

目標：讓 QCA6174（`168c:003e`，子系統 `11ad:0807`）在 macOS 13.x Ventura 上以 **真正原生** 形式運作——出現在「系統設定 → Wi-Fi」、可開關、可掃描、可 WPA2 連線，且**不用相容層、不用獨立 app、不要求使用者開終端機**。驗證由 agent 自己在 VM 內完成（使用者留在 Ubuntu，只接實體網路線）。

**誠實狀態（2026-11-22 01:20）**：這輪把「無線電完全沒開機」這個更底層的阻塞解掉了，VM 內 **firmware 首次真的跑起來**（`Ath10kRecoveryPhase = ready`、`Ath10kWMIInitSucceeded = Yes`、serial 有 `READY Qualcomm Atheros QCA6174`），同時 `en2` 這個 **BSD 介面單元**也出現了。但 Wi-Fi 服務被 SystemConfiguration 歸類成 **Ethernet**（`IOInterfaceType = 6`），所以 `airportd` 一個 apple80211 請求都沒送進來（計數器實測 0），系統設定仍看不到可用的 Wi-Fi、B～E 後半全部未達成。

分支：`ventura-io80211`，最新 commit `5cf75ba`（已推遠端）。VM 目前跑 **v40 bundle**（`/tmp/vk40`，radio up + en2 + SSH 正常）。

## 目前唯一權威狀態（v40）

VM：macOS **13.7.8 / 22H730**，`com.apple.iokit.IO80211Family 1200.13.0`（載入基址 `0xffffff8001f35000`，長度 `0x19bff9`，結束於 `0x20d0ff9`），`IONetworkingFamily 3.4`。kext `br.com.jonasw8.driver.ath10kmac 0.0.105`，OpenCore 1.0.5 DEBUG（`Target=67`、`ShowPicker=false`、`Timeout=0`）。v40 hash 前 20 碼 `a0ac30d702e698c8d1d6`。

| 斷點屬性 | 值 | 意義 |
|---|---|---|
| `Ath10kProbeMatched` | Yes | PCI personality 匹配成功 |
| `Ath10kVenturaLogPhase` | `ready` | 3 pipe + LogStream + CCFaultReporter 建立成功 |
| `Ath10kIfacePhase` | `inited` | 介面 init 成功 |
| `Ath10kInterfaceType` | `ieee80211-after-attach` | attach 後已把 `IOInterfaceType` 蓋成 71 |
| `Ath10kStartPhase` | `interface-published` | registerService + attachToDataLinkLayer 完成 |
| `Ath10kAlignPhase` | `aligned` | vtable slot 補丁已寫入 |
| `Ath10kStackPhase` | `driver-owns-ifnet` | 不再呼叫 IONetworkStack（見「死了的路」第 6 條） |
| `Ath10kVifPhase` | `vif-create-failed` | `createVirtualInterface(addr,7)` 回 null |
| `Ath10kASPMControl` | `write-ignored-continued` | 平台不吃 ASPM 寫入，改為非致命（本輪關鍵） |
| `Ath10kPCIeParityPrepared` | Yes | bring-up parity 過了（以前卡死在這） |
| `Ath10kRecoveryPhase` / `Detail` | `ready` / `Firmware runtime is active; waiting for app scan/connect` | **firmware 真的活了** |
| `Ath10kWMIInitSucceeded` | Yes | WMI init 完成 |
| `Ath10kColdTargetInitSucceeded` | Yes | 冷啟動 target-init 成功 |
| `Ath10kFirmwareBootStatus` | `htc-ready-before-reset` | HTC ready 已到 |
| `Ath10kApple80211Requests` | （不存在） | **airportd 完全沒送請求進來**（本輪新增的計數器） |
| `ifconfig -l` | `lo0 gif0 stf0 en0 en2` | en2 = 我們的單元 |
| `ioreg -r -c IO80211Interface` | 2 筆 | 一筆 state 0（start 內自建）、一筆 `BSD Name=en2`、`IOInterfaceType=71` |
| `networksetup -listallhardwareports` | 有 `Hardware Port: Wi-Fi / Device: en2` | B 的前半達成 |
| `networksetup -listnetworkserviceorder` | `(3) Ethernet 2 (Hardware Port: Ethernet, Device: en2)` | **服務被歸類成 Ethernet → 這就是現在的卡點** |
| `networksetup -getairportpower en2` | `Off (forced)`，想開會說 “all AirPort network services are disabled” | airportd 不認這個介面 |

## 不可破壞的規則（每條附一句理由）

1. **Ventura 建置不得引用 `IO80211FaultReporter`、`IO80211WorkLoop::workLoop()`** —— 13.7 的注入環境解析不到，或讓 OpenCore 直接**靜默跳過整個 kext**。
2. **`Ath10kPCIDevice` 必須覆寫 `enable/disable(IO80211SkywalkInterface*)` 與 16 個 `_RESERVEDIO80211ControllerN()`** —— 缺任一個 → `Prelinked injection … Invalid Parameter`。
3. **先 `createWorkLoop()` 再跑 corecapture** —— `CCPipe` 建立需要 work loop。
4. **Ventura 分支不要呼叫 `IO80211Controller::start()`** —— 實測必 panic（fault reporter 在驅動能插手之前就被讀）。
5. **除錯一律用 IORegistry 斷點 + serial** —— kext 的 `IOLog` 在 `log show`/`dmesg` 看不到，但**會出現在 QEMU serial**（`/tmp/ventura-vm.serial`）：本輪 `Ath10kVenturaInterface: initIfnetParams failed`、`READY Qualcomm Atheros QCA6174` 都是從 serial 讀到的，比 IORegistry 快。
6. **部署前必須 `oc-enable.py`** —— 漏了就是「完全沒有斷點」的假象。
7. **vtable 位移一律用「vptr 相對」計算** —— vptr 指向 `&vtable[2]`，kernel 端 `[vptr+0xNNN]` = vtable `0xNNN+0x10`。
8. **不要碰 IONetworkStack 的插入函式** —— 它會把整個網路堆疊弄壞（見下文）。

## 這輪真正解掉的根因（勿重查）

1. **VM 內無線電從來沒開機：`preparePCIeBringupParity()` 在 ASPM 這關就 return false。**
   QEMU/VFIO 的 PCIe Link Control 可以讀（`0x143`，L0s+L1 開著）但**寫不進去**（readback 一樣 `0x143`），舊碼把它當致命錯誤 → `probeTarget()` 在第 0 步就退出，`Ath10kBMIAttempted=No`、所有 firmware 斷點都是 `not-run`。
   修法：只有讀到 `0xffff`（真的讀不到）才致命，寫不進去就記 `Ath10kASPMControl = write-ignored-continued` 繼續。ASPM-off 只是 reset 視窗的餘裕，不是開機必要條件。修完 firmware 立刻跑起來。
2. **`IONetworkStack::insertNetworkInterface` 的固定位移呼叫會把 en0 搞死。**
   症狀不是 panic、不是掛住：guest 照常跑（serial 一直長、launchd/Keychain 一直在動），但 **en0 從此不能用**（serial 滿是 `interface name en0 is unavailable`、`interface en2 open failed`），sshd 永遠不回 banner。v33/v34/v37 都是這個死法；把那段程式碼整段拿掉（v36 起）SSH 立刻回來、en2 也照樣出現。**這條路徹底封閉，不要再試任何位移。**
3. **BSD 單元其實已經長得出來，卡在「介面型別」。**
   `IONetworkInterface::initIfnetParams()` 是 Apple 的實作：它對 controller 取型別、`cmp eax, 6`，**不等於 6（IFT_ETHER）就直接失敗**，所以 unit 只能在意義上「長得像 Ethernet」時建立（v36/v37 的 en2 就是這樣來的）。反過來，把型別在 attach 前設成 71 會讓 initIfnetParams 失敗 → **連 en2 都不見**（v38 實測）。
   現在做法：attach 前保持 6 讓 ifnet 建起來，attach 成功後在**真正拿到 unit 的那個物件**上把 `IOInterfaceType` 蓋成 71（`Ath10kVenturaInterface::attachToDataLinkLayer` 覆寫）。v40 ioreg 確認 en2 這筆是 71。
4. **`airportd` 對我們完全沉默是可量測的。** `apple80211Request` 現在包一層計數器（`Ath10kApple80211Requests` / `Ath10kLastApple80211Request|Verb|Status`），v40 讀到的是**屬性根本不存在 = 0 次呼叫**。所以「Wi-Fi 打不開」不是我們拒絕請求，是請求從沒到。
5. **`attachInterface(&iface,true)` 會自己再 `createInterface()` 一次**，這就是為什麼 ioreg 有**兩個** `Ath10kVenturaInterface`：我們 start 內自建的那個（`+0x200` ivar）和 Apple 在 attach 流程裡再造、最後拿到 en2 的那個。蓋屬性要蓋在後者上。

## 死了的路（全部實測過，不要重試）

1. **`super::start(provider)`（= `IO80211Controller::start`）→ 必定 panic**：`"No ivars->_faultReporter" @IO80211Controller.cpp:3604`，任何 override 都救不了。
2. **`createWorkLoop()` 回 `IO80211WorkLoop::workLoop()` → 整支 kext 不被注入**（`kmutil showloaded` 一條都沒有）。
3. **靠 vtable slot 硬塞 fault reporter/getLogPipes** → 偏移修正後仍 panic。
4. **`attachVirtualInterface(&vif, mac, 7, true)` / `createVirtualInterface(mac, 7)` → 都失敗**（`vif-create-failed`）；MAC 有給、兩個 request slot 也接過，沒用。
5. **`IONetworkStack` 的 `vptr+0x340`** 是 `IORegistryEntry::isParent`，不是插入函式。
6. **用固定位移硬呼叫 `IONetworkStack::insertNetworkInterface`（vptr−0xac94）→ 弄壞整個網路堆疊**（en0 死、sshd 不回 banner、無 panic），見上文根因 2。
7. **attach 前 `setInterfaceType(71)`** → `initIfnetParams` 失敗 → 沒有 BSD 單元（v38）。
8. **把 `IOInterfaceType` 蓋在 start 內自建的那個介面上** → 蓋錯物件，en2 那筆還是 6（v39 實測）。

## 下一個接手的人：照這個順序做

1. **讓 SystemConfiguration 真的把 en2 當 Wi-Fi 服務。** v40 的 ioreg 已經是 `IOInterfaceType=71`，但 `networksetup -listnetworkserviceorder` 還是舊的 `Ethernet 2`（SC 把服務存在 `/Library/Preferences/SystemConfiguration/preferences.plist`，會沿用舊分類）。
   實測失敗的一招（別重試）：`sudo networksetup -removenetworkservice "Ethernet 2"` → **`You cannot remove Ethernet 2 because there aren't any other network services on Ethernet.`**（它是該類別唯一的服務，所以砍不掉）。
   要做的是讓 SC 重新分類：直接動 `/Library/Preferences/SystemConfiguration/preferences.plist` 裡 en2 那筆（`NetworkInterfaces` 的 `Type`／或整筆移除後重開機），或先重開機確認新 boot 的 en2 一出現就是 71（SC 在介面首次出現時才寫入分類，之後就沿用快取）。
2. **確認 airportd 的請求會進來。** 服務變 Wi-Fi 之後再看 `Ath10kApple80211Requests`；>0 就代表 `apple80211Request` 通道打通，接著要一個一個補真的實作。
3. **把 stubbed 的 80211 動作接到真正的 WMI。** 目前 `venturaScan()` 只清 index、`venturaAssociate()` 直接回 `kIOReturnUnsupported`、`venturaScanResult()` 回 `NotFound`——**掃描/連線核心邏輯完全沒接**（firmware 已經 ready，WMI 掃描通道 `_wmiScan` 在 Sequoia 分支有實作可直接搬）。
4. **實機驗證。** 以上在 VM 通了之後才把 kext 部署回實體 ESP 並在真機重驗（ASPM 在真機是可控的，`Ath10kASPMControl` 應該不會出現 `write-ignored-continued`）。

驗收條件（依序，未變）：

- A：`Ath10kIfacePhase = "inited"` 且 `Ath10kStartPhase = "interface-published"`。**已達成**
- B：`ioreg -r -c IO80211Interface` ≥ 1 筆（已達成，且有 `BSD Name=en2`）且 `networksetup -listallhardwareports` 出現 Wi-Fi 介面（已達成）。**服務仍被歸類成 Ethernet → 未完成**
- C：「系統設定 → Wi-Fi」可見且開關可操作（截圖為證）。**未達成**
- D：可掃到 AP 並以 WPA2 連線、取得 IP。**未達成**（掃描/關聯都還是 stub）
- E：全程不修改系統設定以外的東西、不需終端機、無獨立 app；最後把 kext 部署回實體 ESP 並在真機重驗。**未開始**

## 檔案地圖

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | `probe()`/`start()`：Ventura 分支建 work loop、備 corecapture、publish `IOPacketFilters`、建 output queue、手動 create/configure/attach/register/attachToDataLinkLayer、attach 後蓋 `IOInterfaceType=71`、`preparePCIeBringupParity()`（ASPM 非致命）、vif 嘗試；所有 `Ath10k*Phase` 斷點 |
| `src/kext/ventura/Ath10kVentura.cpp` | `apple80211Request`（計數器 wrapper）→ `venturaDispatchRequest`、`getLogPipes()`、`alignVenturaVtable()` + slot thunk、`createInterface()`、`venturaPower/Scan/Associate…`（**掃描/關聯仍是 stub**） |
| `src/kext/ventura/Ath10kVenturaInterface.cpp/.hpp` | `Ath10kVenturaInterface : IO80211Interface`；`init()` 用 mangled `ath10kNetworkInterfaceInit` + `ath10kEthernetInterfaceInit`，並在 attach 前把型別釘成 IFT_ETHER；`attachToDataLinkLayer()` 覆寫在成功後蓋 `IOInterfaceType=71`；`inputPacket()` 把 EAPOL 交 IO80211 supplicant |
| `src/kext/ventura/Ath10kVenturaDecl.inc` | Ventura 專屬宣告：Skywalk enable/disable、16 reserved slot、`getLogPipes`、vtable thunk、請求計數器成員 |
| `src/kext/Ath10kNetwork.cpp` | `configureInterface()`、`createWorkLoop()`、`createOutputQueue()`、`getHardwareAddress()` |
| `src/kext/native/*` | Sequoia（v2）對照實作：WMI 掃描/關聯、`getFaultReporterFromDriver()`、`IO80211InfraInterface::init()` 用法 |
| `docs/hardware-verification.md` | 實機 stage0/stage3 陷阱與 hash 比對 |

## 重跑 / 救援（Acer `100.64.0.152`）

```bash
# 0) 縮短 ssh 進 guest 的字串（在 Acer 上建一次）
#    /tmp/g.sh = exec ssh -o StrictHostKeyChecking=no -p 2223 user@127.0.0.1 "$@"

# 1) 同步原始碼並在 guest 內編譯
cd ~/Data/Dev/qca6174-native && tar czf /tmp/qca-src.tgz --exclude=build --exclude=.git --exclude=artifacts-ci* .
scp -q /tmp/qca-src.tgz 100.64.0.152:/tmp/
ssh 100.64.0.152 'scp -q -P 2223 /tmp/qca-src.tgz user@127.0.0.1:/tmp/ && /tmp/g.sh \
  "tar xzf /tmp/qca-src.tgz -C ~/qca && cd ~/qca && make VENTURA=1 -j2 kext 2>&1 | tail -3"'

# 2) 部署 + 開機（/tmp/deploy_v40.sh 為最新樣板，S() 全程包 sudo；編號自增請整份改寫不要 sed）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_vNN.sh'

# 3) 讀斷點（ioreg 快、serial 更早看到東西）
ssh 100.64.0.152 '/tmp/g.sh "ioreg -l -w0 | grep -E \"Ath10k(StartPhase|IfacePhase|InterfaceType|RecoveryPhase|Apple80211Requests)\"; ifconfig -l"'
ssh 100.64.0.152 'tr -d "\000" < /tmp/ventura-vm.serial | grep -iE "ath10k|en2" | tail -20'

# 4) 截圖（驗 C 用）
ssh 100.64.0.152 'printf "screendump /tmp/shot.ppm\n" | socat - UNIX-CONNECT:/tmp/ventura-vm.mon'
# 再 python3 -c "from PIL import Image; Image.open('/tmp/shot.ppm').save('/tmp/shot.png')"
```

**救援（VM panic 循環或網路堆疊被弄壞、不需要 guest）**：

```bash
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/install_bundle.sh /tmp/vk40'   # 裝回 v40（radio up + en2 + SSH 正常）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/install_bundle.sh /tmp/vk20'   # 最保險的舊穩定版（介面已發布、無 stack hack）
```

`install_bundle.sh <dir>` 直接從 Acer 的 `/tmp/vkNN` 取 bundle 寫進 ESP 再開機，完全不依賴 guest SSH。目前 Acer 上留著 vk3…vk40（v36 起 SSH 正常；v33/v34/v37 會弄死 en0；v38 沒有 BSD 單元）。

`/tmp/disable_boot.sh` 取得可登入但不載入 kext 的 guest。

## 分析工具（可直接重用）

- `/tmp/kc2`：Ubuntu 上的 `BootKernelExtensions.kc` 副本（69MB），不用再從 guest 抓。
- `/tmp/an.py` + venv `/tmp/cs`（`capstone 5.0.7`）：
  - `list <子字串>`：找符號；`vtable <位址> <entry數>`：解 vtable（內建 chained fixup 解碼）；`dis <符號或位址> <bytes>`：反組譯並標註 call 目標；`str`/`ptr`：讀字串/指標。
- 本輪用它確認的兩件事：**`IOEthernetInterface::initIfnetParams` 內有 `cmp eax, 6` 的硬性檢查**（這就是型別必須先 6 的原因）；**`IO80211Interface::init` 會依序用 `apple80211RequestIoctl(0xc03069c9, 0xc/0xe/0x50, this, …)` 問能力**（能力查詢不走 `apple80211Request`）。
- `/tmp/vt_both.txt`：兩份 `IO80211Controller` vtable 對照（vtable offset / obj offset / 符號）。
- **解析 KC 的地雷**：抽單一 kext 的 Mach-O 後不能解析它的 `LC_SYMTAB`（offset 是整個 KC 的絕對位置）；`nm` 對 kext 內 local 符號標 `ext=False`，這種不能直接引用（會讓 OpenCore 跳過 kext）。

## 其他已知陷阱（沿用）

- **不要在 VM panic 循環時 `scp` 進 guest** → 用 `install_bundle.sh`。
- **不要**用 `pgrep -q`（此 Ubuntu 不支援）、**不要** `pkill -f qemu-nbd` → 用 `systemctl --user stop qca-vm`。
- **不要**用 `ioreg | grep 0x168c` 判斷網卡（byte-swapped）→ 用 `system_profiler SPPCIDataType`。
- **qemu-nbd**：VM 開著會 `Failed to get write lock`；掛載後等 `/dev/nbd0p1` 最多 15 秒。
- **多層 ssh 的引號地獄**：`ssh Acer 'ssh guest "cmd | grep \"x\""'` 會爆；一律把腳本 `scp` 到 guest 再 `/tmp/g.sh "sh /tmp/x.sh"`，需要 sudo 的腳本用 `read -r PW` + `cat ~/sudo.pwd |` 餵。
- **sshd banner timeout ≠ 掛住**：先看 serial 有沒有在長。serial 在長 + banner 不回 = 網路堆疊被弄壞（本輪的 stack insert）或系統真的睡了；`system_wakeup` 若回「not in suspended state」代表不是睡眠問題。
- **git push**：若出現 TLS 卡死，先檢查 `http.https://github.com.proxy` 設定有沒有被塞 socks5。
- **`gem install`/pip 不要動系統 python**：capstone 裝在 `/tmp/cs` venv。

## 建議技能

- `osx-kvm-headless-install`：VM 生命週期、qemu-nbd、SSH 進 guest。
- `macos-kext-hardware-bringup`：kext 建置/注入、OpenCore 設定、IO80211 ABI 陷阱（本輪新增：initIfnetParams 的 IFT_ETHER 硬檢查、IOInterfaceType 決定 SC 服務分類、stack insert 會弄壞網路堆疊）。
- `handoff`：下次再交接時沿用本文件結構。
