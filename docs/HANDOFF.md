# QCA6174 Ventura 原生 Wi-Fi — 交接文檔

最後更新：2026-11-22（本輪做了 radio bring-up、BSD 單元、SC 分類三輪實測，以及照抄社群解法的兩次嘗試）
分支：`ventura-io80211`　最新 commit：見 `git log -1`
VM 現況：**v48**（`/tmp/vk48`）— kext 有載入、`en2` 存在、firmware runtime ready、SSH 正常

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

## 2. 目前權威狀態（v48）

**環境**：macOS **13.7.8 / 22H730**；`com.apple.iokit.IO80211Family 1200.13.0`（載入基址 `0xffffff8001f35000`，長度 `0x19bff9`，**live 範圍 `0x1f35000`–`0x20d0ff9`**）；`IONetworkingFamily 3.4`；kext `br.com.jonasw8.driver.ath10kmac 0.0.105`；OpenCore 1.0.5 DEBUG（`Target=67`、`ShowPicker=false`、`Timeout=0`）。

| 觀察點 | v48 實測值 | 意義 |
|---|---|---|
| `ifconfig -l` | `lo0 gif0 stf0 en0 en2` | `en2` 是我們的 BSD 單元 |
| `Ath10kStartPhase` | `interface-published` | create→configure→attach→data-link→register 全過 |
| `Ath10kIfacePhase` | `inited` | 介面 init 成功（走繞過 v1 init 的路） |
| `Ath10kAlignPhase` | `aligned` | vtable slot 補丁已寫入 |
| `Ath10kStackPhase` | `driver-owns-ifnet` | 不再呼叫 `IONetworkStack`（見 §5.2） |
| `Ath10kASPMControl` | `write-ignored-continued` | QEMU 不吃 ASPM 寫入，已改為非致命（§4.1） |
| `Ath10kRecoveryPhase` | `ready` | **firmware runtime 活著** |
| `Ath10kWMIInitSucceeded` | Yes | WMI init 完成 |
| `Ath10kColdTargetInitSucceeded` | Yes | 冷啟動 target-init 成功 |
| `Ath10kFirmwareBootStatus` | `htc-ready-before-reset` | HTC ready 已到 |
| `Ath10kVifPhase` | `vif-create-failed` | 向 family 要 vif 失敗（§5.4、§6） |
| `Ath10kApple80211Requests` | **屬性不存在 = 0 次** | airportd 從沒送過請求（§4.4） |
| `ioreg -r -c IO80211Interface` | 2 筆 | 一筆 state 0（我們 start 內建的）、一筆 `BSD Name=en2` |
| `networksetup -listallhardwareports` | 有 `Hardware Port: Wi-Fi / Device: en2` | SC 認得這是 Wi-Fi 硬體埠 |
| `networksetup -listnetworkserviceorder` | `(3) Ethernet 2 (Hardware Port: Ethernet, Device: en2)` | **卡點：服務型別是 Ethernet** |
| `networksetup -getairportpower en2` | `Off (forced)`；想開會說 “all AirPort network services are disabled” | airportd 不接管 |
| `tools/ifnet-type`（ifnet 真實型別） | `en0 0x06`、`en2 0x47`（v41 實測） | ifnet 型別可以改成 80211，**但 SC 看的不是它** |

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

## 7. 下一個接手的人：照這個順序

### 7.1 先解結構性問題（優先，零風險）
**問清楚「這個 kext 為什麼會被靜默跳過」。**
```bash
# 在 guest 內手動載入，取得真正的拒絕原因（kext 已在 ESP，但沒被 OC 注入時）
sudo kmutil load -p /Library/Extensions/Ath10kMac.kext   # 或從 ESP 複製到 /tmp 再指定
sudo kmutil showloaded | grep -i ath10k
```
搭配：打開 OpenCore 的 kext 載入日誌（`Misc/Debug/Target=67` 已開），或比對「能載入的 v48」與「不能載入的 v49」binary 的 undefined 符號差異（`nm -u` 對 KC 匯出表）。
→ 有了真正的拒絕原因，才能判斷 §6.1 的 `super::start()` + `IO80211WorkLoop` 是不是可以重啟。

### 7.2 然後才是把 ifnet 交給 80211 核心
依 §6.2，缺的最後一步是 `prepareBSDInterface(ifnet, 0)`。優先序：
1. 設法取得一個 `IO80211VirtualInterface` / `IO80211InfraInterface` 實例（**不要**用 `new`，見 §5.10；也不要直接把 Skywalk 版 virtual 拿到我們 v1 子類身上呼叫，ivar 佈局不同會踩壞記憶體）。
2. 在我們的 `attachToDataLinkLayer` 成功後把 `getIfnet()` 交給它的 `prepareBSDInterface(ifnet, 0)`，並照 §6.2 補上那五個屬性再 `registerService()`。
3. 通了之後 SC 服務會自然變成 Wi-Fi，airportd 才會開始送請求 → 看 `Ath10kApple80211Requests` 是否 >0。

### 7.3 再把 80211 動作接到真的 WMI
`venturaScan()` 只清 index、`venturaScanResult()` 回 `NotFound`、`venturaAssociate()` 回 `kIOReturnUnsupported` —— **掃描/關聯核心邏輯完全沒接**。firmware 已 ready，WMI 掃描通道 `_wmiScan` 在 Sequoia 分支（`src/kext/native/`）有現成實作可搬。

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

---

## 9. 檔案地圖

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | `probe()`/`start()`：Ventura 分支建 work loop、備 corecapture、publish `IOPacketFilters`、建 output queue、手動 create→configure→attach→attachToDataLinkLayer→蓋型別→register、`preparePCIeBringupParity()`（ASPM 非致命）、vif 嘗試；所有 `Ath10k*Phase` 斷點 |
| `src/kext/ventura/Ath10kVentura.cpp` | `apple80211Request`（計數 wrapper）→ `venturaDispatchRequest`、`getLogPipes()`、`alignVenturaVtable()` + slot thunk、`createInterface()`、`venturaPower/Scan/Associate…`（**scan/assoc 仍 stub**） |
| `src/kext/ventura/Ath10kVenturaInterface.cpp/.hpp` | `Ath10kVenturaInterface : IO80211Interface`；`init()` 用 mangled `ath10kNetworkInterfaceInit`+`ath10kEthernetInterfaceInit`，attach 前釘 IFT_ETHER；覆寫 `initIfnetParams()`（改 `params->type=71`）與 `attachToDataLinkLayer()`（成功後蓋 `IOInterfaceType=71`）；`inputPacket()` 把 EAPOL 交給 IO80211 supplicant |
| `src/kext/ventura/Ath10kVenturaDecl.inc` | Ventura 專屬宣告：Skywalk enable/disable、16 reserved slot、`getLogPipes`、vtable thunk、請求計數器成員 |
| `src/kext/Ath10kNetwork.cpp` | `configureInterface()`、`createWorkLoop()`、`createOutputQueue()`、`getHardwareAddress()` |
| `src/kext/native/*` | Sequoia（v2）對照實作：WMI 掃描/關聯、`getFaultReporterFromDriver()`、`IO80211InfraInterface` 用法 |
| `tools/ifnet-type.c` | 在 guest 內編譯即印出 kernel 眼中的 `if_data.ifi_type`（判斷單元真型別，別信 ioreg property） |
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
`install_bundle.sh <dir>` 直接從 Acer 的 `/tmp/vkNN` 取 bundle 寫進 ESP 再開機，完全不依賴 guest。Acer 上留著 vk20…vk50。

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

---

## 13. 建議技能

- `osx-kvm-headless-install`：VM 生命週期、qemu-nbd、SSH 進 guest。
- `macos-kext-hardware-bringup`：kext 建置/注入、OpenCore 設定、IO80211 ABI 陷阱（已收錄本輪所有教訓：ASPM 非致命、stack insert 弄壞網路、initIfnetParams 的 IFT_ETHER 硬檢查、SC 分類時機、重複匯出符號、不能在 kext 內 `new` Apple 類別）。
- `handoff`：下次交接沿用本文件結構。
