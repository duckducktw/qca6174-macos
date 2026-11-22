# QCA6174 Ventura 原生 Wi-Fi — 交接文檔

最後更新：2026-11-22（第四輪：vif 想借 ifnet 卻一個來源都拿不到 → ifnet KPI 在本 kext 失效、介面不是控制器子節點、vif vtable 位移全部量好；下一步改成自己 `ifnet_allocate`）
分支：`ventura-io80211`　最新 commit：見 `git log -1`
VM 現況：**v62**（`/tmp/vk62`）— kext 載入、單元 en1、Apple start 唯一 publisher、無 panic；v52/v53/vk48 都在

---

## 1. 目標與驗收條件

讓 QCA6174（`168c:003e` / 子系統 `11ad:0807`）在 macOS 13.x Ventura 上以**真正原生**形式運作，**不用相容層、不用獨立 app、不要求使用者開終端機**。驗證由 agent 在 VM 內自行完成（使用者留在 Ubuntu，只接實體網路線）。

| 條件 | 內容 | 現況 |
|---|---|---|
| **A** | `Ath10kIfacePhase = "inited"`、`Ath10kStartPhase = "interface-published"` | ✅ 已達成 |
| **B** | `ioreg -r -c IO80211Interface` ≥1 筆且有 `BSD Name`；`networksetup -listallhardwareports` 出現 Wi-Fi | 🟡 一半（`en2` 存在、有 MAC；但 SC 服務被歸類成 Ethernet） |
| **C** | 「系統設定 → Wi-Fi」可見、開關可操作（截圖為證） | ❌ 未達成 |
| **D** | 可掃描到 AP 並以 WPA2 連線取得 IP | ❌ 未達成（scan/associate 仍是 stub） |
| **E** | 全程不改系統設定以外的東西、不需終端機、無獨立 app；最後部署回實體 ESP 並真機重驗 | ❌ 未開始 |

**一句話總結現況**：無線電已經真的在跑（firmware、WMI、HTC、CE/IRQ 全部 ready），BSD 單元 `en2` 也長得出來，但**沒有任何東西把這個 ifnet 登記進 80211 核心**，所以 SystemConfiguration 只把它當一般網路介面、airportd 完全不碰它。

---

## 2. 目前權威狀態（v53；**SC 相關的舊結論已被 §6.7 推翻**，下表保留 v51 的量測供對照）

**環境**：macOS **13.7.8 / 22H730**；`com.apple.iokit.IO80211Family 1200.13.0`（載入基址 `0xffffff8001f35000`，長度 `0x19bff9`，**live 範圍 `0x1f35000`–`0x20d0ff9`**）＋ **`com.apple.iokit.IO80211FamilyLegacy`（另一個 bundle，見 §6.5）**；`IONetworkingFamily 3.4`；kext `br.com.jonasw8.driver.ath10kmac 0.0.105`；OpenCore 1.0.5 DEBUG（`Target=67`、`ShowPicker=false`、`Timeout=0`）。

| 觀察點 | v51 實測值 | 意義 |
|---|---|---|
| `ifconfig -l` | `lo0 gif0 stf0 en0 en2` | `en2` 是我們的 BSD 單元 |
| `Ath10kAlignPhase` | `abi-native` | header 已是 live B 順序，不再手動補 slot（§6.5） |
| `Ath10kStartPhase` | `interface-published` | 手動發布路徑的結論（會覆蓋 apple-started 的值，見 §6.6） |
| `Ath10kIfacePhase` | `inited` | 介面 init 成功 |
| `Ath10kWorkQueue` | Yes | `IO80211WorkQueue::workQueue()` 已供應（§6.6） |
| `Ath10kFaultSlotPhase` | `reporter` | live 的 `findAndAttachToFaultReporter` 真的呼叫到我們的 slot |
| `Ath10kStackPhase` | `driver-owns-ifnet` | 不再呼叫 `IONetworkStack`（§5.2） |
| `Ath10kASPMControl` | `write-ignored-continued` | QEMU 不吃 ASPM 寫入，非致命（§4.1） |
| `Ath10kRecoveryPhase` | `ready` | firmware runtime 活著 |
| `Ath10kVifPhase` | **`vif-registered`** | `createVirtualInterface(this, addr, 7)` 這次真的成功（role=SoftAP） |
| `Ath10kVifBsdPhase` | **`no-bsd-name`** | vif→`prepareBSDInterface` 交接卡在取名稱（§6.6） |
| `Ath10kApple80211Requests` | **1** | 第一次有請求送達（vif 能力查詢） |
| `ioreg -r -c Ath10kVenturaInterface` | **6 筆**（其中 1 筆 `BSD Name=en2`） | 疊出多個介面物件，手動發布路徑現在是多餘的 |
| `ioreg -r -c IO80211VirtualInterface` | 1 筆 | vif 存在但沒有 BSD 單元 |
| `networksetup -listallhardwareports` | **本輪 Wi-Fi 硬體埠消失**（v48 時還在） | 多介面干擾 SC 的推導 |
| `networksetup -listnetworkserviceorder` | `(3) Ethernet 2 (Hardware Port: Ethernet, Device: en2)` | **卡點：服務型別仍是 Ethernet** |
| `networksetup -getairportpower en2` | `Off (forced)` | airportd 不接管 |
| `tools/ifnet-type`（ifnet 真實型別） | `en2 type=0x47 (71)` | ifnet 型別已是 80211，SC 不吃這個值 |

---

## 3. 不可破壞的規則（每條附理由）

1. **Ventura 建置不得引用 `IO80211FaultReporter`** —— 13.7 沒有這個類別（它是新世代才有的），引用會讓 OpenCore 靜默跳過整個 kext。
2. **`Ath10kPCIDevice` 必須覆寫 `enable/disable(IO80211SkywalkInterface*)` 與 16 個 `_RESERVEDIO80211ControllerN()`** —— 缺任一個 → `Prelinked injection … Invalid Parameter`。
3. **先 `createWorkLoop()` 再跑 corecapture** —— `CCPipe` 建立需要 work loop。
4. **Ventura 分支不要呼叫 `IO80211Controller::start()`** —— 實測必 panic（fault reporter 在驅動能插手前就被讀）；但注意 §6.3，這可能是我們自己的 ABI 沒對齊造成，不是平台限制。
5. **除錯優先用 serial** —— kext 的 `IOLog` 在 `log show`/`dmesg` 看不到，但會出現在 QEMU serial（`/tmp/ventura-vm.serial`）。`Ath10kVenturaInterface: initIfnetParams failed`、`READY Qualcomm Atheros QCA6174`、panic backtrace 都是從這裡讀到的，比 ioreg 快一步。
6. **部署前必須跑 `oc-enable.py`** —— 漏了就會出現「完全沒有斷點」的假象。
7. **vtable 位移：kernel 端 `[vptr+0xNNN]` 對應的就是 vt+0xNNN** —— 不要用某些清單裡的「obj+」欄位（那欄 = vt+0x10），用錯會差一格（§5.3 就是這樣踩的）。
8. **不要碰 `IONetworkStack` 的插入函式** —— 會弄壞整個網路堆疊（§5.2）。

---

## 4. 已解掉的根因（勿重查）

### 4.1 VM 內無線電從來沒開機：ASPM 檢查擋死整個 target probe
QEMU/VFIO 的 PCIe Link Control **可讀**（`0x143`，L0s+L1 開著）但**寫不進去**（readback 不變）。舊碼把「ASPM 關不掉」當致命錯誤 → `preparePCIeBringupParity()` 回 false → `probeTarget()` 第 0 步就退出，`Ath10kBMIAttempted=No`、所有 firmware 斷點都是 `not-run`。
**修法**：只有讀到 `0xffff`（真的讀不到）才致命；寫不進去就記 `Ath10kASPMControl = write-ignored-continued` 繼續。ASPM-off 只是 reset 視窗的餘裕，不是開機必要條件。修完 firmware 立刻跑起來（v35 起）。

### 4.2 `IONetworkStack::insertNetworkInterface` 的固定位移呼叫會弄死 en0
症狀**不是** panic、**不是**掛住：guest 照常跑（serial 一直長、launchd/Keychain 持續活動），但 **en0 從此不能用**，serial 被 `interface name en0 is unavailable`、`interface en2 open failed` 洗版，sshd 永遠不回 banner。v33/v34/v37 都是這個死法。整段拿掉（v36 起）SSH 立刻回來、`en2` 照樣出現。
**這條路徹底封閉，任何位移都不要再試。**

### 4.3 BSD 單元長得出來，但型別被鎖在 IFT_ETHER
`IOEthernetInterface::initIfnetParams()` 是 Apple 實作，反組譯確認有 `cmp eax, 6`：**型別不等於 6（IFT_ETHER）就直接失敗**，所以單元只能在意義上「長得像 Ethernet」時建立。反過來，attach 前把型別設成 71（IFT_IEEE80211）→ initIfnetParams 失敗 → **連 en2 都不見**（v38、v42、v44 三次實測）。
現行做法（v43/v48）：attach 前保持 6 讓 ifnet 建起來；`initIfnetParams()` 覆寫裡呼叫完 base 後把 `params->type` 改成 71（ifnet 真的是 80211，`tools/ifnet-type` 驗證過）；attach 成功後再把 `IOInterfaceType` property 蓋成 71。

### 4.4 airportd 對我們「完全沉默」是可量測的
`apple80211Request` 外面包了一層計數器，會公布 `Ath10kApple80211Requests` / `Ath10kLastApple80211Request` / `…Verb` / `…Status`。v40、v43、v48 讀到的都是**屬性根本不存在 = 0 次呼叫**。所以 Wi-Fi 打不開**不是我們拒絕請求，是請求從來沒到**。

### 4.5 `attachInterface(&iface, true)` 會自己再 `createInterface()` 一次
這就是 ioreg 有**兩個** `Ath10kVenturaInterface` 的原因：我們 start 內自建的那個（存在 controller `+0x200` ivar），以及 Apple 在 attach 流程裡再造、**最後真正拿到 `en2` 的那個**。要蓋屬性必須蓋在後者身上（v39 蓋錯物件就是這個坑）。

---

## 5. 已封閉的路（全部實測，附症狀）

| # | 嘗試 | 結果 / 症狀 | 版本 |
|---|---|---|---|
| 5.1 | `super::start(provider)`（= `IO80211Controller::start`） | 必 panic：`No ivars->_faultReporter @IO80211Controller.cpp:3604`；任何 override 都救不了 | 早期 |
| 5.2 | `createWorkLoop()` 回 `IO80211WorkLoop::workLoop()` | 整支 kext 不被注入（`kmutil showloaded` 一條都沒有） | 早期 |
| 5.3 | 用固定位移呼叫 `IONetworkStack::insertNetworkInterface` | **弄壞整個網路堆疊**（§4.2）；另外 `vptr+0x340` 其實是 `IORegistryEntry::isParent` | v33/v34/v37 |
| 5.4 | `createVirtualInterface(mac, 7)` | 回 null、**serial 完全沒有 IOLog**（長期查不出原因，§6.2 才找到根因） | 全程 |
| 5.5 | `attachVirtualInterface(&vif, mac, 7, true)`（聚合版） | panic：frame 在 `Ath10kPCIDevice::start + 0x7f7`，跳進 `detachVirtualInterface+0xe` | v47 |
| 5.6 | 改走自己物件的 vptr 呼叫 vif 建立 | panic：OpenCore 重建的 table 不是 family 那張；`vptr+0xdb8` → `detachVirtualInterface`（general protection）；`vptr+0xda8` 也 panic | v45、v46 |
| 5.7 | attach 前 `setInterfaceType(71)` | `initIfnetParams` 失敗 → **沒有 BSD 單元**（`ifconfig -l` 只剩 en0） | v38 |
| 5.8 | 在 `init()`／`createInterface()` 提前蓋 `IOInterfaceType=71` | 同上，單元消失（family 的 attach 路徑自己會讀這個 property） | v42、v44 |
| 5.9 | 把 71 蓋在 start 內自建的介面上 | 蓋錯物件，en2 那筆仍是 6 | v39 |
| 5.10 | `new IO80211VirtualInterface`（AirportItlwm 的寫法） | **整個 kext 被靜默拒絕**：`kmutil showloaded` 無、serial 無 ath10k 一行（連 probe 都沒跑）。符號對 KC 匯出表是齊的，但 vtable/ctor 那組過不了 `-nostdlib -static -kext` 的注入檢查 | v49 |
| 5.11 | `OSMetaClass::getMetaClassWithName` + `alloc()`（只走 virtual） | **VM 卡在開機**：serial 停在 OpenCore 載完驅動後、**一行 kernel 訊息都沒有**，SSH banner 永不回、無 panic | v50 |
| 5.12 | 手動修 SystemConfiguration | 見 §5.1（分類相關） | — |

### 5.1 手動重分類 SystemConfiguration（全失敗）
- 改 `preferences.plist` 把 en2 服務的 `Type/Hardware` 設成 `IEEE80211` → 重開機被 SC 用原值覆蓋。
- `networksetup -removenetworkservice "Ethernet 2"` → 拒絕：`You cannot remove Ethernet 2 because there aren't any other network services on Ethernet.`
- `networksetup -createnetworkservice "Wi-Fi" en2` → `en2 is not a valid hardware port name.`
- 刪掉 `NetworkInterfaces.plist` 的 en2 記錄 + `preferences.plist` 的服務 + ServiceOrder，再重置 VM → SC 照樣重新推導成 Ethernet（試三次）。
- `launchctl kickstart -k system/com.apple.configd` → `Operation not permitted`（SIP）。
- 刪完服務後 `networksetup -listallhardwareports` 連 en2 都不列了。

### 5.2 SC 分類機制的量測結論（重要）
- 介面分類記錄在 `/Library/Preferences/SystemConfiguration/NetworkInterfaces.plist`：v43 開機時 en2 那筆是 `SCNetworkInterfaceType = IEEE80211`、`UserDefinedName = Wi-Fi`，**但 `IOInterfaceType = 6`**；服務記錄（`preferences.plist` 的 `NetworkServices`）因此建成 `Type/Hardware = Ethernet`。
- 這個值是在介面**第一次被 SC 看到**時（`registerService()`）讀的，不是 attach 時。所以「attach 後才蓋 71」一定留下 6 的記錄。
- 把 71 提前 → family 的 attach 路徑拒絕 → 單元消失（§4.3）。
- **結論：不要再試「把 Ethernet 形狀的介面硬掰成 80211」。**

---

## 6. 社群怎麼解的（AirportItlwm，本輪實查原始碼）

Ventura 可用的版本是 **v2.2.0**（`AirportItlwm/` 目錄）；master 的 `AirportItlwmV2.cpp` 是 Sonoma+ 的新路線。

### 6.1 Ventura 版（v2.2.0）的四個關鍵做法
1. `AirportItlwm::start()` 第一行就是 `super::start(provider)`（`IO80211Controller::start`），沒有繞。
2. `createWorkLoop()` 回 `IO80211WorkLoop::workLoop()`。
3. `configureInterface()`：`super::configureInterface(netif)` 之後呼叫 **`ether_ifattach(ifp, netif)`**，其中 `ifp` 是它**自己** net80211（`itl80211`，隨 kext 一起編）的 `ic_ac.ac_if` —— 它有自己的 ifnet，且 `ether_ifattach` 是**自己 bundle 內的符號**（不依賴 kernel 匯出）。
4. `AirportItlwmInterface::init()` 只有一行 `super::init(controller)`；`useAppleRSNSupplicant()` 預設 **false**（社群註記：Ventura 開 Apple RSN 會 panic）。

### 6.2 Sonoma+ 版（更像我們的形狀：`IOEthernetInterface` 拿 ifnet）
`AirportItlwmEthernetInterface::attachToDataLinkLayer()` 在 `super::` 成功後多做：

```cpp
interface->setProperty("built-in", ...);
interface->setProperty("IOInterfaceName", "enN");
interface->setProperty(kIOInterfaceUnit, ifnet_unit(getIfnet()));
interface->setProperty(kIOInterfaceNamePrefix, ifnet_name(getIfnet()));
setProperty(kIOMACAddress, &addr, kIOEthernetAddressSize);
interface->registerService();
interface->prepareBSDInterface(getIfnet(), 0);   // ← 關鍵
```

**`prepareBSDInterface(ifnet, 0)` 就是把 ifnet 交給 80211 核心的那一步。** KC 實查：它只存在於新世代類別 —— `IO80211SkywalkInterface`（live `0x2000ccc`）、`IO80211VirtualInterface`（`0x20173a6`）、`IO80211InfraInterface`（`0x203f9a0`）；**舊的 `IO80211Interface` 沒有這個 virtual**。

→ 這解釋了我們所有症狀：單元建得出來，但沒有任何東西把它登記進 80211 核心。不是 SC 分類難搞，是**我們用的發布類別在 13.7 已經沒有登記鉤子**。

### 6.3 我們照抄後失敗的兩次（§5.10 / §5.11 的補充）
- `new IO80211VirtualInterface`：連 kext 都載不進去。
- metaclass alloc：卡開機、無任何 kernel 輸出。
- **關鍵矛盾**：AirportItlwm 的起手式（`super::start()` + `IO80211WorkLoop`）正是我們量到「必 panic／不被注入」的兩件事。這強烈暗示**我們的 kext 與 KC 的符號/ABI 綁定本身還沒對齊**，也就是每次「靜默被跳過」的根源。這一點沒解決之前，任何照抄社群的 vif 寫法都只是換一種死法。

### 6.4 附帶發現：KC 裡有兩份 `IO80211Controller`，而且都匯出同名符號
symtab 實查：`createVirtualInterface`、`attachVirtualInterface`、`IO80211VirtualInterface::gMetaClass`、`IO80211VirtualInterface::init` 等**各有兩組位址，全部 `N_SECT|N_EXT`**（一份在 live 範圍 `0x1fe…`，一份在 `0x218…`）。用名字呼叫可能綁到不是 live 的那份 → alloc 回 null、靜默失敗。
另外 live 與非 live 兩份的方法集也不同（例：`attachIfnet` 只在非 live 那份、`prepareBSDInterface` 只在 live 那份）。

---

## 6.5 根因（2026-11-22 離線比對 KC 得出，已可重現驗證）

**13.7.8 的 KC 裡有兩個不同的 `IO80211Controller` 類別，分屬兩個 bundle：**

| 表 | 位址 | bundle | 佈局 |
|---|---|---|---|
| A | `0xffffff8002240b60` | `com.apple.iokit.IO80211FamilyLegacy` | 舊 v1：`apple80211Request(ji P16IO80211Interface Pv)`、`getLogPipes` @ vt+0xd48、無 `getFaultReporterFromDriver` |
| B | `0xffffff80020d86f8` | `com.apple.iokit.IO80211Family`（**live，載入範圍 0x1f35000–0x20d0ff9**） | Skywalk 期：`apple80211VirtualRequest`/`apple80211SkywalkRequest`、`getLogPipes` @ vt+0xd20、`getFaultReporterFromDriver` @ vt+0xd58 |

**實測方法（零開機、可重複）**：
1. `otool -rv <kext>` 取 relocation（`addr ... UNSIGND ... <symbol>`）與 `otool -s __DATA __const` 取原始 8-byte 字，重組出 `__ZTV15Ath10kPCIDevice` 的完整 slot 序列（v48 binary：vtable @ `0x1e5940`）。
2. 用 KC symtab 解析 `__ZTV17IO80211Controller` 兩份的 slot 序列（本輪腳本見 §11）。
3. 逐 slot 比對：**v48 與表 A 吻合 391 個、只有 7 個不符（全是 reserved pad）；與表 B 有 40 個不符。**

**結論**：`vendor/itlwm-v1/Airport/IO80211Controller.h` 描述的是 **Legacy** 的佈局，我們的 vtable 因此是 A 形狀；但實際在跑、會呼叫我們的是 **B（live）**。這解釋了所有未解症狀：

- `super::start()` 呼叫 `[vptr+0xc58]` 想拿 `getWorkQueue()`（B 的 vt+0xc68），我們的 0xc58 是 `_RESERVEDIOEthernetController…` → 回 null → `IO80211CommandGate::allocWithParams(null)` → **start 回 false**（不 panic，所以先前誤判為「Apple 的路走不通」）。實測 `Ath10kFaultSlotPhase=reporter` 證明 fault reporter 這關已經過了。
- 舊的 `alignVenturaVtable()` 補丁（0xd10/0xd48/0xca8/0xcb0）其實位置本來就對（B 的 `getLogPipes`/`getFaultReporterFromDriver`/兩個 request）——只是物件其餘 450 個 slot 還是 A 形狀，補四個洞救不了。
- `apple80211Request` 在 live 根本沒有這個 slot（airportd 走 `apple80211VirtualRequest`/`apple80211SkywalkRequest`）→ `Ath10kApple80211Requests` 永遠 0。
- HANDOFF §6.4 的「兩份同名符號」不是複製貼上，是兩個 bundle；`attachIfnet` 只在 Legacy，`prepareBSDInterface` 只在 live。

**正解方向**：把 Ventura 分支的 `IO80211Controller` 宣告改成 **B 的順序**（`0xc60` 起：`createWorkQueue`、`getWorkQueue`、`requestPacketTx`、`getIO80211CommandGate`、`getHardwareAddressForInterface(IOEthernetAddress*)`、`useAppleRSNSupplicant(IO80211VirtualInterface*)`、…、`getLogPipes`、…、`getFaultReporterFromDriver`、… 、`createVirtualInterface` 0xda8、`attachVirtualInterface` 0xdb0、`enable/disable(IO80211SkywalkInterface*)` 0xdc0/0xdc8、RESERVED 0–15），overrides 一起改簽名；介面端（`IO80211Interface`）也要用同一份 live 表比對。驗證方式：重編後照上面 1–3 步比對 vtable，**順序對了才開機**。

---

## 6.6 ABI 修正後的第一輪實測（v51，2026-11-22）

改了什麼：
1. `vendor/itlwm-v1/Airport/IO80211Controller.h` 的類別尾段（`0xc60` 起）整段改寫成 **live B 順序**，並補 `IO80211WorkQueue`/`IO80211CommandGate` forward decl。
2. overrides 依新簽名重排：`apple80211VirtualRequest`/`apple80211SkywalkRequest` 取代 `apple80211Request`；`hardwareOutputQueueDepth()`、`performCountryCodeOperation(IO80211CountryCodeOp)`、`monitorModeSetEnabled(bool,UInt)`、`dataLinkLayerAttachComplete()`、`useAppleRSNSupplicant(IO80211VirtualInterface*)`；移除 `stopDMA`；`alignVenturaVtable()` 不再寫 slot，只回報（`Ath10kAlignPhase=abi-native`）。
3. 補 `createWorkQueue()`/`getWorkQueue()` override（`IO80211WorkQueue::workQueue()`，live 有 EXT 匯出）。

離線驗證（不開機）：新 binary 的 `__ZTV15Ath10kPCIDevice` 與 live B 逐 slot 比對，**繼承 slot 385 吻合、13 個差異全部可解釋**（同位置不同類別實作 7 個、`IONetworkController` reserved pad 版本差 6 個）。關鍵 slot 全對：`0xc60 createWorkQueue`、`0xc68 getWorkQueue`、`0xcb8/0xcc0` = 我們的兩個 request、`0xd20 getLogPipes` = 我們的、`0xd58 getFaultReporterFromDriver` = 我們的、`0xda8/0xdb0/0xdb8` = live vif 三兄弟、`0xdc0/0xdc8` = 我們的 enable/disable。

實測結果：
- 第一次開機 panic：`apple80211VirtualRequestIoctl + 0x26`（page fault）。反組譯：`call [vptr+0xc58]` = `getWorkQueue()` 回 null → `mov rcx,[rax]` 撞 null。frame 是 `Ath10kPCIDevice::start + 0x787 → IO80211Controller::createVirtualInterface → IO80211VirtualInterface::init`，**證明 Apple 的 `start()` 已經跑到自己建 vif 這步**。
- 補 `createWorkQueue/getWorkQueue` 後（v51 重編）：不 panic、`Ath10kWorkQueue=Yes`、`Ath10kFaultSlotPhase=reporter`、**`Ath10kApple80211Requests = 1`（專案史上第一次非零）**、`ioreg -r -c IO80211VirtualInterface` **1 筆**（role=SoftAP，無 BSD Name）。

仍卡：SystemConfiguration 還是把 en2 記成 `IOInterfaceType = 6` / 服務 `Ethernet 2`、`getairportpower en2` = `Off (forced)`。量測：
- 介面物件 property `IOInterfaceType = 71`、`tools/ifnet-type` 讀到 `en2 type=0x47 (71)` —— 兩個值都對。
- 刪掉 `NetworkInterfaces.plist` 的 en2 記錄＋`preferences.plist` 的該服務後重開，SC **重新推導仍然寫 6**。
→ **結論：SC 的分類不吃 `IOInterfaceType` property，也不吃 `ifi_type`。** 它八成看的是「這個介面有沒有掛進 IO80211 plane / 是不是 80211 core 發布的介面」。所以下一步不是繼續掰 SC，而是走 vif 發布路線：把 ifnet 交給 `IO80211VirtualInterface::prepareBSDInterface(ifnet, 0)`，照 §6.2 補 `built-in` / `IOInterfaceName` / `IOInterfaceUnit` / `IOInterfaceNamePrefix` / `IOMACAddress` 再 `registerService()`，讓 80211 core 自己發布 BSD 單元（那時 SC 記的就會是 Wi-Fi）。

---

## 6.7 第三輪實測（v52/v53，2026-11-22）：SC 不是分類錯，是**完全看不到介面**

**程式改動**
1. §7.1 落實：手動 create→configure→attach→attachToDataLinkLayer→蓋 71→register 那段現在只在 `!appleStart` 時才會跑（單一 publisher）。新增 `Ath10kPublisher`（`apple`/`manual`）與 `Ath10kAppleStart`（`ok`/`failed`/`skipped-no-reporter`）斷點。
2. `publishVenturaVifBsdUnit()` 改成**先拿 ifnet 再取名稱**：優先 `_venturaInterface->getIfnet()`（`IONetworkInterface::getIfnet()` 是 public virtual），其次才用 `IOInterfaceUnit`/`IOInterfaceNamePrefix` 組合名稱、最後才掃 `"BSD Name"`。新增 `Ath10kVifBsdSource`（`interface-ifnet`/`bsd-name`/`none`）與 `Ath10kVifBsdName`。
3. 介面 `init()` 加 `setProperty("IOPrimaryInterface", true)` 實驗（**無效**，attach 階段被 Apple 覆寫回 No）。

**實測（v52 = 只做 1+2，v53 = 再加 3）**

| 觀察 | 值 | 意義 |
|---|---|---|
| `Ath10kAppleStart` | **`ok`** | Apple 的 `IO80211Controller::start()` 真的跑完（不是 fallback） |
| `Ath10kPublisher` | `apple` | 手動發布路徑**沒跑**，`ifconfig` 仍有單元 → 是 Apple 的 start 發布的 |
| `Ath10kStartPhase` | `controller-started` | 手動段不再蓋掉 |
| `Ath10kVifPhase` | `vif-registered` | vif 仍建得起來 |
| `Ath10kVifBsdSource` / `Phase` | `none` / `no-ifnet` | `_venturaInterface->getIfnet()` 回 **null**：`createInterface()` 交給我們的物件不是拿到 BSD 單元的那顆（Apple attach 會另建） |

**SC 決定性量測（本輪最重要的發現）**
```bash
# 一隻 20 行的 SC 程式（本輪新增，見 §11 的 sclist.c）
SCNetworkInterfaceCopyAll() -> 2
[0] bsd=serial1 type=Modem
[1] bsd=en0     type=Ethernet
```
- **`en2`/`en1` 根本不在 `SCNetworkInterfaceCopyAll()` 裡** → SystemConfiguration 從來沒看到這個介面，所以「SC 一直寫 6」其實是**舊快取記錄**（早期版本留下的），不是它對 71 的判讀。
- 把 `NetworkInterfaces.plist` 的 en2 記錄＋`preferences.plist` 的服務刪掉後重開：SC **完全沒有重建記錄**，`listallhardwareports` 只剩 serial1/en0、`listnetworkserviceorder` 沒有它、`ipconfig getiflist` 也只有 `en0`（`ipconfig getsummary en1` = `interface doesn't exist`）。
- 因為記錄被刪，單元從 en2 變 **en1**（單位號由 `IONetworkStack::getNextAvailableUnitNumber` 配，跟著 SC 記錄走）。en1 的節點：`IOInterfaceType=71`、`IOInterfaceState=3`、`IOBuiltin=Yes`、`IOPrimaryInterface=No`、`BSD Name=en1`，而且掛在我們的控制器底下（`…/QCA6174/en1`）——**一切看起來正常，SC 就是看不到**。
- 手動把 en1 記錄塞回 `NetworkInterfaces.plist`（`SCNetworkInterfaceType=IEEE80211`、`IOInterfaceType=71`）：記錄**存活**（SC 沒覆寫），但仍**沒有 hardware port、沒有服務**。→ 記錄可以寫，服務推導要靠「活著的介面被 SC 認出來」。
- `sudo ifconfig bridge0 create` 後 `bridge0` 也不在 `SCNetworkInterfaceCopyAll()` 裡（bridge 類本來就不是 hardware port），這條只證明 SC 的清單不是單純列 ifnet。

**KC 反組譯：介面要怎麼才「被認出來」**
- `IONetworkStack` 只有 **1 個 instance**（跟著第一個介面 en0 長出來），configd 對它開 `IONetworkStackUserClient`（ioreg 可見 `IOUserClientCreator = "pid 86, configd"`）。**configd/SC 的介面清單就是走這條**。
- `IONetworkStack::interfacePublished(void*, IOService*, IONotifier*)` 是它的 IONotifier callback：把發佈的 service `safeMetaCast` 成 `IONetworkInterface`，讀介面物件 `+0xd8`（state）與 `+0xe0`（**noattach** 旗標）後呼叫（kprintf 格式 `interfacePublished(%p, state 0x%x, parent %d, noattach %d)`），接著走 `registerNetworkInterface` → `reserveInterfaceUnitNumber` → `attachNetworkInterfaceToBSD`。**`noattach` 為真時 stack 就不做 BSD attach**，那條路徑等於 self-managed datapath。
- 另外：`IO80211Controller::createVirtualInterface` 的 live 實作**只接受 role == 7**（`cmp edx,7; jne fail`），內部 `gMetaClass`→`alloc`→`init(controller, addr, 7, …)`（vt+0xdd0）。也就是說我們現在這顆 vif 天生是 SoftAP/P2P 類，**不是**主介面，別把它當 Wi-Fi 主體。
- live `IO80211VirtualInterface` 有完整鏈：`setUnitNumber(const char*)`（用 `IO80211VirtualInterfaceNamer` 配號，寫進 ivars）、`initBSDInterfaceParameters(ifnet_init_eparams*, sockaddr_dl**)`（只補旗標/角色檢查；**要求 `version==2`、`len==0x158`**，且 `getIfnet()==0`）、`prepareBSDInterface`、`configureIfnet()`（設 addrlen/hdrlen/flags/baudrate）、`attachToBpf()`（先送 `apple80211VirtualRequestIoctl(0xc03069c9,12,…)` 再 `IOSkywalkNetworkInterface::bpfAttach(0xc,0x48)`）。
- **真正把 ifnet 塞進 vif 的是 `IOSkywalkNetworkInterface::prepareBSDInterface(ifnet, unit)`**（`ivars=[this+0xc0]; [ivars+8]=ifnet`，然後用 `[ivars]` 的 mtu 呼叫 `ifnet_set_mtu`）。`IO80211SkywalkInterface::prepareBSDInterface` 只是薄薄一層（先 `getIfnet()` 檢查必須為 null、`setProperty("IOInterfaceName", getBSDName())`、`ifnet_set_mtu(1500)`、`createIOReporters`、`ifnet_set_eflags(0x200000,0x200000)`），**不會自己 attach**。
→ 所以 vif 路線必須自己生 ifnet（`ifnet_allocate`/`ifnet_attach`，public），再用 `setUnitNumber`＋`initBSDInterfaceParameters`＋`prepareBSDInterface`＋`configureIfnet`＋`attachToBpf` 收尾。

**下一步（§7.2 改寫）**：vif 自己生 ifnet（type 從出生就是 `IFT_IEEE80211`），交給 `prepareBSDInterface`，然後看 `SCNetworkInterfaceCopyAll()` 是否出現該單元。判斷依據只要這一支程式，不用再猜 SC 的分類規則。

---

## 6.8 第四輪（v54–v62）：vif 借 ifnet 這條路的**五個量測結論**（本輪無進展，但把死路全部標記完）

這一輪照 §7.2 動手：讓 vif 拿到 enN 的 ifnet，再走 `prepareBSDInterface` → `configureIfnet` → `attachToBpf`。結果**一個來源都拿不到 ifnet**，而每一次失敗都排除掉一條路：

| # | 取得 ifnet 的方式 | 實測 | 結論 |
|---|---|---|---|
| 1 | `_venturaInterface->getIfnet()` | null | 我們手上的介面物件不是拿到單元的那顆（Apple attach 另建） |
| 2 | 控制器 ivar `+0x200`（`attachInterface` 停車位） | null | Apple 的 start 沒有把介面停在這裡 |
| 3 | 掃控制器的 registry 子節點找 `"BSD Name"` | 6 個子節點全是 `CCLogPipe`×4 / `CCDataPipe` / `IO80211VirtualInterface` | **介面不是控制器的子節點**（起始時不是，3 秒後也不是） |
| 4 | `ifnet_find_by_name("en0".."en3")` | **全部回 6（ENXIO），連 en0 也一樣** | 這個 KPI 在我們的 kext 裡等於不能用（重複符號/世代問題），任何 name 路徑都死 |
| 5 | `ifnet_list_get(IFNET_FAMILY_ANY, …)` | 沒有 `enN`（N≥1）符合 | 同上，ifnet 清單側也看不到我們的單元 |

**其餘量測（重要，別再重試）**
- `en0` 與 `en1` 底下掛的是**同一個** `IONetworkStack`（id 0x10000019f），所以 §6.7 猜的「stack 必須是介面的 parent」**不成立**（`isParent` 對兩者都為真）。
- IONetworkStack 只有一個 instance，節點屬性是 `IOProviderClass = IOResources`、`IOResourceMatch = "IOBSD"`、`IOUserClientClass = IONetworkStackUserClient`（configd 開的），另外有一個**驅動白名單** `IONetworkMbufDMADrivers = ("com.apple.driver.AppleVmxnet3Ethernet")` —— 只有 en0 的驅動在裡面，這是目前唯一「en0 有、我們沒有」的結構性差異。
- `IONetworkInterface::attachToDataLinkLayer`（live `0x2c638a4`）自己就會建兩種參數結構：`memset(0x90)` = `ifnet_init_params`、`memset(0x158)` = `ifnet_init_eparams`，並寫死 `eparams.version=2, len=0x158`；先 `call [vptr+0x9c0]`（= `initIfnetParams`），再從我們的 params 抄 lladdr/addrlen/callbacks 進 eparams，`options` 的 bit 2/3 決定內部 mode 1 或 2。→ 所以「自己手刻 ifnet_init_eparams」不是必要步驟，這條路徑本來就會做。
- vif 側的 vtable 索引（`vptr = &vtable[2]`，所以 `[vptr+off]` 的 index = `(off+0x10)/8`）：`284 initBSDInterfaceParameters`、`285 prepareBSDInterface`、`286 finalizeBSDInterface`、`287 getBSDInterface`、`288 setBSDName`、`455 setUnitNumber`、`456 attachToBpf`、`457 configureIfnet`。程式裡已用這些位移呼叫（見 `publishVenturaVifBsdUnit`）。
- `setBSDName()` 只是 `strlcpy(ivars+0x49, name, 16)`，不會生 ifnet。

**結論**：vif 這條路線缺的不是「呼叫哪個 virtual」，而是**能用的 ifnet 參照**；在 ifnet KPI 於本 kext 失效的前提下，唯一乾淨的作法是**自己 `ifnet_allocate` 一顆**（`ifnet_init_params` 由我們填、`ifnet_attach`），再交給 `prepareBSDInterface`。下一輪就從這裡開始，不要再去掃 registry 或 name lookup。


---


## 7. 下一個接手的人：照這個順序

### 7.1 重疊的發布路徑（**v52 已完成**）
`super::start()` 現在會跑完（`Ath10kAppleStart=ok`），而且**它自己就發布了 BSD 單元**。所以 `Ath10kPCIDevice.cpp` 的手動 create→configure→attach→蓋 71→register 整段現在只在 `!appleStart` 時才跑，`Ath10kPublisher` 會寫 `apple`。待確認（下一次開機隨手看）：
```bash
ioreg -r -c IO80211Interface | grep -c '+-o'          # 應該只有 Apple 那一顆（+1 顆 vif）
networksetup -listallhardwareports | grep -A2 -i wifi # 目前仍然沒有
```
**注意**：§6.7 已證明 Wi-Fi 硬體埠消失不是「多顆介面」造成的，是 SC 根本看不到這個介面。

### 7.2 把 ifnet 交給 80211 核心（**改寫：必須自己生 ifnet**）
`publishVenturaVifBsdUnit()` 現在會先用 `_venturaInterface->getIfnet()` 拿 ifnet，但實測是 **null**（Apple 的 attach 會另建介面物件，我們手上的不是拿到單元的那顆）。而 `prepareBSDInterface` 依 §6.7 反組譯**只會把傳進去的 ifnet 記進 vif 的 ivar、設 MTU/eflags、建 IOReporters，不會自己 attach**。所以：
1. `ifnet_allocate(&ifnet_init_params, &ifp)`（public KPI；`ifp_type = IFT_IEEE80211` 從出生就對），`ifnet_set_lladdr`、`ifnet_attach(ifp, &sadl)`。
2. `vif->setUnitNumber("en")` → `initBSDInterfaceParameters(&eparams, &sadl)`（**`eparams.version = 2`、`eparams.len = 0x158`**，否則回 `0xe00002c2`）→ `prepareBSDInterface(ifp, 0)` → `configureIfnet()` → `attachToBpf()`。
3. 驗收只看 `SCNetworkInterfaceCopyAll()`（`sclist.c`）有沒有出現這個單元、然後 `networksetup -getairportpower` 是否不再 `Off (forced)`。


### 7.3 再把 80211 動作接到真的 WMI
`venturaScan()` 只清 index、`venturaScanResult()` 回 `NotFound`、`venturaAssociate()` 回 `kIOReturnUnsupported` —— **掃描/關聯核心邏輯完全沒接**。firmware 已 ready，WMI 掃描通道 `_wmiScan` 在 Sequoia 分支（`src/kext/native/`）有現成實作可搬。請求入口已從 `apple80211Request` 改成 live 的 `apple80211VirtualRequest`/`apple80211SkywalkRequest`（都導到 `venturaRecordRequest`），搬邏輯時改這裡。

### 7.4 最後才是真機驗證
VM 通了之後把 kext 部署回實體 ESP，在真機重驗（真機 ASPM 可控，`Ath10kASPMControl` 應該不會出現 `write-ignored-continued`）。

---

## 8. 版本時間軸（Acer `/tmp/vkNN` 上的 bundle）

| 版本 | 改了什麼 | 結果 |
|---|---|---|
| v20 | 舊穩定版 | 介面已發布、無 stack hack、無 radio、SSH 正常（救援用） |
| v33/v34 | 加了 stack insert hack | **en0 死**（§4.2） |
| v35 | ASPM 改為非致命 | **radio 首次起來**（但 stack hack 仍在，en0 仍死） |
| v36 | 移除 stack hack | SSH 回來 + `en2` 出現（型別 6） |
| v37 | 加 apple80211 請求計數器 | 證明 airportd 0 次呼叫 |
| v38 | attach 前 `setInterfaceType(71)` | 單元消失 |
| v39 | start 內蓋 71 | 蓋錯物件 |
| v40 | `attachToDataLinkLayer` 覆寫蓋 71 | en2=71，SC 仍 Ethernet |
| v41 | `initIfnetParams` 覆寫設 `params->type=71` | ifnet=0x47（工具驗證），SC 記錄仍 6 |
| v42 | `init()` 提前蓋 71 | 單元消失 |
| v43 | 重排順序（attach → 蓋 71 → register） | 單元在、ifnet 0x47、SC 記錄 6 |
| v44 | `createInterface` 蓋 71 + 翻轉 | 單元消失 |
| v45 | 自己 vptr dispatch（錯一格） | panic |
| v46 | vptr 改 0xda8 | 仍 panic |
| v47 | 聚合 `attachVirtualInterface` | panic |
| **v48** | 回到 v43 行為 + vif 指標 breadcrumb | **現行可用狀態** |
| v49 | `new IO80211VirtualInterface` | kext 靜默不載入 |
| v50 | metaclass alloc | 卡開機 |
| **v51** | header 尾段改成 **live IO80211Family 順序**、requests 改成 `apple80211VirtualRequest`/`SkywalkRequest`、移除宣告未實作的 override、`alignVenturaVtable` 改純回報 | 第一次：`super::start()` 跑到 `createVirtualInterface`，panic 在 `apple80211VirtualRequestIoctl+0x26`（`getWorkQueue()` 回 null） |
| **v51b** | 補 `createWorkQueue()`/`getWorkQueue()`（`IO80211WorkQueue::workQueue()`） | **不 panic、vif 建起來（role=SoftAP）、`Ath10kApple80211Requests=1`** |
| **v51c** | 加 `publishVenturaVifBsdUnit()`（`prepareBSDInterface(ifnet,0)` + 命名屬性）與名稱查找 | `Ath10kVifBsdPhase=no-bsd-name`；介面節點變 6 顆、SC 的 Wi-Fi 硬體埠消失 → 手動發布路徑要拿掉（§7.1） |
| **v52** | 單一 publisher（手動段只在 `!appleStart` 時跑）＋ `Ath10kPublisher`/`Ath10kAppleStart` ＋ vif 改走 `getIfnet()` 取 ifnet | `Ath10kAppleStart=ok`、`Ath10kPublisher=apple`、單元由 Apple 發布；`Ath10kVifBsdSource=none`（`getIfnet()` 回 null） |
| **v53** | 介面 `init()` 加 `IOPrimaryInterface=true` | 無效（attach 時被覆寫回 No）。同輪量到 **SC 完全看不到介面**（§6.7） |

---

## 9. 檔案地圖

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | `probe()`/`start()`：Ventura 分支建 work loop、備 corecapture、publish `IOPacketFilters`、建 output queue、手動 create→configure→attach→attachToDataLinkLayer→蓋型別→register、`preparePCIeBringupParity()`（ASPM 非致命）、vif 嘗試；所有 `Ath10k*Phase` 斷點 |
| `src/kext/ventura/Ath10kVentura.cpp` | `apple80211VirtualRequest`/`apple80211SkywalkRequest`（→ `venturaRecordRequest` 計數 wrapper）→ `venturaDispatchRequest`、`getLogPipes`（會延遲建 corecapture）、`getFaultReporterFromDriver`、`createWorkQueue`/`getWorkQueue`、`publishVenturaVifBsdUnit`（vif→`prepareBSDInterface`）、`alignVenturaVtable`（純回報）、`createInterface()`、`venturaPower/Scan/Associate…`（**scan/assoc 仍 stub**） |
| `src/kext/ventura/Ath10kVenturaInterface.cpp/.hpp` | `Ath10kVenturaInterface : IO80211Interface`；`init()` 用 mangled `ath10kNetworkInterfaceInit`+`ath10kEthernetInterfaceInit`，attach 前釘 IFT_ETHER；覆寫 `initIfnetParams()`（改 `params->type=71`）與 `attachToDataLinkLayer()`（成功後蓋 `IOInterfaceType=71`）；`inputPacket()` 把 EAPOL 交給 IO80211 supplicant |
| `src/kext/ventura/Ath10kVenturaDecl.inc` | Ventura 專屬宣告：live 順序的 override（兩個 request、`getLogPipes`、`getFaultReporterFromDriver`、`createWorkQueue`/`getWorkQueue`、`hardwareOutputQueueDepth()`、`performCountryCodeOperation(IO80211CountryCodeOp)`）、16 reserved slot、`publishVenturaVifBsdUnit`、請求計數器成員 |
| `vendor/itlwm-v1/Airport/IO80211Controller.h` | **已改寫**：`0xc60` 起整段換成 live `IO80211Family` 的虛擬函式順序（原本描述的是 `IO80211FamilyLegacy`）。改動理由與驗證方式見 §6.5，**不要退回舊順序** |
| `src/kext/Ath10kNetwork.cpp` | `configureInterface()`、`createWorkLoop()`、`createOutputQueue()`、`getHardwareAddress()` |
| `src/kext/native/*` | Sequoia（v2）對照實作：WMI 掃描/關聯、`getFaultReporterFromDriver()`、`IO80211InfraInterface` 用法 |
| `tools/ifnet-type.c` | 在 guest 內編譯即印出 kernel 眼中的 `if_data.ifi_type`（判斷單元真型別，別信 ioreg property） |
- `tools/sc-interfaces.c` | 在 guest 內編譯即印出 **`SCNetworkInterfaceCopyAll()` 真正看得到的介面**（名稱/型別/BSD name）。驗收 Wi-Fi 服務就靠這支：介面不在清單裡 = SC 根本沒註冊它，`IOInterfaceType` 再對也沒用（§6.7） |
| `docs/hardware-verification.md` | 實機 stage0/stage3 陷阱與 hash 比對 |

---

## 10. 重跑 / 救援（Acer `100.64.0.152`）

```bash
# 0) 一次建好縮寫（Acer 上）：/tmp/g.sh = exec ssh -o StrictHostKeyChecking=no -p 2223 user@127.0.0.1 "$@"

# 1) 同步原始碼並在 guest 內編譯
cd ~/Data/Dev/qca6174-native && tar czf /tmp/qca-src.tgz --exclude=build --exclude=.git --exclude=artifacts-ci* .
scp -q /tmp/qca-src.tgz 100.64.0.152:/tmp/
ssh 100.64.0.152 'scp -q -P 2223 /tmp/qca-src.tgz user@127.0.0.1:/tmp/ && /tmp/g.sh \
  "tar xzf /tmp/qca-src.tgz -C ~/qca && cd ~/qca && make VENTURA=1 -j2 kext 2>&1 | grep -E \"error:|Built\" | tail -3"'

# 2) 部署 + 開機（複製上一版 deploy 腳本、整份改編號，不要用 sed 半套改）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_vNN.sh'

# 3) 讀斷點（先 serial、再 ioreg）
ssh 100.64.0.152 'tr -d "\000" < /tmp/ventura-vm.serial | grep -iE "ath10k|panic|Kernel trap" | tail -20'
ssh 100.64.0.152 '/tmp/g.sh "ifconfig -l; ioreg -l -w0 | grep -E \"Ath10k(StartPhase|IfacePhase|VifPhase|RecoveryPhase|Apple80211Requests)\" | head"'

# 4) 網路/SC 狀態
ssh 100.64.0.152 '/tmp/g.sh "networksetup -listallhardwareports | grep -A2 -i wifi; networksetup -listnetworkserviceorder | tail -6; networksetup -getairportpower en2"'

# 5) 截圖（驗 C 用）
ssh 100.64.0.152 'printf "screendump /tmp/shot.ppm\n" | socat - UNIX-CONNECT:/tmp/ventura-vm.mon'
# 再 python3 -c "from PIL import Image; Image.open('/tmp/shot.ppm').save('/tmp/shot.png')"
```

**救援（panic 循環／開機卡死／網路堆疊壞掉，不需要 guest SSH）**：
```bash
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/install_bundle.sh /tmp/vk48'   # 現行可用版
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/install_bundle.sh /tmp/vk20'   # 最保險的舊穩定版
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/disable_boot.sh'               # 可登入但不載入 kext
```
`install_bundle.sh <dir>` 直接從 Acer 的 `/tmp/vkNN` 取 bundle 寫進 ESP 再開機，完全不依賴 guest。Acer 上留著 vk20…vk51。

**本輪新增的可重用腳本（Acer `/tmp/`，密碼一律 `cat ~/sudo.pwd |` 餵 stdin）**：
```bash
bash /tmp/build_v.sh                       # 本機：tar src → Acer → guest 內 make VENTURA=1 -j2 kext
ssh 100.64.0.152 'bash /tmp/pull51.sh'     # Acer：從 guest 拉 build 成 /tmp/vk51
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_arg.sh /tmp/vkNN'   # 部署任意版本並開機
```
注意：`deploy_arg.sh` 用 `read -r PW` 收密碼，**同一個 ssh 指令裡不要先跑會吃 stdin 的腳本**（例如再包一層 ssh 的 pull），否則密碼會被吃掉、sudo 認證失敗。

**離線驗證 vtable（不開機，改 header 後必做）**：guest 內 `otool -rv` + `otool -s __DATA __const` + `nm -n`，本機用 KC symtab 重建 live 表比對；腳本在 `~/.hermes/cache/scratch/`（`kc_own.py`、`kc_sym.py`），方法見 §6.5。

---

## 11. 分析工具（可直接重用）

- `/tmp/kc2`：Ubuntu 上的 `BootKernelExtensions.kc` 副本（69MB），不用再從 guest 抓。
- `/tmp/an.py` + venv `/tmp/cs`（capstone 5.0.7）：
  - `list <子字串>` 找符號、`vtable <位址> <entry數>` 解 vtable（含 chained fixup 解碼）、`dis <符號或位址> <bytes>` 反組譯並標註 call 目標、`str` / `ptr` 讀字串指標。
- `/tmp/vt_both.txt`：兩份 `IO80211Controller` vtable 對照（`vt+` 欄位才是 kernel 索引用的值）。
- 檢查「符號是否 exported」的小腳本：解析 KC symtab，看 `n_type & 1`（`N_EXT`）。判斷 undefined 符號能不能用這是唯一可靠方法。
- **解析 KC 的地雷**：抽單一 kext 的 Mach-O 後不能直接解析它的 `LC_SYMTAB`（offset 是整個 KC 的絕對位置）；local 符號（`ext=False`）不能引用，引用會讓 OpenCore 跳過整個 kext。

---

## 12. 其他已知陷阱

- **不要在 VM panic 循環時 `scp` 進 guest** → 用 `install_bundle.sh`。
- **不要** `pgrep -q`（此 Ubuntu 不支援）、**不要** `pkill -f qemu-nbd` → 用 `systemctl --user stop qca-vm`；重開 VM 用 QEMU monitor 的 `system_reset`，不要用 `shutdown -r`（agent 端被硬封鎖）。
- **不要**用 `ioreg | grep 0x168c` 判斷網卡（byte-swapped）→ 用 `system_profiler SPPCIDataType`。
- **qemu-nbd**：VM 開著會 `Failed to get write lock`；掛載後等 `/dev/nbd0p1` 最多 15 秒。
- **多層 ssh 引號地獄**：一律把腳本 `scp` 到 guest 再 `/tmp/g.sh "sh /tmp/x.sh"`；要 sudo 的腳本讀 `read -r PW` 並用 `cat ~/sudo.pwd |` 餵。
- **sshd banner timeout 有三種意思**：(a) serial 有在長但 en0 壞 → 網路堆疊被弄壞（§4.2）；(b) serial 停在 OpenCore 之後、一行 kernel 都沒有 → 開機卡死（§5.11）；(c) 有 panic backtrace → 真的 panic。先看 serial 再判斷。
- **guest 的 Wi-Fi 服務狀態會快取**，改 plist 後必須整台重開（`system_reset`）才會重讀。
- **git push**：若 TLS 卡死，先檢查 `http.https://github.com.proxy` 有沒有被塞 socks5。
- **pip/gem 不要動系統 python**：capstone 裝在 `/tmp/cs` venv。
- **`getWorkQueue()` 回 null 會炸在 family 裡面**：`apple80211VirtualRequestIoctl` 第一件事就是 `call [vptr+0xc58]` 然後直接解引用回傳值，null → page fault（frame 卻顯示在 `Ath10kPCIDevice::start`，容易誤判成自己的 bug）。必須 override `createWorkQueue`/`getWorkQueue` 回 `IO80211WorkQueue::workQueue()`。
- **`IOService::serviceMatching("IO80211Interface")` 不會回傳子類別實例**（`getMatchingServices` 只給精確類別）；要掃自己的介面得用實際類名 `"Ath10kVenturaInterface"`。
- **`_venturaInterface` 不是擁有 BSD 單元的那顆物件**：`super::start()` 自己會建／發布介面，ioreg 會出現多顆 `Ath10kVenturaInterface`（v51c 量到 6 顆），而 `BSD Name` / `IOInterfaceUnit` 只長在其中一顆上。多介面還會讓 SC 連 Wi-Fi 硬體埠都不列 —— 別再加介面，先減（§7.1）。
- **改完 header 一律先做離線 vtable 比對再開機**：順序錯了不會編譯失敗，只會在開機時 panic 在看起來無關的 framework 函式裡（§6.5）。

---

## 13. 建議技能

- `osx-kvm-headless-install`：VM 生命週期、qemu-nbd、SSH 進 guest。
- `macos-kext-hardware-bringup`：kext 建置/注入、OpenCore 設定、IO80211 ABI 陷阱（已收錄本輪所有教訓：ASPM 非致命、stack insert 弄壞網路、initIfnetParams 的 IFT_ETHER 硬檢查、**IO80211Family 與 IO80211FamilyLegacy 兩份 IO80211Controller 的 ABI 差異與離線 vtable 比對法**、`getWorkQueue()` 必須非 null、SC 不吃 `IOInterfaceType`/`ifi_type`、不能在 kext 內 `new` Apple 類別）。
- `handoff`：下次交接沿用本文件結構。
