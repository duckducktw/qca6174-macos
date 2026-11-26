# QCA6174 Ventura 原生 Wi-Fi — 交接文檔

**第四十輪（§6.45）補記**：`error 37 Already associated` 的來源找到了——**BSD 介面的 link 狀態沒掉**
（拆線後 `ifconfig en1` 仍是 `status: active` 且留著舊 IP）。`networkLink()` 只更新 controller，
沒推到已發佈介面。改用 live 的 `IO80211InfraInterface::setLinkState`（`IO80211Interface::setLinkState`
只存在 legacy bundle，引用它會讓 kext 直接載入失敗）。修好後 `status: inactive`。

**第三十九輪（§6.44）補記**：`-3903` 的真正來源是 **airportd 自己的 error 37「Already associated, and network
transition not permitted」**（`log show --predicate 'process == "airportd"'` 直接寫出來），不是驅動回的。
原因：驅動的 auto-join 先連上 + disassociate 沒真的拆鏈路。已加「拆鏈路 + 壓住 auto-join 60 秒」，
並在斷線時補送 LINK_CHANGED。另一個線索：airportd 眼中的目標 BSS 是 `BSSID:01:00:00:00:00:00 channel:0`。

**第三十七輪（§6.42）補記**：驅動端 join 只要 **約 2 秒**，但 `Apple80211Associate2` 每次都等 **10.2 秒**
然後回 `-3905`（errno 60＝讀事件逾時）——缺的是**完成事件**，不是關聯失敗。
已補 `M_LINK_CHANGED`/`M_ASSOC_DONE` 的 kern_event 發送（`Ath10kLinkEvent = up=1 kev(link=0 assoc=0)`）、
同網路快路徑，並把 v215 的「真拆鏈路」改回去（它讓 airportd 例行 DISASSOCIATE 反覆打斷連線）。

**第三十六輪（§6.41）補記**：自己呼叫 `Apple80211Associate2` 的請求**已經打到驅動**
（`Ath10kAssocWho = pid=… name=a2join`），卡點收斂成一個：字典裡的金鑰材料沒被認出來，
Associate2 送出的 `assoc_data` 是 `keylen=0`（開放式），驅動以 `protected-network-not-supported` 拒絕。
它自己會把 32-byte PMK 塞進 `ad_key.key`（`0xa25c: key_len=0x20, cipher=6`），所以只要字典形狀對就能通。

**第三十五輪（§6.40）補記**：`-3903` 在 `Apple80211Associate2` 內只有兩個出口，其中一個要求「密碼參數非 nil」
（`[ref+0x51] & 0x40` 這一位實測是 1）；airportd 傳給 Associate2 的字典是 `[[profile scanRecord] mutableCopy]`，
不是從零組的鍵值表 —— 這就是自己重組字典永遠對不上的原因。

**第三十四輪（§6.39）補記**：assoc 迴路上找到並修掉四個真 gate（`useAppleRSNSupplicant` 必須回 false、
第二次 VDEV_START 要 defer/replay、`DISASSOCIATE` 要真的拆鏈路、controller ivar `+0x1a0` 要填 role-6 vif）。
現在 `a2push`（用 ref 的 fd 送 type 20）可以走完 teardown → replay → connected-wpa2，
但 airportd 的 ASSOCIATE 仍然沒送到驅動（`Ath10kAssocWho` 一直是 kernel_task、trace 全 getter）→ 牆在 userspace 的 Associate2。

**第三十三輪（§6.38）補記**：系統點選連線走的是 `Apple80211Associate2`（`nm -u airportd` 有這個 import），
它自己用 `ioctl(ref+0x00 的 fd, 0x802869c8, {type=20,len=0x250})` 送 ASSOCIATE —— **繞過 `Apple80211Set` 的型別白名單**，
而且這條通道實測會到我們的 dispatcher（`Ath10kAssocRaw=upper=0008 lower=0000 keylen=12`、`AssocPhase=requested`）。
缺的是 assoc 之後的完成回報（userspace 拿 EIO／-3905）。所以「只能關 SIP 改 framework」的結論太強。

最後更新：2026-11-26（**第三十二輪：把「不能隨點連線」定位到 framework 的 userspace 白名單**。
`-3903` 的完整鏈路已釘死：`Apple80211Set` 本體以位圖 `0x08089000`（只放行 12/15/19/27）過濾型別，
type 20 落到 `0x7ffb1eb43b11`：拿 `OS_os_log` 記一條 log、直接把 `r15d` 設成 `0xfffff0c1` 後返回。
所以 airportd 永遠發不出 ASSOCIATE，隨點連線只能靠「關 SIP 改 framework」或「驅動自連」。
本輪同時：P-256 在核心裡真的算得出來（`Ath10kP256SelfTest = mul-ok`）、SAE 偵測與 AKM 傳遞打通
（`Ath10kSAEPhase = "sae-detected … upper=1000"`）、SAE/PMF 類別進 Ventura 連結、
開機 panic 收斂（`ath10kvifbsd` 閘門）、自動連線等 WMI 就緒
（`Ath10kWmiLive = active`、`Ath10kAutoJoinPhase = up`、`connected-wpa2`、DHCP 192.168.77.75）。
多網路：`ath10kjoin=ssid:psk,…` 開機參數。真機 ESP 已備妥但未重開驗證。）
分支：`ventura-io80211`　最新 commit：見 `git log -1`
VM 現況：**v212**（Acer `/tmp/vk212`，sha256 `474d0c3e…`）— 驅動自連、SSID 正確、DHCP＋上網已通


## 1. 目標與驗收條件

讓 QCA6174（`168c:003e` / 子系統 `11ad:0807`）在 macOS 13.x Ventura 上以**真正原生**形式運作，**不用相容層、不用獨立 app、不要求使用者開終端機**。驗證由 agent 在 VM 內自行完成（使用者留在 Ubuntu，只接實體網路線）。

| 條件 | 內容 | 現況 |
|---|---|---|
| **A** | `Ath10kIfacePhase = "inited"`、`Ath10kStartPhase = "controller-started"` | ✅ 已達成（Apple 的 `start()` 是唯一 publisher） |
| **B** | `ioreg -r -c IO80211Interface` ≥1 筆且有 `BSD Name`；`networksetup -listallhardwareports` 出現 Wi-Fi | ✅ 已達成（v64 起 `SCNetworkInterfaceCopyAll()` 第 3 筆 = `en1 type=IEEE80211`，Wi-Fi 硬體埠成立；一旦替它建了服務，`listallhardwareports` 會把它當「已設定」而不列，屬正常） |
| **C** | 「系統設定 → Wi-Fi」可見、開關可操作 | ✅ 有截圖（v149 起：`kcpassword` 自動登入生效，System Settings → Wi-Fi 面板可開、開關顯示為開；掃描清單仍受系統的「重開機提示」與 airportd 過濾影響，見 §6.15.6／§6.24） |
| **D** | 可掃描到 AP 並以 WPA2 連線取得 IP | ✅ 已達成（v165：`cwscan` 13 個網路、WPA2-PSK 關聯＋4-way（`connected-wpa2`、securityPhase 5）、**DHCP 取得 192.168.77.75**（`dhcp_bound(en1)`、`RouterARPVerified=TRUE`）、預設路由指到 AP 時 `ping 1.1.1.1` 3/3 且 `https` 200，§6.25） |
| **E** | 全程不改系統設定以外的東西、不需終端機、無獨立 app；最後部署回實體 ESP 並真機重驗 | ❌ 未開始（§7.4） |

**一句話總結現況**：無線電在跑、Apple 的 `start()` 發布了 `en1`，而**第五輪找到了「SC 看不到介面」的真兇**：不是 SC 的分類規則、不是 80211 core 的註冊，而是介面節點上 `IOInterfaceType=71` 這個**屬性**——`SCNetworkInterface.c` 只接受 `IFT_ETHER(6)`/`IFT_IEEE1394(13)`，其他值直接 `"unknown interface type"` 把介面丟掉。改回 6 之後 SC 立刻列 `en1 type=IEEE80211`（因為它改看 controller 是否符合 `IO80211Controller`），Wi-Fi 硬體埠與服務都成立。**最後一哩是 airportd**：它透過 `IO80211Controller` 的 +0x200 ivar（`getNetworkInterface()`/`getInfraInterface()` 都只是讀這個 pointer）找裝置，而該 ivar 實測是 null。

---

## 2. 目前權威狀態（**v62**，2026-11-22 第四輪結束時）

**環境**：macOS **13.7.8 / 22H730**；`com.apple.iokit.IO80211Family 1200.13.0`（載入基址 `0xffffff8001f35000`，長度 `0x19bff9`，**live 範圍 `0x1f35000`–`0x20d0ff9`**）＋ **`com.apple.iokit.IO80211FamilyLegacy`（另一個 bundle，見 §6.5）**；`IONetworkingFamily 3.4`；kext `br.com.jonasw8.driver.ath10kmac 0.0.105`；OpenCore 1.0.5 DEBUG（`Target=67`、`ShowPicker=false`、`Timeout=0`）。

| 觀察點 | v62 實測值 | 意義 |
|---|---|---|
| `ifconfig -l` | `lo0 gif0 stf0 en0 en1` | 我們的單元是 `en1`（號碼會跟著 SC 記錄走，砍掉記錄就會重配，見 §6.7） |
| `Ath10kAlignPhase` | `abi-native` | header 已是 live B 順序，不再手動補 slot（§6.5） |
| `Ath10kAppleStart` | **`ok`** | Apple 的 `start()` 真的跑完（不是 fallback） |
| `Ath10kPublisher` | **`apple`** | 手動 create→attach→register 路徑**沒跑**（只有 `!appleStart` 才會跑，§7.1） |
| `Ath10kStartPhase` | `controller-started` | 手動段不再覆蓋這個值 |
| `Ath10kIfacePhase` | `inited` | 介面 init 成功 |
| `Ath10kWorkQueue` | Yes | `IO80211WorkQueue::workQueue()` 已供應（§6.6） |
| `Ath10kFaultSlotPhase` | `reporter` | live 的 `findAndAttachToFaultReporter` 真的呼叫到我們的 slot |
| `Ath10kStackPhase` | `driver-owns-ifnet` | 不再呼叫 `IONetworkStack`（§5.2） |
| `Ath10kASPMControl` | `write-ignored-continued` | QEMU 不吃 ASPM 寫入，非致命（§4.1） |
| `Ath10kRecoveryPhase` | `ready` | firmware runtime 活著 |
| `Ath10kVifPhase` | **`vif-registered`** | `createVirtualInterface(this, addr, 7)` 成功（role 7 = SoftAP/P2P，**不是**主介面，§6.7） |
| `Ath10kApple80211Requests` | **1** | 一次 vif 能力查詢曾送達 |
| `Ath10kVifBsdSource` | **`none`** | 五種取 ifnet 的方式全失敗（§6.8） |
| `Ath10kVifBsdPhase` | **`no-ifnet`** | 因此 vif 交接沒有東西可交給 `prepareBSDInterface` |
| `Ath10kVifPublishRetry` | `failed` | start 後 3 秒的延遲重試也一樣 |
| `Ath10kChildCount` | `6` | 控制器子節點 = `CCLogPipe`×4 + `CCDataPipe` + `IO80211VirtualInterface`（**介面不在裡面**） |
| `Ath10kFindEn0..3` | **`6`（ENXIO）** | `ifnet_find_by_name()` 在本 kext 完全不能用（§6.8） |
| 介面節點（`en1`） | `IOInterfaceType=6`、`IOInterfaceState=3`、`IOBuiltin=Yes`、`IOPrimaryInterface=No`、`BSD Name=en1` | 個個看起來都對；**6 是關鍵**（71 會被 SC 丟掉，§6.9.1） |
| `SCNetworkInterfaceCopyAll()` | **serial1 / en0 / `en1 type=IEEE80211`** | ✅ v64 起 SC 認得它是 Wi-Fi（§6.9.1） |
| `networksetup -listallhardwareports` | 有 `Hardware Port: Wi-Fi / Device: en1`；建了服務之後會從這份清單消失（已設定） | 正常行為 |
| `networksetup -getairportpower en1` | `Wireless is not available` | airportd 找不到裝置（`[controller+0x200]` null，§6.9.2） |
| `ipconfig getiflist` | 只有 `en0` | IPConfiguration 仍不知道 `en1`（未上層） |

---

## 3. 不可破壞的規則（每條附理由）

1. **Ventura 建置不得引用 `IO80211FaultReporter`** —— 13.7 沒有這個類別（它是新世代才有的），引用會讓 OpenCore 靜默跳過整個 kext。
2. **`Ath10kPCIDevice` 必須覆寫 `enable/disable(IO80211SkywalkInterface*)` 與 16 個 `_RESERVEDIO80211ControllerN()`** —— 缺任一個 → `Prelinked injection … Invalid Parameter`。
3. **先 `createWorkLoop()` 再跑 corecapture** —— `CCPipe` 建立需要 work loop。
4. **`IO80211Controller::start()` 現在會跑完（`Ath10kAppleStart=ok`），而且它是唯一的介面 publisher。** 它以前必 panic 是我們自己的 ABI 沒對齊（§6.5/§6.6）；修好後**不要**再繞過它去手動 create/attach/register，也不要同時跑兩套（§7.1 已改成只在 `!appleStart` 時才用手動段）。
5. **除錯優先用 serial** —— kext 的 `IOLog` 在 `log show`/`dmesg` 看不到，但會出現在 QEMU serial（`/tmp/ventura-vm.serial`）。`Ath10kVenturaInterface: initIfnetParams failed`、`READY Qualcomm Atheros QCA6174`、panic backtrace 都是從這裡讀到的，比 ioreg 快一步。
6. **部署前必須跑 `oc-enable.py`** —— 漏了就會出現「完全沒有斷點」的假象。
7. **vtable 位移的正確算法**：物件的 vptr 指向 `&vtable[2]`，所以 kernel 端 `[vptr+0xNNN]` 對應的 **vtable index = (0xNNN + 0x10) / 8**。手上有「idx」清單時，vptr 位移 = `idx*8 - 0x10`。用錯會差一格（§5.3 就是這樣踩的）。已知 index：vif 的 285 `prepareBSDInterface`、287 `getBSDInterface`、456 `attachToBpf`、457 `configureIfnet`。
8. **不要碰 `IONetworkStack` 的插入函式**（vtable 位移硬呼叫會毀掉整個網路堆疊，§5.2）。也**不要**再假設「SC 會自己發現新介面」——量測方法只有 `tools/sc-interfaces.c`（§6.7）。
9. **`ifnet_find_by_name()` / `ifnet_list_get()` 在本 kext 不可用**（連 `en0` 都回 ENXIO，§6.8）。找自己的 ifnet 只有兩條路：從持有它的物件拿指標，或自己 `ifnet_allocate`。
10. **SC / networkd 看不到的介面，ioreg 再漂亮都沒用** —— 驗收一律先跑 `tools/sc-interfaces.c`，不要用 `IOInterfaceType`、`ifi_type`、plist 記錄來判斷成敗（§6.7）。
11. **介面節點的 `IOInterfaceType` 屬性必須是 6（IFT_ETHER），永遠不要蓋成 71。** `SCNetworkInterface.c` 只認 6 與 13，其他值會被 `default:` 直接丟掉，介面就從 SC 清單消失；Wi-Fi 分類是看 **controller 是否符合 `IO80211Controller`**，不是看這個屬性（§6.9.1）。ifnet 型別也一起留在 6。
12. **不要用 registry 掃描找自己的介面，尤其不要在延遲 thread 裡做。** `IOService::getMatchingServices()` 在 `start()` 後 3 秒的 thread_call 裡會把 kernel 卡死（`registry root held busy`，SSH 永不回 banner、無 panic，只能主機端換 bundle 救援）。
13. **`[controller+0x200]` 是 airportd 的入口，而且必須在 Apple 的 `start()` 之後才能寫。** 先寫會讓 `attachInterface()` 跳過整個 attach/register 流程（Wi-Fi 硬體埠消失）。

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
Apple 的 attach 流程會**自己再造一個介面物件**：`createInterface()` 交給我們的那個（`_venturaInterface`）不是最後拿到 BSD 單元的那個，它的 `getIfnet()` 回 **null**（v52/v62 實測）。Apple 也沒有把介面停在 controller `+0x200` ivar（讀出來也是 null）。所以**任何「在我的指標上蓋屬性／拿 ifnet」的作法都會失敗**——要蓋屬性只能用 registry 上真正掛單元的那個節點（而它也不是控制器的子節點，§6.8）。

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
| 5.12 | 手動修 SystemConfiguration | 手動改 plist 可以寫入，但**沒有硬體埠就沒有服務**；而且根源是 SC 根本看不到介面（§6.7） | — |
| 5.13 | 四輪「把 enN 的 ifnet 交給 vif」嘗試（`getIfnet()`、控制器 ivar `+0x200`、掃 registry 子節點、`ifnet_find_by_name`、`ifnet_list_get`、3 秒延遲重試） | 全部拿不到 ifnet：前者回 null，後兩者回 ENXIO／看不到自己的單元（§6.8） | v54–v62 |

### 5.1 手動重分類 SystemConfiguration（全失敗，**且已證明是白費工**）
- 改 `preferences.plist` 把 en2 服務的 `Type/Hardware` 設成 `IEEE80211` → 重開機被 SC 用原值覆蓋。
- `networksetup -removenetworkservice "Ethernet 2"` → 拒絕：`You cannot remove Ethernet 2 because there aren't any other network services on Ethernet.`
- `networksetup -createnetworkservice "Wi-Fi" en2` → `en2 is not a valid hardware port name.`
- 刪掉 `NetworkInterfaces.plist` 的 en2 記錄 + `preferences.plist` 的服務 + ServiceOrder，再重置 VM → **SC 連新記錄都不建**（不是「重新推導成 Ethernet」，是「看不到這個介面」，§6.7 修正了原本的判讀）。
- 手動把 en1 記錄（`SCNetworkInterfaceType=IEEE80211`、`IOInterfaceType=71`）塞回 `NetworkInterfaces.plist` → 記錄存活，但**仍然沒有硬體埠、沒有服務**：服務推導要靠「活著的介面被 SC 認出來」，不是靠記錄。
- `launchctl kickstart -k system/com.apple.configd` → `Operation not permitted`（SIP）。

### 5.2 SC 分類機制的量測結論（**已於 §6.7 修正，別再照這節推理**）
- 舊判讀：`NetworkInterfaces.plist` 那筆 `IOInterfaceType = 6` 是 SC 在 `registerService()` 時讀 property 寫下的。
- **實際**：那個 6 是**舊版建置留下的快取記錄**。`SCNetworkInterfaceCopyAll()` 顯示 SC 從來沒有看見這個介面（只有 `serial1`/`en0`），所以「SC 的分類規則」這個題目本身問錯了 —— 門檻在**註冊**（networkd / `IONetworkStack`），不在分類。
- 仍成立的部分：把 71 提前會讓 `initIfnetParams` 拒絕、單元消失（§4.3）；`IOInterfaceType` 與 `ifi_type` 兩者都不是 SC 的判準。
- **結論：不要再試「把 Ethernet 形狀的介面硬掰成 80211」，也不要再改 SC 的 plist；先讓介面出現在 `SCNetworkInterfaceCopyAll()` 裡。**

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

### 6.5 根因：兩個 IO80211Controller（2026-11-22 離線比對 KC 得出，已可重現驗證）

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

### 6.6 第二輪實測（v51）：ABI 修好後 Apple 的 start() 首次跑完

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
（↑ 這兩條的判讀在 §6.7 被推翻：SC 從頭到尾沒看見這個介面，那個 6 是舊快取記錄。本節保留當時的推論過程。）
→ **結論：SC 的分類不吃 `IOInterfaceType` property，也不吃 `ifi_type`。** 它八成看的是「這個介面有沒有掛進 IO80211 plane / 是不是 80211 core 發布的介面」。所以下一步不是繼續掰 SC，而是走 vif 發布路線：把 ifnet 交給 `IO80211VirtualInterface::prepareBSDInterface(ifnet, 0)`，照 §6.2 補 `built-in` / `IOInterfaceName` / `IOInterfaceUnit` / `IOInterfaceNamePrefix` / `IOMACAddress` 再 `registerService()`，讓 80211 core 自己發布 BSD 單元（那時 SC 記的就會是 Wi-Fi）。

---

### 6.7 第三輪實測（v52/v53）：SC 不是分類錯，是**完全看不到介面**

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

### 6.8 第四輪（v54–v62）：vif 借 ifnet 的五個量測結論（死路全部標記完）

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

### 6.9 第五輪（v63–v70）：SC 的真兇是 `IOInterfaceType` 屬性

#### 6.9.1 根因：SC 只接受 IFT_ETHER / IFT_IEEE1394 的屬性值

`SystemConfiguration` 的 `SCNetworkInterface.c`（開源：apple-oss-distributions/configd）裡，`processNetworkInterface()` 的邏輯是：

```c
num = interface_dict[kIOInterfaceType];           // ← 介面節點的「屬性」
if (!isA_CFNumber(num)) return FALSE;             // 沒有屬性 → 丟掉
ift = num;
switch (ift) {
  case IFT_ETHER:      // 6
      // 這裡才判斷是不是 Wi-Fi：看 controller 是否符合
      // IO80211Controller / AirPortPCI / AirPortDriver
      // → kSCNetworkInterfaceTypeIEEE80211（Wi-Fi）
      break;
  case IFT_IEEE1394:   // 13
      break;
  default:
      SC_log(LOG_INFO, "unknown interface type = %d", ift);
      return FALSE;                                // ← 介面直接從清單消失
}
```

也就是說：

- 前四輪一直把 `IOInterfaceType` 蓋成 **71（IFT_IEEE80211）**，SC 走到 `default:` 就把整個介面丟掉 → `SCNetworkInterfaceCopyAll()` 只有 serial1/en0、`listallhardwareports` 沒有 Wi-Fi。**這不是「SC 沒看到介面」，是「SC 看到了但拒絕收」**。
- 值留在 **6** 時，SC 走 `case IFT_ETHER`，再去看 controller 是否符合 `IO80211Controller` → 介面就被歸類成 **IEEE80211（Wi-Fi）**，硬體埠與服務都成立。
- 802.11 的「味道」來自 **controller 類別**，不是介面的 ifnet 型別，也不是那個屬性值。AirportItlwm 從來沒動過型別（見 6.9.4），所以它在真機上沒這個問題。

**v64 實測**（把 `initIfnetParams()` 的 `params->type=71` 與 `attachToDataLinkLayer()` 的 71 覆寫全部拿掉）：

```
SCNetworkInterfaceCopyAll() -> 3
[0] bsd=serial1 type=Modem
[1] bsd=en0     type=Ethernet
[2] bsd=en1     type=IEEE80211   hw=94:e9:79:ed:af:b9       ← Wi-Fi
networksetup -listallhardwareports -> Hardware Port: Wi-Fi / Device: en1
```

`networksetup -createnetworkservice "Wi-Fi" en1` 也成功（前幾輪是 `en1 is not a valid hardware port name`）。建完服務之後 `listallhardwareports` 就不再列 en1（SC 把「已設定」的介面排除在硬體埠清單外），這是正常行為，不是退化。

#### 6.9.2 剩下的門檻：airportd 找不到裝置（controller +0x200 ivar 是 null）

Wi-Fi 埠、Wi-Fi 服務都有了，但 `networksetup -getairportpower en1` 仍是 `Wireless is not available`，`airportd`（pid 210）**沒有對 IO80211 開任何 user client**。KC 反組譯指出原因：

- `IO80211Controller::getNetworkInterface()` = `safeMetaCast([this+0x200], IO80211Interface::gMetaClass)`
- `IO80211Controller::getInfraInterface()` 也是讀 `[this+0x200]`
- `IO80211Controller::attachInterface(IONetworkInterface **, bool)` 在成功之後才寫 `[this+0x200] = *iface`

我們這一版（`Ath10kPublisher=apple`、`AppleStart=ok`、`en1` 有 BSD Name）讀出來的 `[this+0x200]` 卻是 **null**（斷點 `Ath10kCtrlInfraIvar = No`）→ airportd 拿不到介面 → 不送請求（`Ath10kApple80211Requests` 一直是 1，那 1 次是 CARD_CAPABILITIES=12 的單次探測）。

airportd 的執行線索（`strings /usr/libexec/airportd`）：`Received XPC launch event com.apple.iokit.matching for IO80211Controller`、`IO80211VirtualInterfaceRole`，也就是它被 IO80211Controller 的 IOKit match 事件叫起來，然後透過 controller 取介面。

#### 6.9.3 這一輪踩到並標記的死路

| # | 嘗試 | 結果 |
|---|---|---|
| 1 | `ifnet_allocate`（type 71、`ifnet_attach` 自己來）→ 交給 vif `prepareBSDInterface` | **panic**：`IOSkywalkNetworkInterface::prepareBSDInterface+0x1a`，`[ivars+0]+0x4c` 解引用 null。ivars 存在但 `ivars[0]`（帶 MTU 的 interface extra）是 null —— 因為這顆 vif 從沒向 Skywalk stack 註冊（`registerNetworkInterface`）。`ifnet_allocate`/`ifnet_attach` 本身沒問題（`Ath10kOwnIfnetPhase=attached` 之前就 panic 在 prepare）|
| 2 | 延遲重試裡用 `IOService::getMatchingServices()` 掃 registry 找自己的介面 | **kernel wedge**：serial 出現 `registry root held busy, kernelmanagerd has not checked in`，SSH 永不回 banner、無 panic。延遲 thread 裡不要掃 registry |
| 3 | 在 `createInterface()` 就寫 `[this+0x200] = interface` | **Wi-Fi 硬體埠消失**：`attachInterface()` 開頭有 `cmp [rbx+0x200],0; jne done`，ivar 一旦先被寫，Apple 就跳過整個 attach/register 流程。要在 Apple 的 `start()` **之後**才寫 |
| 4 | 在 `start()` 之後讀 `_venturaInterface` 再寫 ivar | 讀到 **null**（`Ath10kCtrlInfraSeen = No`），連 `createInterface()` 寫的 file-static 版也是 null → 這條 pointer 的來源本身還沒釐清（下一步：在 `createInterface()` 裡掛呼叫計數 `Ath10kCreateIfaceCalls`，確認 Apple 的 start 到底有沒有走我們的 override）|

#### 6.9.4 AirportItlwm v2.2.0 的真實形狀（本輪把原始碼抓下來看過）

`itl80211/openbsd/sys/_if_ether.h` 在 macOS 分支裡：

```c
static inline int if_attach(struct _ifnet *ifp) { ifp->if_link_state = -1; return 0; }
static inline int ether_ifattach(struct _ifnet *ifp, IOEthernetInterface *iface) { ifp->iface = iface; }
```

→ **它在 macOS 上根本沒有自己建 ifnet**（那顆 `ic_ac.ac_if` 只是 net80211 自己的帳）。BSD 單元完全是 Apple 的 `IO80211Controller::start()` 建的，跟我們一樣；它的 `start()` 自己呼叫 `super::start()` 之後再 `attachInterface((IONetworkInterface **)&fNetIf, true)`。

所以「借 ifnet / 自己 allocate ifnet」對這條路線是**多餘的**；差別只剩它明確呼叫了 `attachInterface()`（會寫 +0x200），而我們的 Apple `start()` 路徑沒有留下那個 ivar。

---

### 6.10 第六輪（v71–v75）：ivar 補上、80811 init 走正規、airportd 仍不認

#### 6.10.1 為什麼「重試讀到的指標是 null」

量到三件事（v72）：

| 斷點 | 值 | 意義 |
|---|---|---|
| `Ath10kInstanceCreate` / `Ath10kInstanceStart` / `Ath10kInstanceRetry` | **完全相同** | 只有一顆 controller 實例，不是雙實例 |
| `Ath10kCreateIfaceCalls` | 1 | `createInterface()` 真的被 Apple 的 `start()` 呼叫了 |
| `Ath10kStaticCallsSeen`（重試時讀 file-static） | **0** | 重試（3 秒）**比介面建立更早** —— Apple 的 attach 是延遲/非同步的 |

→ 既不是 ivar 佈局重疊、也不是兩份 kext，就是**時序**。原本「3 秒延遲一次」太早。

#### 6.10.2 改成持續重試 + 在 attach 裡補 ivar（v74）

- `runVifPublish()` 改成每 5 秒重新 arm（最多 8 次），直到 `[this+0x200]` 非 null。v74 實測：`Ath10kCtrlInfraSeen = Yes`（第 2 次重試，約 8 秒）→ `Ath10kCtrlInfraInstalled = retry`，ivar 補上了。
- `Ath10kVenturaInterface::attachToDataLinkLayer()` 也會在 base 成功後補 ivar（`Ath10kDataLinkEntered=Yes`、`Ath10kDataLinkStatus=0`，代表這個 override 真的會被叫到），但 Apple 發布這顆介面時走的是別條路，所以主要還是靠重試。
- 注意：`AttachToDataLinkLayer` 補 ivar 不可放在 `createInterface()`（§6.9.3 #3 已證明會讓 `attachInterface()` 提前 return、Wi-Fi 埠消失）。

#### 6.10.3 `IO80211Interface::init()` 走正規（v75）

原本 `Ath10kVenturaInterface::init()` 是**繞過** `IO80211Interface::init`，只呼叫 `IONetworkInterface::init` + `IOEthernetInterface::init`（mangled）。airportd 要靠 `IO80211APIUserClient` 下 ioctl，那份 plumbing 就是 `IO80211Interface::init` 建的。v75 改成先呼叫 `__ZN16IO80211Interface4initEP19IONetworkController`（KC 裡是 EXT 匯出，可安全綁定）：

```
Ath10kIfInit80211 = ok     ← 正規 init 成功
Ath10kCtrlInfraInstalled = retry
en1 照樣存在、SC 照樣認得 IEEE80211
```

失敗時的 fallback 保留（退回舊的兩段式 bypass），所以不會因為 Apple 哪天改行為就整個掛掉。

#### 6.10.4 這一輪排除掉的假設

| 假設 | 量測 | 結論 |
|---|---|---|
| 「IO80211 plane 沒掛」 | `ioreg -p IO80211 -c IO80211VirtualInterface` 是空的，**Apple 自己的 vif 也不在裡面** | ❌ 排除，plane 不是 airportd 的入口 |
| 「airportd 只是開機早期快取」 | `killall airportd` 後重查，仍然 `Wireless is not available` | ❌ 排除 |
| 「介面缺 `IOUserClientClass`」 | 節點上沒有這個屬性（只有子節點 `IONetworkStackUserClient`） | 🟡 未定（Apple 的介面也未必把這個屬性寫出來） |

#### 6.10.5 剩下的最強假設：我們沒有實作「infra 請求入口」

KC 裡 live IO80211Family 有這組匯出：

```
IO80211Controller::apple80211_ioctl(IO80211Interface*, IO80211VirtualInterface*, ifnet_t, u_int, void*)
IO80211Controller::apple80211_ioctl(IO80211SkywalkInterface*, u_int, void*)
IO80211Controller::apple80211_ioctl(IO80211VirtualInterface*, ifnet_t, u_int, void*)
```

而 legacy 驅動（`AirPort_BrcmNIC`）覆寫的是 `apple80211Request(unsigned, int, IO80211Interface*, void*)`。我們目前只覆寫了 **Virtual / Skywalk** 兩個入口（`apple80211VirtualRequest` / `apple80211SkywalkRequest`），**infra（`IO80211Interface`）那條沒接**——那正是 airportd 對實體 Wi-Fi 介面走的路。這解釋了為什麼 `Ath10kApple80211Requests` 從頭到尾只有 1 次（那次是 vif 的 CARD_CAPABILITIES 探測）。




---


### 6.11 第七輪（v76–v81）：airportd 的關卡是 **IFM_IEEE80211 medium**，不是 infra ioctl

§6.10.5 的假設（缺 infra 請求入口）**已被反組譯否證**：live `__ZTV17IO80211Controller`（`0xffffff80020d86f8`）只有兩個 `apple80211_ioctl`（slot 405 Virtual / 406 Skywalk），5 參數的 infra 版本只存在於 legacy 那份（`0x2186bce`）。

**真正的關卡從 airportd 自己的 log 讀出來**：

```
_getIfListCopy/ 7687: getifaddrs nInterfaces[ 12], count[  0]
apple80211d ... Apple80211GetIfListCopy: exit -3903/0xfffff0c1, ifCount[0]
```

比對正常機器（UniversalControl gist）可看到接受時會印 `Added 'IFM_IEEE80211' supported interface` → `_getIfListCopy` 會對每個介面問 `SIOCGIFMEDIA`，**只收媒體含 `IFM_IEEE80211`（0x80）的介面**。空手時 `supported[0]=0x20`（IFM_ETHER autoselect），所以機場 d 一律丟掉，與 `IOInterfaceType`、`ifi_type`、`+0x200` 都無關。

**修法（v80 起有效）**：

1. `publishMediumDictionary()` 發 `IONetworkMedium::medium(kIOMediumIEEE80211Auto /*0x80*/, 54000000)` + `setCurrentMedium`/`setSelectedMedium`。
2. **一定要在 `super::start()` 之後**：`publishMediumDictionary` 會鎖 `[this+0xc8]`，那把鎖在 `IONetworkController::start` 才配置，先呼叫就是 panic（實測 `createVenturaMediumTables+0x7d → _IOLockLock+0x3`，RDI=0）。
3. **但只在 start 之後發也不夠**：介面的 ifmedia 清單在 attach 期間就建好，所以要在 `configureInterface()` 裡、`IOEthernetController::configureInterface()` **前後各發一次**（v80 實測 `Ath10kMediumPreBase/PostBase = 00000080|cur=00000080`，`ifconfig en1` 的 media 隨之改變）。
4. `ifi_type` 留 71（airportd 端），但 `IOInterfaceType` property 留 6（SC 端）——兩個判準不同，別一起改。

**v81 量測**：`_getIfListCopy: exit 0/0x00000000, ifCount[1]`（媒體關卡已過），但外層 `Apple80211GetIfListCopy` 仍 `-3903/ifCount[0]`，`networksetup -getairportpower en1` 仍 `Wireless is not available`，`ioreg` 沒有任何 `IO80211APIUserClient`。

**剩下的牆（下一輪主題）**：外層 wrapper 會對找到的介面開 user client，而 live `IO80211APIUserClient::commonStart` 第一件事就是 `safeMetaCast(provider, IO80211SkywalkInterface)`；我們的 `Ath10kVenturaInterface : IO80211Interface`（v1）永遠過不了。同一個 cast 也是 `IO80211Controller::attachInterface(OSObject*,IOService*)` 收介面的條件（`"invalid skywalk interface"`，寫進 `[[this+0x120]+0x198]`，也就是 `getInfraInterface()` 的來源）。→ 要走的路是把 `src/kext/native/Ath10kNativeInterface`（Sequoia：`IO80211InfraProtocol` + `IO80211SkywalkInterface::init(controller, mac)`）**下放到 Ventura 的 `IO80211InfraInterface`**（`vendor/itlwm-v1/Airport/IO80211InfraInterface.h`，`IO80211InfraInterface : IO80211SkywalkInterface`）。

**這一輪重建的環境工具（Acer `~/qca-tools/`，本機同名副本）**：`dm_map.sh`（重建 `/dev/mapper/ventura-ro`：gpt-head.img＋`/dev/nvme0n1p2`＋gpt-tail.img）、`vfio_bind.sh`、`build_v.sh`、`pull_v.sh NN`、`esp_fix.sh [bundle]`、`boot_nokext.sh`（kext 會 panic 時先停用再編譯）、`press_enter.sh`（OpenCore 卡選單時送 `sendkey ret`）、`shot.sh`、`oc_enable.py`（ShowPicker=True/Timeout=3，`ShowPicker=False` 在沒有預設開機項目時仍會卡選單）、`oc_toggle.py`、`media-probe.c`。

**其他實測**：`IOEthernetController` 沒有 `createMediumTables` 這個虛擬（只有各家驅動自己的同名函式）；`IONetworkController::publishMediumDictionary` 在 `0xffffff8002c5f924`；`IONetworkStack` 會把介面節點的 `IOUserClientClass` 蓋成 `IONetworkStackUserClient`（en0 也一樣，所以那條屬性不是 airportd 的入口）。

**追加（v82–v84）：user client 這關換屬性也過不去**

- 在延遲重試裡把介面節點的 `IOUserClientClass` 改回 `IO80211APIUserClient`（`Ath10kUserClientBefore/After` 都錄到、`ioreg` 也確認節點上是 `IO80211APIUserClient`），**airportd 仍然 `Apple80211GetIfListCopy: exit -3903, ifCount[0]`，`class IO80211APIUserClient` 實例數 0**。
- 所以問題不在「節點有沒有廣告 user client」，而在 **class 本身**：live `IO80211APIUserClient::commonStart` 第一件事就是 `safeMetaCast(provider, IO80211SkywalkInterface)`，我們的 v1 介面過不了，`IOServiceOpen` 直接失敗，外層 wrapper 就把介面濾掉。
- **`IO80211InfraInterface` 不是 `IONetworkInterface`**：`__ZTV21IO80211InfraInterface`（`0xffffff80020e4700`）裡 0 個 `IONetworkInterface` slot，整條是 `IOSkywalkEthernetInterface → IOSkywalkNetworkInterface → IOSkywalkInterface`。所以**不能**直接讓 `createInterface()` 回傳它——13.7 上 Skywalk 介面的 BSD 單元是走 skywalk 自己的 `prepareBSDInterface(ifnet, unit)`／`registerNetworkInterface`，不是 `attachInterface(IONetworkInterface **, bool)`。
- 這也解釋了為什麼 `publishVenturaVifBsdUnit()` 會 panic：把 ifnet 交給沒向 skywalk stack 註冊過的 vif，`IOSkywalkNetworkInterface::prepareBSDInterface+0x1a` 直接炸（v83 的 backtrace：`publishVenturaVifBsdUnit+0x685 → IO80211VirtualInterface::prepareBSDInterface → IO80211SkywalkInterface::prepareBSDInterface → IOSkywalkEthernetInterface::prepareBSDInterface → IOSkywalkNetworkInterface::prepareBSDInterface`）。**不要為了別的診斷把重試次數拉長**，那只會多炸幾次。
- 下一步（真正的工程）：在 Ventura 上建一個 `IO80211InfraInterface` 子類（`OSDefineMetaClassAndStructors` 走 Apple 的 `IO80211InfraInterface::gMetaClass` 與 ctor，兩者在 live 都有：`0xffffff80020ee2a0`／`0xffffff800203f06c`），初始化序是 `IO80211InfraInterface::init()` → `IO80211SkywalkInterface::init(IOService*)`（Ventura 只有單參數版）→ `setInterfaceRole(1)`（`setInterfaceRole` 就是寫 `[ivars+0x50]`，正好是 `attachInterface(OSObject*,IOService*)` 檢查 role==1 的那格）→ `setSelfMacAddr()`，然後讓它自己向 skywalk stack 註冊、自己產生 BSD 單元；v1 `IO80211Interface` 那條線只留作 legacy 後備。



### 6.12 第八輪（v85–v87）：綁定的是哪一份 IO80211？——live，而且 **legacy 那條路對注入 kext 是關的**

#### 6.12.1 量測方法（可重用，一次開機就知道答案）

Ventura 的 KC 同時有兩份完整 IO80211（`IO80211Family` 與 `IO80211FamilyLegacy`），mangled 名字完全一樣。新增斷點把「解析到的超類別 metaclass」與一個**只有單一份**的參考點（`IOPCIDevice`，來自 IOPCIFamily）相減：

- `Ath10kCtrlMetaDelta`（`Ath10kPCIDevice::start()`，`super::start()` 之前）
- `Ath10kIfaceMetaDelta`（`Ath10kVenturaInterface::init()`）

位址差是 KASLR-slide 不變量，離線用 KC symtab 算好對照值即可：

| | 期望 delta（對 IOPCIDevice） |
|---|---|
| `IO80211Controller` live | `-0xE02028` |
| `IO80211Controller` legacy | `-0xC9E348` |
| `IO80211Interface` live | `-0xE01FD0` |
| `IO80211Interface` legacy | `-0xC9E318` |

**v85 實測：controller = live、interface = live。**（兩者一致，不是混合。）

#### 6.12.2 為什麼不是 legacy：OpenCore 只按宣告的依賴解析符號

`OcAppleKernelLib/Link.c`：`InternalOcGetSymbolName()` **只搜 `Kext->Dependencies[]`**，順序是「kernel 先、再照 `OSBundleLibraries` 的 plist 順序」，沒有全域掃描。我們的 Info.plist 根本沒宣告任何 IO80211 依賴 → 一律由 dependency[0]=kernel 的符號表命中，而那份是 **live**。宣告 `com.apple.iokit.IO80211FamilyLegacy` 也沒用，kernel 排第一。→ **legacy 綁定對注入 kext 不可得，這條路封閉**（不必再試）。

#### 6.12.3 Apple 自己在 13.7 用的是 legacy —— 但那是 prelinked kext 的特權

`AirPortBrcmNIC.kext` 是 `IO80211FamilyLegacy.kext/Contents/PlugIns/` 裡的 plugin，`OSBundleLibraries = com.apple.iokit.IO80211FamilyLegacy 1200.12.2b1`。實查 KC：

- `AirPort_BrcmNIC::superClass` → **legacy** `IO80211Controller`（`0x22519e8`）
- `AirPort_BrcmNIC_Interface::superClass` → **legacy** `IO80211Interface`（`0x2251a18`）
- 它覆寫 `apple80211_ioctl(IO80211Interface*, IO80211VirtualInterface*, __ifnet*, m, void*)` 與 `apple80211Request(ji P16IO80211Interface Pv)`——**5 參數 infra 入口存在於 legacy 表**。

它在 KC 裡是 Apple 用 kmutil 依賴排序 prelink 的，所以拿得到 legacy；注入 kext 拿不到（6.12.2）。**結論：13.7 的「真機原生路徑」我們用不了，只能走 live（Skywalk 期）**。

#### 6.12.4 兩份介面類別的 inheritance 實查（解釋為什麼 v1 永遠過不了 skywalk cast）

| class | superClass |
|---|---|
| `IO80211Interface`（live 與 legacy 都一樣） | `IOEthernetInterface` |
| `IO80211SkywalkInterface`（兩份） | `IOSkywalkEthernetInterface` |
| `IO80211InfraInterface` | `IO80211SkywalkInterface`（各自綁自己那份） |

live 的 `IO80211Interface` 只有 137 個符號、**沒有 `init(IONetworkController*)`／`start()`**；legacy 那份 826 個、實作齊全。我們的介面 `init` 之所以 `ok`，是因為那個 mangled 符號**只有 legacy 一份**（`0x21894ea`）——也就是說我們現在是「live 的 metaclass + legacy 的 init 實作」，這是 hybrid，遲早要面對。

#### 6.12.5 airportd 到底卡在哪：**不是在我們的節點上開 user client**

v86/v87 在介面與控制器都加了 `newUserClient` 計數器（`Ath10kNewUserClient` / `Ath10kCtrlNewUserClient`），並用使用者端程式驗證 harness 有效（自己 `IOServiceOpen` 控制器節點會讓計數 +1，type=1 → 0xe00002c7）。

- 觸發 `networksetup -getairportpower en1` 之後：**控制器計數不變（4 → 4）**、介面計數仍不存在、`Ath10kApple80211Requests` 仍 1。
- `ioreg -c IO80211APIUserClient` 實例數 0；對介面節點 `IOServiceOpen` 回 `0xe00002c2`（BadArgument），**而且我們的介面 `newUserClient` override 沒被呼叫**（回傳碼不是我們寫的 0xe00002c7）→ 這個 override 在介面節點上是死碼，介面端的計數結論無效，下一輪要先查它為什麼沒接上。
- airportd 的 log 兩段都印：內層 `_getIfListCopy … count[1]`（我們的 en1 過了 media 關卡），外層 `Apple80211GetIfListCopy: exit -3903/0xfffff0c1, ifCount[0]`。→ **濾掉的動作發生在外層 wrapper，而且完全沒有對我們的任何節點開 user client。**

#### 6.12.6 反組譯：attachInterface 與 user client 的硬條件（live）

`IO80211Controller::attachInterface(OSObject*, IOService*)`（live `0x1fed7b8`）：

```
safeMetaCast(iface, IO80211SkywalkInterface::gMetaClass)  ; null -> logDebug "invalid skywalk interface", return false
[this+0x120] -> ivars ; if (ivars[0x198] != 0) done
rcx = [iface+0x110] ; if (*(int*)(rcx+0x50) != 1) done   ; role 必須 1
ivars[0x198] = iface                                      ; getInfraInterface() 的來源
```

`IO80211APIUserClient::commonStart(provider)`（live `0x202445a`）第一行同樣是 `safeMetaCast(provider, IO80211SkywalkInterface::gMetaClass)`，失敗就 return false。

→ 兩道門都要「物件本身是 Skywalk 介面」，而 v1 `IO80211Interface`（不管 live/legacy）繼承 `IOEthernetInterface`，**永遠不可能通過**。這條是硬的，不是時序或屬性問題。

#### 6.12.7 OpenCore 支援「子類化 KC 裡的類別」

`OcAppleKernelLib/Vtables.c`：對注入 kext 的 `__ZTV<Child>`，它會去找 **super class 的 vtable（已 patch 的那份）**，逐 slot 把子類沒覆寫的條目指向父類的實作（`ParentEntry->Name == NULL` 才略過）。→ 建 `Ath10kInfraInterface : IO80211InfraInterface` 在機制上是可行的，不需要每個 Apple 虛擬都匯出。

#### 6.12.8 下一輪（唯一還沒被封的路）

1. 建 `Ath10kInfraInterface : IO80211InfraInterface`（用 Apple 的 `gMetaClass`/ctor，`OSDefineMetaClassAndStructors`），初始化序照 §6.11：`IO80211InfraInterface::init()` → `IO80211SkywalkInterface::init(IOService*)` → `setInterfaceRole(1)`（就是寫 `[ivars+0x50]=1`）→ `setSelfMacAddr()`。
2. 用 `IO80211Controller::attachInterface(OSObject*, IOService*)` 掛上去（**Apple `start()` 之後**、`ivars+0x198` 還是 null 時），斷點看回傳 true 且 `getInfraInterface()` 非 null。
3. 先只做到這裡就開機量測：`ioreg -c IO80211InfraInterface` 有沒有實例、`Apple80211GetIfListCopy` 是否還 -3903、`ioreg -c IO80211APIUserClient` 是否出現。**這一步就能判斷「skywalk 介面存在」是不是 wrapper 的唯一門檻**，不必先解決 BSD 單元。
4. 之後才是 datapath：Skywalk 註冊（`registerNetworkInterfaceWithLogicalLink`，需要 logical link 與 packet buffer pool）＋自己 `ifnet_allocate` 之後交給 `prepareBSDInterface(ifp, 0)`；v1 `IO80211Interface` 那條線只留作 SC 分類的後備。

### 6.13 第九輪（v88–v94）：把 Skywalk 物件掛上去 —— `attachInterface` 終於回 ok，但 airportd 還是 -3903

#### 6.13.1 Skywalk header 要先跟 live KC 對齊（不然整個 kext 進不去）

`vendor/itlwm-v1/Airport/IO80211SkywalkInterface.h` 是 Sonoma 期的順序，跟 13.7 live 的 `__ZTV23IO80211SkywalkInterface`（0xffffff80020dc138，660 slot／119 個自有）不一致：多了 `isCommandProhibited`、`forwardInfraRelayPackets`、`isDebounceOnGoing`、`setInfraSpecificFrameStats`、`cachePeer`、`getEventPipeSize`，少了 `getBSSID`。以 `~/.hermes/cache/scratch/vtcmp.py <vtable dump> <header> <class>` 比對後改成 live 順序（`IO80211InfraInterface.h` 的 `isDebounceOnGoing` override 也要一起拿掉）。**只要順序不符，OpenCore 直接不注入，序列埠一個字都不會有**（`kmutil showloaded | grep -c ath10k` = 0 是唯一線索）。

#### 6.13.2 自己寫 `IO80211InfraInterface` 子類不可行

`OSDefineMetaClassAndStructors(Ath10kInfraInterface, IO80211InfraInterface)` 可以編譯（父類沒有純虛擬），但注入後 kext 不載入。**改走 Apple 自己的 vif**：`IO80211Controller::createVirtualInterface(ether_addr*, 7)` 在 start() 早就建了一個（`Ath10kVifPhase=vif-registered`），把它交給 `IO80211Controller::attachInterface(OSObject*, IOService*)` 就好。

#### 6.13.3 v94：attach 成功（本輪真正的新狀態）

```cpp
extern "C" bool ath10kAttachSkywalkInterface(IO80211Controller *self, OSObject *iface, IOService *service)
    __asm("__ZN17IO80211Controller15attachInterfaceEP8OSObjectP9IOService");
// role 1 直接寫：attachInterface 只讀 [ivars+0x50]，ivars = [iface+0x110]
void **ivarsSlot = (void **)((char *)vif + 0x110);
if (*ivarsSlot) *(UInt32 *)((char *)*ivarsSlot + 0x50) = 1;
ath10kAttachSkywalkInterface(this, vif, this);
```

實測（v94）：`Ath10kInfraAttach = "ok"`、`Ath10kInfraPhase = "attached"`、`Ath10kInfraIf` 有值、`Ath10kVifPhase = "vif-registered"`、`Ath10kMediumPhase = "ieee80211"`、en1 在。**所以 `safeMetaCast(iface, IO80211SkywalkInterface)` ＋ role==1 這兩道門已經過了，`getInfraInterface()` 不再是 null。**

但 airportd 仍然：內層 `_getIfListCopy … count[1]`、外層 `Apple80211GetIfListCopy: exit -3903/0xfffff0c1, ifCount[0]`，而且**對我們任何節點都沒有 `newUserClient`**（`Ath10kCtrlNewUserClient` 完全沒被建立 = 從沒呼叫），request 數仍 1。log 裡除了這幾行沒有任何其他 airportd 訊息 → 卡點在使用者空間的 wrapper，且是**靜默**的檢查。

#### 6.13.4 建置踩到的兩個坑

1. **stale object**：把檔案從 `Makefile` 的 `VENTURA_OBJECT` 拿掉後，舊的 `Ath10kInfraInterface.o` 還是被連進去（`nm | grep -c Ath10kInfraInterface` = 23）。改動物件清單後**一定要 `rm -rf build/ventura`**，並用 `nm` 驗證符號真的消失。
2. **undefined symbol 對照要看乾淨的 build**：incremental build 會多出一堆 `IO80211Interface::*`／`_task_reference`／`_thread_call_*`，看起來像「新增參考就掛」，其實是物件組成變了。v94 與 v92 的 `nm -u` 逐行相同（715 行、零差異）才代表 symbol 集合沒變。

#### 6.13.5 下一步：把 wrapper 的靜默過濾拆開

`-3903` 發生在使用者空間（`/usr/libexec/airportd` 連 `/System/Library/PrivateFrameworks/IO80211.framework`，那是 stub，實作在 dyld shared cache），而且**不經過任何 IOKit 呼叫**（沒有 user client、沒有 request）。所以下一步是把 `Apple80211GetIfListCopy` 從 shared cache 抽出來反組譯，看內層成功之後它還檢查什麼（可能是 `Apple80211Open` 的 mach service、或某個 interface 屬性）。在這之前，不要再猜 kernel 側的門 —— kernel 側兩道 cast 已經都過了。

### 6.14 第十輪（v95–v110）：airportd 終於認得 en1 —— 卡的門是 `IOInterfaceName` ＋ `IO80211InterfaceRole`

#### 6.14.1 airportd 的使用者空間過濾鏈（完整解出，不是猜的）

方法：frameworks 在磁碟上是 stub，真正的程式在 dyld shared cache
（`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h`）。
`dyld_info` / `otool` 讀不了，`dtruss` 被 DTrace 限制擋住；改用
`dlopen`＋`dlsym`／`_dyld_get_image_header` 把程式碼從記憶體 dump 出來（`dumpaddr.c`、
`dumpsym.c`），再用 capstone 反組譯（`/tmp/cs/bin/python3`）。踩到的坑：`lea rsi,[rip+disp]`
的 disp 在指令結尾前 4 bytes（`48 8d 35` 之後），少算一格會 SIGSEGV；跨行程算位址會被
ASLR 打亂，一律在同一行程內算。

解出的鏈條（`Apple80211` → `IO80211.framework`）：

```
for each interface name from getifaddrs (ifi_type 71):
    svc = IOServiceGetMatchingService(kIOMainPortDefault,
              { IOPropertyMatch = { IOInterfaceName = "<bsd name>" } })
    if (!svc) drop
    v = IORegistryEntryCreateCFProperty(svc, "IO80211InterfaceRole", alloc, 0)
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) drop
    if (CFStringCompare(v, "Infrastructure", 0) != 0) drop
```

字串來源是 Apple80211 的 constant CFString：`IOInterfaceName`(15)、`IOPropertyMatch`(15)、
`IO80211InterfaceRole`(20)、`Infrastructure`(14)。stub 解析結果：`CFStringCompare` /
`IOObjectRelease` / `CFRelease` / `IORegistryEntryCreateCFProperty` / `CFGetTypeID` /
`CFStringGetTypeID` / `IOServiceGetMatchingService`。

**必要條件（實測）**：
- `IOInterfaceName = "en1"` 必須在**v1 介面節點**（`Ath10kVenturaInterface`）上。掛到 vif、
  或兩個都掛，airportd 都還是不認（v107 整個 match 不到 → 回到 "Wireless is not available"；
  v110 兩邊都掛 → 還是走我們自己的 client）。
- `IO80211InterfaceRole = "Infrastructure"` 也必須在該節點，值是 CFString。

滿足後：`networksetup -getairportpower en1` 從 **"Wireless is not available"** 變成
**"Wi-Fi Power (en1): Off"**，`Ath10kNewUserClient` 從 0 變成數百次 —— airportd 開始在
en1 節點上開 user client（type 0）。這是第十輪最重要的狀態變化。

#### 6.14.2 已實作：`Ath10kVenturaUserClient`

`src/kext/ventura/Ath10kVenturaUserClient.{hpp,cpp}`：`IOUserClient` 子類，只接受 type 0，
在 `externalMethod` 記錄 `Ath10kUCSel<n>` / `Ath10kUCalls` / `Ath10kUCLastSelector` /
`Ath10kUCLastScalarIn|StructIn|StructOut` / `Ath10kUCIn0|1|3`，並由
`Ath10kVenturaInterface::newUserClient` 產生。空殼版（回 `kIOReturnUnsupported`）會讓
airportd 重試數百次。

#### 6.14.3 `IO80211APIUserClient` 的分派表（反組譯得到）

`sMethods` 在 `0xffffff80020e1600`，每筆 `IOExternalMethodDispatch` 24 bytes：

| selector | 函式 | 重點 |
|---|---|---|
| 0 | `extRunDriverCommand` | 直接跳 `_runDriverCommandHelper`；要求 `scalarInputCount == 0`，且 `structureInputDescriptor` 的長度在 **0x3c30–0x4000**（`apple80211req` 那塊） |
| 3 | `extQueryUseIOUCWhenPossible` | 回 bool |
| 6 | `extSubscribeEvent` | 事件訂閱 |

**airportd 送到我們 client 的 selector 0 全部沒有輸入結構**（`Ath10kUCIn0` 從未出現），
輸出大小依序是 256 → 13 → 24 bytes。所以那不是真正的命令路徑：`Ath10kApple80211Requests`
一直到 v110 都還是 1，代表請求從沒進到 `venturaRecordRequest()`。

#### 6.14.4 目前卡點與三條可行路線

airportd 只認得 family 自己的 user client 協議，而我們能提供的是自製 client。三條路：

1. **讓 match 命中 skywalk 物件**（本輪嘗試失敗）：把 `IOInterfaceName` 放到 vif，讓
   `IO80211APIUserClient::commonStart` 的 `safeMetaCast(provider, IO80211SkywalkInterface)`
   過關。失敗原因是 `IOServiceGetMatchingService` 命中 v1 節點（或 vif 取名時 ifnet 還不存在，
   v109 實測 `Ath10kVifInterfaceName="unresolved"`；v110 改成在 `attachToDataLinkLayer`
   時才寫，兩個節點都有名字，結果 airportd 還是走我們的 client）。
   → 還沒試的變體：把 `IOInterfaceName` **只**留在 vif 且確認真的寫進去（v107 失敗是因為沒寫進去）。
2. **自己實作 `_runDriverCommandHelper` 的等價協定**：解析 0x3c30 大小的請求結構
   （`apple80211req`：requestType / requestNumber / data），轉呼叫既有的
   `Ath10kPCIDevice::venturaRecordRequest()`，再把結果填回 structure output。
   需要先量到真正的輸入（目前還沒看到）。
3. **走 BSD ioctl**：在介面上實作 `SIOCSA80211`／`SIOCGA80211` 的 ifnet ioctl，讓 airportd
   走無 user client 的路徑。前提是確認 airportd 有這條 fallback（selector 1 回 false 時它
   直接停住，量測顯示沒有 fallback）。

#### 6.14.5 現在的 VM 狀態

- v110 已部署（`/tmp/vk110`）：en1 存在、`Wi-Fi Power (en1): Off`、`Ath10kUCSel0≈80`、
  `Ath10kUCalls≈90`、`Ath10kApple80211Requests = 1`、`IO80211APIUserClient` 實例 0。
- 可用對照組：v108（只有介面節點有名字，同樣被接受）、v94（infra attach ok，但當時還沒解出
  名字／角色兩道門）、v87（最早能載入的版本）。

#### 6.14.6 這輪的工程教訓

- **改 `VENTURA_OBJECT` 或換 class 後一定要 `rm -rf build/ventura`**：舊 `.o` 會被連進去，
  症狀是「明明拿掉了還是注入失敗」。用 `nm | grep -c <class>` 驗證。
- **`nm -u` 比對只能在兩個乾淨 build 之間做**，incremental build 會多出一堆
  `IO80211Interface::*`／`_thread_call_*`，看起來像新增參考導致失敗。
- **使用者空間不能改我們的節點屬性**（`IORegistryEntrySetCFProperty` 回 `0xe00002c7`），
  每個屬性實驗都要重編＋開機。
- 取 BSD 名只能在 `attachToDataLinkLayer()` 之後（`ifnet_name()`／`ifnet_unit()`）；在那之前
  `Ath10kVifInterfaceName` 會是 `unresolved`。

---

### 6.15 第十一輪（v111–v115）：airportd 的請求通道是 **ifnet ioctl**，不是 user client

第十輪留下的三條路線（§6.14.4）在這一輪全部量過，結論是前三條都不需要——airportd 自己有一條
**ioctl 備援**，而它才是 v1 介面驅動該走的路。

#### 6.15.1 決定性證據：airportd 的 log 就寫著它在試 ioctl

把 `IOInterfaceName` 只放 vif（讓名字 match 命中 Skywalk 物件）之後，`IOServiceOpen` 依然失敗，
但 airportd 的 log 這次給出完整鏈條：

```
Apple80211BindToInterfaceWithService: ... IOServiceOpen failed with 0xe00002c2
Apple80211BindToInterfaceWithIOCTL: WARNING: Falling back to IOCTL path, ifname['en1'] err -1/0xffffffff
Apple80211BindToInterfaceWithService: FAILED to bind to interface - -1/0xffffffff
```

`-1` 不是「沒有備援」，是「備援被呼叫了、而介面沒有實作 `SIOCSA80211`」。先前 §6.14.4 第 3 條
「量測顯示沒有 fallback」的判讀是錯的：那只發生在 **我們自己的 user client 有回應** 的時候。

#### 6.15.2 為什麼 service 這條路即使通了也不能用

* 名字放 vif → `IOServiceOpen` 被 `IO80211VirtualInterface::newUserClient` 以
  `newUserClient: ERROR: invalid interfaceRole: 1 provided to newUserClient` 拒絕（它只收 6/7；
  而 `attachInterface` 只收 role 1，兩者互斥）。
* 從我們的介面節點把 open **轉給** `IO80211SkywalkInterface::newUserClient`（受器給 vif）
  可以成功（`newUserClient type=0 -> family client 0x0`，family 的 client 真的被建出來），
  但 airportd 一訂閱事件就 **panic**：
  `IO80211SkywalkInterface::createEventPipe + 0x1b`，`[ivars+0x208]` 是 null——
  那個位置只有「向 Skywalk stack 註冊過的介面」才有東西，我們註冊不了。
  **不要在無法完成 Skywalk 註冊的物件上把 family client 交給 airportd。**
  補證：掃描整份 live `IO80211Family.__TEXT`（capstone 逐指令，1.7 MB）**沒有任何** `mov [reg+0x208], …` 的寫入，所以那一格不是 family 自己填的，而是 Skywalk 註冊流程（或別的 kext 傳進來的物件）留下的——我們既然註冊不了，這條路就是關的。

#### 6.15.3 正解：實作 `performCommand` 的 80211 ioctl

`IONetworkInterface::if_ioctl` → `performCommand(controller, cmd, arg0=ifnet, arg1=data)`，
live 的 `IO80211Interface` **沒有** `performCommand`（只有 legacy 有），所以
`SIOCGA80211/SIOCSA80211` 一路掉到 `IOEthernetInterface::performCommand` → ENOTTY。
自己覆寫 `performCommand`，把請求轉給既有的 `venturaRecordRequest()` 就通了。

實作要點（`src/kext/ventura/Ath10kVenturaInterface.cpp`）：

1. `arg1` 是 **kernel 記憶體**，不是 user pointer。BSD 的 `ioctl()` 對 `_IOW/_IOWR` 指令會先把
   `IOCPARM_LEN(cmd)`（本輪實測 = 40）的請求結構複製進來，驅動直接 `memcpy` 讀即可；
   這裡用 `copyin` 會回 EFAULT，**請求永遠到不了 dispatcher**（v113 的 174 次呼叫全部卡在這）。
2. 真正在 user space 的是 `req.req_data`（payload 指標），get 要 `copyout`、set 要 `copyin`。
3. 指令碼兩組都要收：`0xc02869c9`/`0x802869c8`（size 40，pre-10.15 編碼）與
   `0xc03069c9`/`0x803069c8`（size 48）。13.7 的 airportd 實測用 **40 那組**。
4. 非 80211 指令要轉給 `IOEthernetInterface::performCommand`（KC 裡是 `N_SECT|N_EXT`，
   可以用 mangled 名稱直接呼叫；不要用 `super::`，那會虛擬派回自己）。

#### 6.15.4 v115 實測（本專案第一次有「請求」）

```
networksetup -getairportpower en1     -> Wi-Fi Power (en1): On          (以前 Off / Wireless is not available)
Ath10kApple80211Requests              -> 318                            (前十輪一直是 1)
Ath10kIoctlCalls                      -> 317
Ath10kIoctlType / Verb / Status       -> 19 (APPLE80211_IOC_POWER) / get / 0
ifconfig en1                          -> UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST
networksetup -listallhardwareports    -> Hardware Port: Wi-Fi / Device: en1
Ath10kNewUserClient                   -> 32（service 這條被我們主動拒絕，airportd 就轉 ioctl）
```

#### 6.15.5 又一個「live 沒有這個 virtual」

`venturaPublishState()` 原本呼叫 `_venturaInterface->setLinkState(...)`。live 的
`IO80211Interface` **沒有** `setLinkState`（只有 legacy 有），呼叫落到隔壁 slot，CPU 直接跳進
未映射位址：panic 的 `RIP == CR2 == 0xffffff8052c00000`，frame 顯示
`venturaDispatchRequest + 0x1b6`（其實是 `venturaPower()` 的返回位址）。
規則：**每個要呼叫的 Apple virtual，都先在 live KC 確認它存在**；不確定就只寫屬性。

#### 6.15.6 還沒做／下一步

1. **掃描與關聯**（§7.3）：請求通道有了，`APPLE80211_IOC_SCAN_REQ` 現在會進到
   `venturaScan()`（回 0 筆），所以選單還是空的。把 `src/kext/native/` 的 `_wmiScan`
   搬過來即可。
2. **系統設定的截圖**：VM 沒有 VNC 客戶端；auto-login 由 SSH 寫
   `/Library/Preferences/com.apple.loginwindow` 沒生效（`defaults read` 說不存在），
   主控台仍是 `loginwindow`（`stat -f%Su /dev/console` = root、`launchctl print gui/501` 失敗）。
   下一輪若要 GUI 證據，先解決這步（或直接在真機驗）。
3. `tools/io-match.c`（本輪新增）：重播 airportd 的 `IOServiceGetMatchingService` 字典，
   印出命中的節點類別與 `IO80211InterfaceRole`/`IOUserClientClass`，再自己 `IOServiceOpen`。
   注意它沒有 `com.apple.wlan.authentication` entitlement，**失敗是正常的**：
   `IO80211APIUserClient::initWithTask` 第一件事就是 `copyClientEntitlement`。

### 6.16 第十二輪（純量測，v115 不重編）：掃描的成果老早就在記憶體裡

為了排下一階段（掃描 → 密碼 → 企業），先把 v115 這台的 WMI/BSS 計數全部拉出來看，
結果發現**掃描不需要「攻破」，它已經在跑**：`Ath10kWMIScanAllChannelsObserved=Yes`、
`channelInfoCount=76`、`Ath10kBSSAccepted=252`、`Ath10kBSSCount=24`（容量 64）。
原因是 `serviceWMIScanChannels()` / `serviceWMIScan()` / `recordWMIBSS()` 都是 shared code，
Ventura 版本來就有編進去，只是 `venturaScanResult()` 還是 stub，沒有人把 `_wmiBSS` 端給 airportd。

四個判讀上的重點，避免下一輪重踩：

1. **不要重寫 WMI 掃描**。`_wmiScan` 是 shared 狀態機（credit/DMA/CE3 ownership 都在裡面），
   自己手刻 frame 會和 `_wmi.credits` 打架。
2. **`SCAN_RESULT` 是游標式 GET**：airportd 一次要一筆，回非 0 就停（itlwm 用 5/12 當結束碼，
   我們用 `kIOReturnNotFound`）。結果不是「陣列整包丟回去」。
3. **`asr_ie_data` 的形態要看 OS 世代**：SDK 標頭是 `void*`，但 12+ 有 inline 版本；
   先量 airportd 傳進來的 `req_len` 再決定，別照抄舊 itlwm。
4. **PSK 缺的是 passphrase→PMK（PBKDF2-HMAC-SHA256, 4096 輪）**，樹裡沒有；
   native 那條直接拿 `join.key` 當 PMK（只有 SAE 是真的算）。`Ath10kSHA256.hpp` 已有 HMAC-SHA256。

詳細三步計畫寫在 §7.3。

### 6.17 第十三輪（v116–v120）：S1 的驅動端做完並量到真正的牆 —— **airportd 收不到「掃描完成」**

#### 6.17.1 這輪新增的功能（`src/kext/ventura/Ath10kVentura.cpp`）

1. `venturaScan()` 現在會**重新 arm 共用的 WMI 掃描狀態機**（`attempted/submitted/txOwned/txConsumed/
   creditReturned/started/terminal/failed/completed/cancelRequested/stopSubmitted/stopConsumed` 全清、
   `visitedMask`/`channelInfo*`/`mgmtCount` 歸零、`generation++`、`id = 0xa000|((0x36+gen)&0xfff)`）。
   理由：`serviceWMIScan()` 是 one-shot（`completed` 就 return nullptr），不重新 arm 的話 airportd
   永遠只拿到開機那次掃到的 BSS。**不清 `_wmiBSS`**：`recordWMIBSS()` 本來就按 BSSID 就地更新，
   掃描進行中舊清單還能立刻服務（清掉反而讓 UI 空一輪）。
2. `venturaScanResult()` 改成**游標式**：一次回一筆，`ssidLength==0`（未知 SSID）跳過，
   指標用完回 `kIOReturnNotFound`（airportd 收到非 0 就停）。
   填的欄位：`asr_channel`（channel + 2GHz/5GHz/20MHz/ACTIVE flags）、`asr_noise`（firmware stats
   的 `noiseDbm`，沒有就用 -95）、`asr_snr`（WMI mgmt event 帶的 snr）、`asr_rssi = noise + snr`、
   `asr_beacon_int`、`asr_cap`、`asr_bssid`、`asr_ssid(_len)`、`asr_rates`（去掉 0x80 basic 位）、
   `asr_age = 0`。
3. **RSN IE 的形狀由 `req_len` 決定，不是由我們的標頭決定**：`asr_ie_data` 在 12.0+ 是緊接
   `asr_ie_len` 的 **1024-byte inline 陣列**（offset `0x8c`，`apple80211_scan_result` 沒有
   `asr_unk3`），舊 ABI 才是指標。`performCommand()` 把 `req.req_len` 寫進
   `_owner->_venturaIoctlLength`，只有在 `req_len >= 0x8c + ieLen` 時才寫 inline IE，
   避免寫出呼叫端緩衝區。檔頭有三個 `static_assert` 釘住 `asr_rates 0x24`、`asr_ssid 0x61`、
   `asr_ie_len 0x8a`。
4. **掃描 dwell 縮到 110 ms**（`Ath10k::kWMIScanAirportdDwellMs`，只在 `ATH10K_VENTURA` 分支傳給
   `buildWMIScan`）：airportd 一次掃描只給約 20 秒，38 個通道 × 500 ms 一定超時。
5. **`SUPPORTED_CHANNELS` 從 0 改成 38 個實際通道**（`kWMIScanPlan`）。這是關卡之一：
   `num_channels=0` 時 CoreWLAN 問完 POWER/BSSID/ROLE 就直接回空清單，**連 SCAN_REQ 都不送**。
6. **`VIRTUAL_IF_ROLE`(96) / `VIRTUAL_IF_PARENT`(97) 回 success（零填）**。live family 是在自己的
   vif 簿記裡回答這兩個，而 ioctl 通道根本不經過那段程式；先前回 `kIOReturnUnsupported` 時，
   CoreWLAN 問到 96 就放棄（`g27=0 g9=0 g19=0 g96=e00002c7` 後停止）。改完之後 SCAN_REQ 才真的送出。
7. `Ath10kRequestTrace`：控制器上留最近 16 筆 `(verb,type,status)`，用來看「客戶端問到哪一筆就停」。

#### 6.17.2 實測（v120，Acer `/tmp/vk120`，sha256 `953b577f…`）

驅動端（`tools/apple80211-scan.c`，自己走 `SIOCSA80211`/`SIOCGA80211`，不經 CoreWLAN）：

```
SCAN_REQ accepted on en1, waiting 6s
 1 ssid='Jane'         bssid=50:3d:d1:d7:b9:67 ch=3 flags=0x8a rssi=-74 noise=-95 snr=21 cap=0x1c11 rates=12 ie=22 (inline-rsn)
 2 ssid='142-1F-2.4G'  ... ch=1 ... ie=22 (inline-rsn)
 ...
15 ssid='AndroidAP_5078' ... ch=2 ... ie=22 (inline-rsn)
SCAN_RESULT end after 15 results (Input/output error)
total=15 with-rsn=15
```

斷點：`Ath10kScanRequests=8`、`Ath10kScanRearms=8`、`Ath10kScanServed=15`、`Ath10kScanEnds=1`、
**`Ath10kScanCompletedMs=2514`**（掃描 2.5 秒完成，之前 19 秒）、`Ath10kScanReqLen=2048`（自己的工具；
airportd 來的是 5456 → 它用的是 `apple80211_scan_multiple_data` 那條）。

airportd 端（`tools/cwscan.m`，CoreWLAN → airportd → 我們）：

```
cwscan: interface=en1 power=1 → scan failed: nil   (耗時 84 秒 = 4 × 20s 重試)
airportd log: Scan: Perform scan for pid 511 (cwscan) ...
              performScanWithChannelList:...: Failed to perform Wi-Fi scan, returned error code -3905
```

也就是：**airportd 確實把 SCAN_REQ 送到我們這裡**（`Ath10kIoctlType=10`、`Ath10kLastApple80211Request=10`、
`ScanRequests` 跟著長），掃描也真的跑完（2.5s），但 airportd 從頭到尾**沒有發過任何
`SCAN_RESULT`(11) 請求**，20 秒一到就 -3905（`0xfffff0bf`）放棄並重試。

#### 6.17.3 量到真正的牆：`[vif ivars + 0x208]`（event pipe）

`IO80211SkywalkInterface::createEventPipe(IO80211APIUserClient *)`（live `0x200128c`）反組譯：

```
rdi = [this+0x110]            ; ivars
rdi = [rdi+0x208]             ; ← 這裡 null 就直接炸（v112 的 panic 就是它）
eax = [[rdi]+0x130](rdi)      ; 某個計數/容量
if (eax < 0x18) → log + 回 kIOReturnError
pipe = IO80211IOUCEventPipe::withClient(client, (role==6 ? 0x4b000 : 0x4000))
[[[this+0x110]+0x208]+0x1b8](that, pipe)   ; 把 pipe 掛上去
```

本輪把它變成可量的斷點：`Ath10kVifEventPipe`（在 `attachPlatformVif()` 讀 `[[vif+0x110]+0x208]`）
**= 0**，`Ath10kScanEvent = "no-event-pipe"`。另外掃過整個 KC 的 `__TEXT`（capstone 逐指令）找
`mov [reg+0x208], reg`：**0 命中** —— 這一格不是任何 kext 的程式碼寫的，是物件建構／Skywalk
註冊流程留下來的，跟 §6.15.2 的結論一致。

**結論（可重現）**：v1 介面＋live family 的 ioctl 通道能拿到請求、也能把結果備好，但**沒有任何方式
告訴 airportd「掃描結束了」**：
- `IO80211Interface::postMessage` 在 live 那份**不存在**（符號只有 legacy 0x218ba36 一份），
- 走 `IO80211Controller::postMessage`（live `0x1fef46a`，EXT）最後也是落到 vif 的 event pipe →
  `+0x208` 是 0（所以程式裡先檢查，非 null 才呼叫，目前記錄 `no-event-pipe`），
- 事件要送到 airportd 又需要 family 的 user client（`IO80211APIUserClient::commonStart` 第一行 cast
  到 `IO80211SkywalkInterface`，v1 介面永遠過不了，§6.12.6）。

因此 S1 的**最後一哩是 Skywalk 註冊**（`registerNetworkInterfaceWithLogicalLink` + packet buffer
pool，§6.12.8 第 4 點）：註冊完成後 `+0x208` 才有東西，事件（或 family user client）才成立。
在那之前，UI 的掃描清單、`networksetup -setairportnetwork`（本輪實測：airportd 直接回
`Could not find network`，因為它的 scan cache 是空的）都會卡在同一個門。

#### 6.17.4 這輪新增的工具

- `tools/apple80211-scan.c`：guest 內 `cc -o /tmp/apple80211-scan apple80211-scan.c`，
  `./apple80211-scan en1 [wait]`。直接對 ifnet 下 `SCAN_REQ`、再用游標把 `SCAN_RESULT` 全撈出來，
  印 ssid/bssid/ch/rssi/snr/rates/ie 長度。**驗證掃描結果服務就靠這支，不需要 airportd 配合**。
- `tools/cwscan.m`：`clang -fobjc-arc -framework Foundation -framework CoreWLAN -o /tmp/cwscan cwscan.m`，
  `./cwscan en1`。走 CoreWLAN → airportd 的真實路徑，用來看 airportd 這邊成不成功。

#### 6.17.5 下一輪的起點

只有一條：**讓 vif 有 Skywalk 註冊**（或等價地讓 `[ivars+0x208]` 有物件）。判斷成功的最短量測是
`Ath10kVifEventPipe != 0`；一旦非 0，`Ath10kScanEvent` 會變 `posted-scan-done`，airportd 就應該
開始拉 `SCAN_RESULT`（`Ath10kLastApple80211Request` 變成 11、`Ath10kScanServed` 跟著長）。
在註冊成功之前不要再去試 user client / infra attach 的變體，也不要靠猜測改 SCAN_RESULT 的格式
—— 驅動端已經用 `tools/apple80211-scan.c` 證明是對的。

### 6.18 第十四輪（v121–v126）：Skywalk 註冊那格填上了 —— 事件通道成立，但 airportd 的掃描握手還差一步

#### 6.18.1 找到 `[ivars+0x208]` 的寫入者（本輪最大的發現）

上一輪量到 `[vif ivars+0x208]`（event pipe 陣列）= 0，並推論「沒有任何 kext 寫它」。**推論方向對，位置錯**：
寫入者在 `IO80211SkywalkInterface::start(IOService*)`（live `0x200043a`）：

```
0x0488 mov edi, 0x18
0x048d call OSArray::withCapacity(0x18)
0x0492 mov rcx, [r12 + 0x110]        ; ivars
0x049a mov qword ptr [rcx + 0x208], rax   ; ← event pipe 陣列
```

而且 `IO80211VirtualInterface::start(provider)`（live `0x201627e`）本體是
`pop rbp; jmp IO80211SkywalkInterface::start`——純 tail call。也就是說：
**`createVirtualInterface()` 只 init()，從來沒有 start()，所以那一格永遠是 0。**

修法（v121）：`attachPlatformVif()` 在掛 infra 介面之前先呼叫 Apple 自己的 start()：

```cpp
extern "C" bool ath10kVifStart(IO80211VirtualInterface *self, IOService *provider)
    __asm("__ZN23IO80211VirtualInterface5startEP9IOService");
const bool vifStarted = ath10kVifStart(vif, this);
```

同時：`attachInterface` 要求角色 ivar `[ivars+0x50] == 1`，而
`IO80211VirtualInterface::newUserClient` 只接受 6/7（其餘 IOLog `invalid interfaceRole` 並回
`kIOReturnBadArgument`）。所以流程是：**先用 1 attach，attach 完再寫回 7**。

#### 6.18.2 v121 之後的量測（全部是 breakpoint，不是推論）

| 斷點 | v120 | v121+ |
|---|---|---|
| `Ath10kVifStart` | — | `ok` |
| `Ath10kVifEventPipe` | 0 | 非 0（核心指標） |
| `Ath10kFamilyClient` | — | `created`（family 的 IO80211APIUserClient 真的建起來） |
| `Ath10kInfraAttach` | ok | `ok` |
| `ioreg -c IO80211APIUserClient` | 0 | 8–13 個 |
| `Apple80211GetIfListCopy` | **-3903 / ifCount[0]** | **0 / ifCount[1]**（介面被接受） |
| `Ath10kEventPipes` | — | **2**（真的有 2 個 client 掛了 event pipe） |
| `Ath10kScanEvent` | `no-event-pipe` | **`posted-scan-done`** |

`networksetup -getairportpower en1` 也從 Off 變成 **On**（把 `_venturaPowerOn` 預設改成 true：
radio 開機就是開的，讓 POWER get 從第一次就答 On；否則 airportd 自己認定 Wi-Fi 關著，永遠不發掃描，
而它想設 ON 時又被 family 的 `setPower` 擋掉）。

#### 6.18.3 family 的請求是怎麼進來的（v124 破解）

airportd 走 family client 之後，請求不再走 ifnet ioctl，而是進到
`IO80211Controller::apple80211_ioctl(IO80211SkywalkInterface*, ...)`（live vtable slot 406，offset `0xcb0`）。
反組譯 Apple 這份實作（`0x1febf98`）發現它其實只認兩個 cmd：

```
mov eax, 0xc03069c9 ; cmp rdx, rax ; je → eax=0x1ad   (get)
mov eax, 0x803069c8 ; cmp rdx, rax ; jne → return 0x66 (EOPNOTSUPP)
```

而且 **同一個 slot 有兩種呼叫慣例**（family 自己前後不一致）：
- getter 路徑：`(iface, 未用, cmd, req_block)`
- `setPower` / `setScanRequest`：`(cmd, type, iface, payload)`

因此 v124 把這個 override 改成 4 參數並兩種都認（見 `Ath10kVentura.cpp` 的 `apple80211_ioctl`），
把 `apple80211req`（`if_name[16]; int type; int val; u32 len; void *data`）解出來丟進
`venturaRecordRequest()`；不認得的情況才回 Apple 的實作。**效果：SCAN_REQ 從 102/EOPNOTSUPP 變成
真的進到 driver**（`Ath10kIoctlReqType=10`、`Ath10kLastApple80211Request=10`、`Ath10kScanRequests` 成長）。

#### 6.18.4 還差的那一步（下一輪直接從這裡開始）

現況：SCAN_REQ 進得來、掃描 2.5 秒跑完、結果可用游標協議供應、SCAN_DONE 也真的 post 進 2 條 pipe，
**但 airportd 仍然 20 秒後 -3905 放棄，而且從頭到尾沒發過一次 `SCAN_RESULT`(11) 請求**。

已排除：請求通道（通了）、通道清單/角色 getter（補了）、dwell 太慢（2.5s）、沒有 pipe（有 2 條）、
事件沒送（送了 M_SCAN_DONE + M_SCAN_CACHE_UPDATED，帶 payload）。

因此下一個假設是**family 的掃描管理器**：`setScanRequest`（live `0x1fe2318`）在
`IO80211Controller::getPrimaryInterfaceScanManager()` 回 null 時直接回 0x66，非 null 才走
`IO80211ScanManager::startScan`。它要的物件有現成 initializer：
`IO80211ScanManager::initWithControllerAndSkywalkInterface(IO80211Controller*, IO80211SkywalkInterface*)`
（`0x2010568`，內部會 `commonInit(controller, service)`）。下一輪要做的就是把這個 manager 建起來、
塞進介面那個 getter 讀的 ivar，讓 family 自己完成「掃描完成 → 通知 airportd → airportd 拉
SCAN_RESULT」的握手。判斷成功的斷點：`Ath10kLastApple80211Request` 變成 11、`Ath10kScanServed` 成長。

#### 6.18.5 這一輪的檔案變更

- `src/kext/ventura/Ath10kVentura.cpp`：`ath10kVifStart` + start/角色 1→7 流程、
  `venturaEventPipeCount()`（讀 `[+0x208]+0x130` 的 pipe 數 → `Ath10kEventPipes`）、
  `runScanNotify()` 改為同時用 controller 與 vif 兩條路徑 post、兩種慣例的 `apple80211_ioctl` 分派。
- `src/kext/ventura/Ath10kVenturaDecl.inc`：`_venturaPowerOn = true`、ioctl override 改 4 參數。
- `vendor/itlwm-v1/Airport/IO80211Controller.h`：`apple80211_ioctl(IO80211SkywalkInterface*,unsigned long,void*,void*)`
  的 arity 依 KC 實測改成 4（名稱沿用，slot 位置不變）。

### 6.19 第十五輪（v127）：牆的真正名字 —— **掃描快取住在 `IO80211ScanManager`，而那個物件我們拿不到**

#### 6.19.1 決定性證據（兩支 family handler 都反組譯完了）

`IO80211Controller::getPrimaryInterfaceScanManager()` 是最後一根柱子，**掃描請求與掃描結果都靠它**：

`setScanRequest`（live `0x1fe2318`）：
```
req->req_len == 0 → 0x16 ; req->req_data == null → 0x16
rax = getPrimaryInterfaceScanManager()
if (!rax) → return 0x66            ← EOPNOTSUPP，airportd 早期看到的 102
jmp IO80211ScanManager::startScan(rax, req->req_data)
```

`getScanResult`（live `0x1fdc76e`）：
```
req->req_len 必須 == 0x48c         ← 1164 = sizeof(apple80211_scan_result)，正好驗證 §6.17 的 inline IE 佈局
rbx = req->req_data
rax = getPrimaryInterfaceScanManager()
if (!rax) → 錯誤路徑（airportd 永遠拿不到結果）
IO80211ScanManager::getScanResult(&result, &ts) ; memcpy(req_data, result, 0x48c)
```

而 `IO80211SkywalkInterface::getScanManager()`（live `0x20037a8`，vtable index 373 / offset `0xba8`）本體是：

```
xor eax, eax
ret            ← 無條件回 null
```

`setScanningState`（index 371 / `0xb98`）也只是 IOLog 的樁。**結論：live family 把掃描快取放在
scan manager 裡，driver 不再「按需求送 SCAN_RESULT」，而是由 manager 收送；而 vif（Apple 的
`IO80211VirtualInterface`）的 `getScanManager()` 永遠回 null，我們又不能用子類覆蓋它。**
`ioreg` 的 `IOKitDiagnostics` 也印證：`"IO80211ScanManager"=0`（整個核心沒有任何實例）。

這解釋了第十五輪所有量測：SCAN_REQ 我們自己服務會成功（v124 起 `Ath10kIoctlReqType=10`、
`ScanRequests` 成長），但 airportd 之後的完成/取結果握手全在 manager 裡，manager 不存在 → 20 秒 -3905，
且 `Ath10kScanServed` 永遠 0（它從沒向我們要 type 11，因為 type 11 是 family 內部處理掉/失敗掉）。

#### 6.19.2 已排除的路（不要再重試）

- 補 `SUPPORTED_CHANNELS` / `VIRTUAL_IF_ROLE` / `PARENT`：已做，只讓 SCAN_REQ 送得出來。
- 加快掃描（110ms dwell，2.5 秒掃完）：已做，仍超時。
- 事件通道：`Ath10kVifEventPipe` 非 0、`Ath10kEventPipes=2`、M_SCAN_DONE + M_SCAN_CACHE_UPDATED
  三種送法（vif 廣播、controller fan-out 帶 flag、payload 有無）都試過，airportd 仍不動。
- 直接建 `IO80211ScanManager`：`initWithControllerAndSkywalkInterface(controller, skywalkIface)`
  存在（`0x2010568`），但它 init 時會對 driver 打 `cmd=0xc03069c9 / type=0x0c(CARD_CAPABILITIES)`
  並要求回傳 caps 第 6 byte 的 bit 0x40（capability 索引 22）；而且**沒有任何人把 manager 寫回介面**，
  `getScanManager()` 又是硬 null，所以「建了也接不上」。

#### 6.19.3 下一輪只剩兩條路（都要動架構）

1. **讓 infra 介面是 `IO80211InfraInterface`（而不是 vif）**：`IO80211InfraInterface` 是 Apple 為
   role 1 準備的子類，很可能覆寫了 `getScanManager()` 並持有 manager。歷史紀錄說直接 new 會卡在
   metaclass（§5.11），但值得先確認 family 有沒有現成 factory（`createVirtualInterface` 只給 role 7）。
2. **自己實作掃描快取與握手**：既然 `setScanRequest`/`getScanResult` 都因 manager 缺席而失敗，
   就必須讓 airportd 走另一條「driver 自己服務」的路徑 —— 目前唯一已知可行的是 v120 的純 ifnet ioctl
   通道，但那條沒有事件通道，airportd 一樣會 -3905。除非找到「ioctl 通道 + 事件」的組合，
   否則這條不成立。

判斷進度的斷點仍然是：`Ath10kLastApple80211Request` 變成 11、`Ath10kScanServed` 成長。

## 6.35 第三十輪：join 打通（驅動自行關聯）

### 6.35.1 `-3903` 的最終定位（userspace 白名單）
- `Apple80211Set` 本體（entry 是 10-byte thunk，本輪開機在 `0x7ffb1eb0856e` → 本體 `0x7ffb1eb41df7`）
  把 type 透過兩段式表分派：小 type（≤0x1b）先過位圖 `0x08089000`（只有 12/15/19/27 被放行），
  其餘落到 `0x7ffb1eb43b11`：拿 `OS_os_log` 句柄（已用 `peek` 讀出是 `<OS_os_log: …>`）、記一條 log、
  然後直接 `mov r15d, 0xfffff0c1` 回 `-3903`。**type 20 就是走這條**。
- 另一條 service 路徑（dispatcher `cmp byte ptr [ref+0x5d],0` 非 0 時 `jmp …e8df7`）同樣不吃 type 20。
- 因此 airportd 不可能送出 `ASSOCIATE(20)`；`-3903` 不是驅動回的，也不是 caps/AUTH_TYPE 的問題
  （那兩個 bug 本輪仍修掉並保留）。`Apple80211BindToInterfaceWithService` 對非 airportd 程式回 `-3900`
  （要 `com.apple.wlan.authentication`），所以也無法在自製程式裡重現 service 路徑。

### 6.35.2 解法：驅動自己 join
- `venturaScheduleAutoJoin()`：`thread_call_allocate_with_options(..., THREAD_CALL_OPTIONS_ONCE)`，
  start 後 15 秒觸發；`runVenturaAutoJoin()` 必要時先 `venturaPower(true)`，組
  `apple80211_assoc_data`（`ad_auth_lower=OPEN`、`ad_auth_upper=WPA2_PSK`、SSID、key）後呼叫
  既有的 `venturaAssociate()`，把結果寫進 `Ath10kAutoJoinPhase`。
- 看門狗：無線開著就永遠重排（已連線 30 秒一次、未連線 10 秒一次），`venturaPower(true)` 也會重新
  啟動它 → 關/開 Wi-Fi、AP 重啟都能自癒（實測 15 秒內回到 `connected-wpa2`）。
- 憑證：`src/kext/ventura/Ath10kAutoJoin.inc`（`ATH10K_AUTOJOIN_SSID` / `..._PSK`），由建置前產生、已列入
  `.gitignore`；換測試網路只需重新產生該檔並重建。

### 6.35.3 SSID 回報（可疑的 ABI 已解）
- 用 `apple80211_ssid_data{version, ssid_len, ssid_bytes}` 回 IOC_SSID 時，`networksetup`、`wdutil`、
  System Settings 都把 version 的低byte 當成網路名（`Current Wi-Fi Network: \u0001`）。
- 改成平坦佈局：offset 0 起 32 bytes 放 SSID、offset 32 放長度 → 實測 `QCA-TEST` 正確顯示。

### 6.35.4 驗證（v200 / v199 同一二進位，含冷開機）
`Ath10kConnectionState = connected-wpa2`、`Current Wi-Fi Network: QCA-TEST`、DHCP `192.168.77.75`、
`ping 1.1.1.1` 3/3（6–19 ms）、`curl https://example.com` 200、DNS 解析正常；冷開機與電源循環後
皆自動恢復。Wi-Fi 單獨可用也驗過：`ping -S 192.168.77.75 1.1.1.1` 3/3、`curl --interface en1`
回 200（local_ip=192.168.77.75）、`nc -z 1.1.1.1 443` 成功。驗收表 A–D 仍成立；E（實機）依使用者
指示暫緩。

---

## 6.36 第三十一輪（同日）：P-256 在核心裡真的能算、SAE/PMF 進 Ventura、開機 panic 收斂

- **P-256（SAE 的地基）已可用**：`Ath10kP256KernelBinding` 原本綁 `cczp_n`/`cczp_prime`，這兩個是 macro
  不是符號（KC 裡不存在）。改成讀 `cczp` 欄位後仍失敗——實測 `ccec_cp_256()` 回傳的物件第一個 word
  是 limb 數（4），接下來是 `0x100` 與一個 kernel pointer，**不是** header 描述的 `{di,n,prime}`。
  由於 `P256FieldCandidate::run()` 只用 `units`/`prime` 做驗證（真正的運算走 `cczp_mul(curve,…)`），
  直接回常數 `units=4`、`prime= P-256 modulus` 即可。`ccn_read_uint` 另需 adapter（真實原型吃
  `const uint8_t*`，shared API 的欄位是 `const void*`）。**v208 實測 `Ath10kP256SelfTest = mul-ok`**
  （2·3 mod p = 6），且 WPA2 連線不受影響。
- **SAE/PMF 目標檔已進 Ventura**：`Ath10kSAEWorker/Ath10kSAESession/Ath10kPMFSession` 併入
  `VENTURA_OBJECT`，定義 `-DATH10K_P256_DIAGNOSTIC_BINDING=1`；`strings` 可見到
  `Ath10kSAESession`/`Ath10kSAEJob`/`Ath10kSAEPrepareJob`/`Ath10kPMFSession`/`Ath10kPMFJob`。
  **尚未接線**：`venturaAssociate()` 仍把 `ad_auth_upper` bit12（SAE）當 PSK 處理，缺
  commit/confirm 管理框交換與 `beginEAPOL`/`copyPMK` 流程。
- **開機 panic 收斂**：三份 panic 都在 `publishVenturaVifBsdUnit +0x685` →
  `IOSkywalkNetworkInterface::prepareBSDInterface +0x1a`（CR2=0x4c，vif 沒有 Skywalk extra）。
  en1 不需這條接手即可運作，改成 `ath10kvifbsd=1` 診斷專用，平常回報
  `Ath10kVifBsdPhase=skipped-guarded`；v204 起多次重啟未再 panic。
- **多網路**：開機參數 `ath10kjoin=ssid:psk,ssid2:psk2`（≤4 組），改 config.plist 即可換網路；
  VM 實測 `Ath10kJoinList = boot-args entries=2 first=QCA-TEST` 仍正常連線。
- **真機 ESP 已部署**（`nvme0n1p3`）：kext v204、`Kernel/Add … Enabled=True`、boot-args 帶
  `ath10kjoin=…`；備份在 `/root/ath10k-real-esp-backup-20261126-084903`。**尚未重開機驗證**。

---

## 6.37 第三十二輪：為什麼「不能隨點連線」——完整證據鏈

### 6.37.1 兩條 Set 路徑，只有一條吃 type 20
`Apple80211Set`（使用者空間 `IO80211.framework`，entry 是 10-byte thunk，本次開機
`0x7ffb1eb0856e` → 本體 `0x7ffb1eb41df7`）在把請求送進 kernel 前先做型別分派：

| type 範圍 | 走法 |
|---|---|
| ≤ 0x1b 且在位圖 `0x08089000` 內（只有 12/15/19/27） | 快速路徑，直接轉發 |
| 0x60 / 0x61 | 個別處理 |
| **其餘（含 20 ASSOCIATE）** | `0x7ffb1eb43b11`：`mov rdi,[OS_os_log]; mov esi,0x11; call log; mov r15d,0xfffff0c1; … jmp epilogue` |

⇒ **-3903 是 userspace 直接回的**，不是 family、更不是驅動。實測：我們自己寫 40 行小程式呼叫
`Apple80211Set(ref, 20, …)` 一樣拿到 `-3903`，且驅動端請求計數不動（`Ath10kApple80211Requests` 只 +1，
那是背景 airportd 的）。

另一條 service 路徑（dispatcher 開頭 `cmp byte ptr [ref+0x5d], 0`，非 0 時 `jmp 0x…be8df7`）才是
airportd 走的路；`Apple80211BindToInterfaceWithService` 對非 airportd 的程式回 `-3900`（要
`com.apple.wlan.authentication`），所以無法在自製程式裡重現。

### 6.37.2 結論與兩條可行路線
1. **關 SIP + 改 framework**：唯一能讓「系統設定點選任意網路」真正走 airportd 的辦法，但要動
   Apple 的 dyld 快取／注入 dylib，等於放棄原廠簽章與更新。
2. **驅動自連（目前採用）**：`ath10kjoin=ssid:psk,…` 開機參數 + 看門狗，開機自動連、關開 Wi-Fi
   會自癒、換網路改 boot-args 即可。日常可用，但不是「隨選」。

### 6.37.3 本輪追加的實測（4 項，含推翻）

1. **service 路徑也不吃 type 20**（推翻 §6.37.3 原本的假設）。本輪開機的 service 入口
   `0x7ffb19afedf7`（`Apple80211Set` 本體 `0x7ffb19ad81b3`、entry `0x7ffb19ac556e`）反組譯後
   與 ioctl 路徑結構相同：`mov ebx,[rsi+0x10]` → `cmp ebx,0x73; jle` → ≤0x1b 過位圖 `0x8089000`
   → 其餘（含 20）落到 `0x…b00b11` 的 `mov r15d,0xfffff0c1` 尾巴。**兩條路都不支援用 Set 送
   ASSOCIATE**，所以真正的 join 一定是 `Apple80211Associate2`（它自己組 `apple80211_assoc_data`，
   不走 Set(20)）。這也解釋了為何自製程式呼叫 Associate2 只拿到純 userspace 的 -3903、errno=0。

2. **我們的 user client 其實開得成功**：`Ath10kFamilyClient = created`、
   `Ath10kVifEventPipe = 18446743613723228192`（非 null）、`Ath10kUserClientAttempt = 2`、
   `Ath10kNewUserClientType = 0`、節點 `IOUserClientClass = IO80211APIUserClient`
   （`Ath10kVenturaInterface::newUserClient` 轉呼叫 family 的 `IO80211APIUserClient`）。
   ⇒ 原始碼註解裡那行 `IOServiceOpen failed 0xe00002c2` 是舊輪次的記錄，不是現在。

3. **「隨點」在 v212 的實際序列**（`networksetup -setairportnetwork en1 QCA-TEST`，等於系統設定）：
   `… g12=0 s22=0 g254=0 g19=0 g15=0 g15=0 g1=0 g103=0 g1=0` 然後結束。
   `s22` 是 DISASSOCIATE，之後全是 getter，**沒有 `s20`**；`Ath10kLastApple80211Request = 1`
   （最後一個是 SSID get）。錯誤仍是 `-3903`。

4. **airportd 的 logger 起不來**，所以 wifi.log 沒有 join 細節：
   `_initInterface: Unable to initialize logger` / `unified logger` / `driver message tracer monitor`
   每次開機都印。要在日誌層面看原因，得先讓那個 driver message tracer 成功（猜測與 corecapture
   的 CCLogStream / `getLogPipes` 有關）。

**因此下一步只有一條主線**：把 `Apple80211Associate2` 的成功路徑反組譯出來（它在 +0x2500 之後
才真正送出請求），找出它用哪個 user-client selector 送 assoc、以及我們的 family client 是否回
`0xe00002c2`；同時把 airportd 的 logger（driver message tracer）修起來，之後每次隨點都能在
wifi.log 看到它自己的理由。

---

## 7. 下一個接手的人：照這個順序

### 7.1 重疊的發布路徑（**v52 已完成**）
`super::start()` 現在會跑完（`Ath10kAppleStart=ok`），而且**它自己就發布了 BSD 單元**。所以 `Ath10kPCIDevice.cpp` 的手動 create→configure→attach→蓋 71→register 整段現在只在 `!appleStart` 時才跑，`Ath10kPublisher` 會寫 `apple`。待確認（下一次開機隨手看）：
```bash
ioreg -r -c IO80211Interface | grep -c '+-o'          # 應該只有 Apple 那一顆（+1 顆 vif）
networksetup -listallhardwareports | grep -A2 -i wifi # 目前仍然沒有
```
**注意**：§6.7 已證明 Wi-Fi 硬體埠消失不是「多顆介面」造成的，是 SC 根本看不到這個介面。

### 7.2 請求入口（**v115 已完成**，走 ifnet ioctl）

airportd 不是靠 family 的 user client，而是 `SIOCGA80211/SIOCSA80211` → `performCommand` 這條
ioctl 備援（§6.15）。`Ath10kApple80211Requests` 現在會長、`getairportpower` 回 `On`。
下一輪直接從 §7.3 的掃描/關聯開始，不要再碰 user client 或 Skywalk。

以下為當時的假設，僅留記錄（已被 §6.15 取代）：

### 7.2-old 接上 airportd 的 infra 請求入口

§6.9/§6.10 已經完成：介面在 SC 裡是 Wi-Fi 硬體埠、`IO80211Interface::init` 走正規、controller 的 `+0x200` 介面指標補上（`Ath10kCtrlInfraInstalled = retry`）。airportd 仍回 `Wireless is not available`，最強假設是**請求入口沒接**：

1. **實作 infra 入口**：在 `Ath10kPCIDevice` 覆寫 live IO80211Family 的
   `apple80211_ioctl(IO80211Interface *, IO80211VirtualInterface *, ifnet_t, u_int, void *)`
   （必要時連 `apple80211_ioctl_get/set(IO80211Interface*, IO80211VirtualInterface*, ifnet_t, void*)` 一起），把 request 轉給既有的 `venturaDispatchRequest()`。用 KC 的 live vtable 位移驗證槽位，不要靠 vendored header 的順序（§6.5 的教訓）。
2. **驗收**：`Ath10kApple80211Requests` 從 1 開始持續增加（airportd 會連發 CARD_CAPABILITIES/POWER/STATE/SUPPORTED_CHANNELS）、`ioreg` 出現 airportd 對 IO80211 開的 user client、`networksetup -getairportpower en1` 回 `On/Off` 而不是 `Wireless is not available`。
3. 若 infra 入口接了仍不認：把 `getCARD_CAPABILITIES` 的位元改成 AirportItlwm 的填法（`/tmp/itlsrc/AirportItlwm/AirportSTAIOCTL.cpp:667`，逐 bit 對 `APPLE80211_CAP_*`，不要用我們的 `bits[0]=0x0f` 自創格式）。

### 7.2-old 讓 airportd 拿得到裝置（**已被本節取代，僅留記錄**）

§6.9 已經把「介面在 SC 裡是 Wi-Fi」這件事做完（B ✅）。剩下的門檻是 airportd：它透過 `IO80211Controller::getNetworkInterface()`／`getInfraInterface()`（兩者都只是讀 `[this+0x200]`）拿介面，而這個 ivar 是 null。

1. **先釐清 pointer 從哪來**：在 `createInterface()` 裡掛 `Ath10kCreateIfaceCalls++` 與 `Ath10kCreateIfacePtr`（寫到 provider），確認 Apple 的 `start()` 到底有沒有呼叫我們的 `createInterface()` override。本輪的 `Ath10kCtrlInfraSeen = No` 表示連 `createInterface()` 寫的 file-static 都讀不到，這一點比 ivar 本身更可疑（可能是我們自己 ivar 區與 live `IO80211Controller` 的 ivar 區重疊，`_venturaInterface` 實測在 +0x668）。
2. **在 Apple 的 `start()` 之後（`Ath10kPublisher=apple` 已確定）寫 `[this+0x200] = <published interface>`**，並且只寫一次、寫之前確認它是 null。**不要在 `createInterface()` 裡寫**（§6.9.3 #3：會讓 `attachInterface()` 直接 return，Wi-Fi 硬體埠消失）。
3. **驗收**：`networksetup -getairportpower en1` 不再是 `Wireless is not available`、`ioreg` 出現 `pid 210, airportd` 對 IO80211 開的 user client、`Ath10kApple80211Requests` 開始增長（airportd 會持續送 POWER/STATE/CAPABILITIES）。
4. 若 ivar 還是寫不進去／寫了 airportd 仍不認，改用 `attachInterface((IONetworkInterface **)&iface, true)` 這條 Apple 自己會寫 ivar 的聚合入口，並且**先關掉 Apple `start()` 的介面發布**（否則會多一顆介面，v51c 的教訓）。AirportItlwm 就是這條路（§6.9.4）。

### 7.2-old 讓 BSD 單元被 80211 核心／networkd 認可（**已被 §6.9 取代，僅留記錄**）
四輪實測（§6.8）已經把所有「借別人的 ifnet」的路封死，所以：

1. **自己生 ifnet**：`ifnet_allocate(&params, &ifp)`（`params.type = IFT_IEEE80211` 從出生就對；欄位抄 `IONetworkInterface::attachToDataLinkLayer` 的填法，見 §6.8 第 3 點）、`ifnet_set_lladdr`、`ifnet_attach(ifp, &sadl)`。**注意** `ifnet_find_by_name`/`ifnet_list_get` 在本 kext 不能用，所以建立後要自己保存指標。
2. **交給 vif**：`vif->setUnitNumber("en")`（index 455）→ `prepareBSDInterface(ifp, 0)`（285，會把 ifp 寫進 `ivars+8`）→ `configureIfnet()`（457）→ `attachToBpf()`（456）。`publishVenturaVifBsdUnit()` 已經有這些呼叫，只要把「怎麼取得 ifnet」那段換成自己 allocate 即可；失敗時 `Ath10kVifBsdSource`/`Ath10kVifAttachPhase` 會指出卡在哪一步。
3. **驗收**：`tools/sc-interfaces.c` 必須列出這個單元；接著 `networksetup -listallhardwareports` 應出現 Wi-Fi、`networksetup -getairportpower en1` 不再是 `Off (forced)`。若單元出來了但 SC 仍看不到，下一步查 `IONetworkMbufDMADrivers`（§6.8 唯一「en0 有、我們沒有」的差異）。


### 7.3 掃描 → 密碼驗證 → 企業驗證（下一階段的三步）

#### 7.3.0 先看清楚：掃描其實**已經在跑**

v115 的 ioreg（不是推論，是實測）：

| 觀察點 | 值 | 意義 |
|---|---|---|
| `Ath10kWMIScanChannelsTransportSucceeded` | `Yes` | `SCAN_CHAN_LIST` 已送出並被 firmware 收下 |
| `Ath10kWMIScanAttempted` / `AllChannelsObserved` | `Yes` / `Yes` | 掃描真的跑過，76 個 channel-info 事件 |
| `Ath10kBSSAccepted` / `Ath10kBSSCount` | **252 / 24** | `_wmiBSS` 裡現在就有 24 個 AP（容量 64） |
| `Ath10kBSSBeacons` | 299 | beacon 有進來（mgmt 事件路徑正常） |
| `Ath10kBSSMalformed` / `LastReject` | 47 / `ds-parameter` | 有 47 個 frame 被 IE 檢查擋掉，先不動 |

也就是說：**WMI 掃描、BSS 解析、channel 清單全部是 shared code 既有功能**
（`serviceWMIScanChannels()` / `serviceWMIScan()` 在 `Ath10kPCIDevice.cpp`，
`recordWMIBSS()` 在 `Ath10kBSS.hpp`），Ventura 版本來就有編進去。缺的只是把
`venturaScan()` / `venturaScanResult()` 這兩個 stub 接上去。

#### 7.3.1 第一步：讓 airportd 看得到掃描結果（S1）—— **v149 已完成**

現況：`cwscan en1` 回 14–16 個真實網路、airportd `Completed scan … error=0`（§6.21）。驅動端、
family 的 scan manager、事件通道（kern_event）、結束碼（`EIO`）全部到位。

要改的只有 `src/kext/ventura/Ath10kVentura.cpp` 兩個函式，外加一個 kick：

1. `venturaScan(const apple80211_scan_data *)`：
   - `_wmiBSS` 清空（`count=0`，其他計數保留）
   - 重新 arm `_wmiScan`：`attempted=false; completed=false; terminal=false; cancelRequested=false; started=false; generation++`（掃描命令由 `serviceWMIScan()` 自己送，不要在這裡手刻 WMI frame）
   - 回 `kIOReturnSuccess`；`SCAN_REQ_MULTIPLE`（`apple80211_scan_multiple_data`）先忽略 SSID 清單、走全通道掃描
2. `venturaScanResult(apple80211_scan_result *result)`：**游標式**，一次回一筆，這是 airportd 的協議
   （itlwm 的 `getSCAN_RESULT()` 就是這樣做）：
   - `_venturaScanIndex` 當游標；`>= _wmiBSS.count` → 回 `kIOReturnNotFound`（airportd 收到非 0 就停止）
   - 逐欄填：`version=APPLE80211_VERSION`、`asr_bssid`、`asr_ssid_len`/`asr_ssid`、
     `asr_channel`（`rxChannel` → 頻率／channel 轉換）、`asr_rssi`/`asr_noise`/`asr_snr`、
     `asr_beacon_int`、`asr_cap`（`capabilities`）、rate 表（`rates[]`，上限 `APPLE80211_MAX_RATES`）、
     `asr_age`（0 = 新鮮結果）
   - **RSN IE**：`asr_ie_len` 填 `_wmiBSS.entries[i].rsnLength`；`asr_ie_data` 在 SDK 標頭是 `void*`，
     但 12+ 的 family 可能是「inline 緊接在結構後面」，**先量一次 airportd 傳進來的 `req_len`**
     （本輪 ioctl 已可記錄）再決定用指標還是 inline memcpy，不要照抄舊 itlwm 寫法
3. `SCANCACHE_CLEAR` 已經有實作（清 `_venturaScanIndex`），順手讓它也清 `_wmiBSS.count`。

**驗收**：`Ath10kApple80211Requests` 持續增加且 `Ath10kLastApple80211Request = 11`
（`APPLE80211_IOC_SCAN_RESULT`）＝airportd 正在拉結果；再加一個自己的計數
`Ath10kScanServed`（回成功的筆數）應該停在 `_wmiBSS.count`。UI 面：登入後 Wi-Fi 選單 / 系統設定的
網路清單應該開始列出 SSID。

#### 7.3.2 第二步：密碼驗證（WPA2-PSK，S2）

程式骨架**已經在 shared code**：`Ath10kNetwork.cpp` 的 `_connection` 狀態機
（`Scan → Start → Barrier → Peer → Auth → Assoc → …`）、`Ath10kSupplicant`（4-way handshake、
CCMP、rekey）、`buildStationManagement()`、`buildConnectionWMI()`、`outputPacket()`、
`deliverNetworkRX()` 全都有，native 版只是多一層 WCL opcode 轉接。

要做的：

1. `venturaAssociate(const apple80211_assoc_data *ad)`：解 `ad_ssid`/`ad_bssid`、
   `ad_auth_upper`（PSK=2、PSK_SHA256=6）、`ad_key`（`apple80211_key`，**內容是 passphrase 不是 PMK**）
2. **PBKDF2-HMAC-SHA256(4096, passphrase, ssid) → 32-byte PMK**：樹裡**目前沒有**這段
   （native 直接把 `join.key` 當 PMK 丟進 `supplicant.begin()`，只有 SAE 那條是真的算出來的）。
   `src/shared/Ath10kSHA256.hpp` 已有 HMAC-SHA256，補一個 4096 輪的 PBKDF2 迴圈即可；
   注意 `ad_key.key_length` 是 8–63（passphrase）還是 64（hex PSK）要分開處理
3. 把 BSS（`_wmiBSS` 裡符合 SSID/BSSID 那筆）綁進 `_connection`，設 `secure=true`、
   `securityAKM=2`，然後 `stage=LinkStage::Scan` 讓共享狀態機接手（它自己會做 vdev start / peer create /
   EAPOL 4-way / 金鑰安裝 / port authorize）
4. 回報：`APPLE80211_IOC_ASSOCIATE` 回 `kIOReturnSuccess` 後，airportd 會輪詢
   `ASSOCIATE_RESULT`（21）、`ASSOCIATION_STATUS`（22）、`STATE`（13）—— 這三個要改成反映
   `_connection.stage`，目前是寫死的 0
5. RX/TX：`_networkInterface` 必須指向**已發布**的那顆介面（`[controller+0x200]`），
   EAPOL 才會走 `inputPacket()` → `IO80211Interface` 的 supplicant 路徑；這條現在是 null-safe 的
   stub，要確認 RX 真的送得進 ifnet

**驗收**：`Ath10kSupplicantState` 前進、`_connection.portAuthorized` 變 true、
`ifconfig en1` 拿到 DHCP 位址、`networksetup -getairportnetwork en1` 回 SSID。
風險：這步開始真的要收發資料幀，任何 ring/DMA 問題都會以 panic 或靜默丟包出現，
先用空密碼的**開放網路**（`selectOpenBSS` 已有）驗證 TX/RX 通，再開 PSK。

#### 7.3.3 第三步：企業驗證（WPA2/WPA3-Enterprise，S3）

企業驗證不是自己跑 EAP：macOS 的 IO80211 supplicant（`IO80211Interface` 的 EAPOL 入口）
會處理 EAP-TLS/PEAP/TTLS，驅動只要**不要擋**：

1. RSN IE 協商允許 802.1X AKM（suite 1 / 3 / 5），`supportsPSKAKM()` 那种硬性 PSK 檢查要放寬
2. `ASSOCIATE` 時不要要求 PMK（`secure=true` 但跳過 `supplicant.begin()`），
   改成 `portAuthorized=false` 等 supplicant 完成
3. EAPOL 全數往上送（`inputPacket` 已經把 `ETHERTYPE_PAE` 交給 `IO80211Interface`），
   port 授權後才放行一般資料幀（`_connection.portAuthorized` 這格 shared code 已有）

**驗收**：802.1X 測試 AP（hostapd + EAP-TLS 最省事）能完成 EAP 交換、`portAuthorized` 變 true。

#### 7.3.4 順序與理由

先 S1 再 S2 再 S3，理由是每一階都只多一個變數：S1 只碰「把既有資料端出去」，
S2 才第一次碰 TX/RX 資料面（開放網路先驗），S3 只放寬 RSN/AKM 政策。
反過來做（先 PSK）會在「掃描結果根本看不到」的情况下同時 debug 握手與掃描，成本最高。

### 7.3-old 再把 80211 動作接到真的 WMI（已被上節取代，僅留記錄）
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
| **v54–v62** | vif 交接補上「控制器 ivar `+0x200`」「掃控制器/vif 子節點找 `BSD Name`」「`ifnet_list_get`」「3 秒延遲重試」等來源，並改用 vif 的 live vtable 位移呼叫 `configureIfnet`/`attachToBpf` | 全部拿不到 ifnet（§6.8）：`getIfnet()` null、介面不是控制器子節點、`ifnet_find_by_name` 連 en0 都回 ENXIO。VM 穩定、`en1` 在、無 panic |
| **v63** | 真的自己 `ifnet_allocate`（type 71）+ `ifnet_attach`，交給 vif `prepareBSDInterface` | **panic** 在 `IOSkywalkNetworkInterface::prepareBSDInterface+0x1a`（`ivars[0]` null：vif 沒向 Skywalk stack 註冊）。§6.9.3 #1 |
| **v64** | **把 `IOInterfaceType` 從 71 改回 6**（`initIfnetParams` 不再改 `params->type`、`attachToDataLinkLayer` 覆寫 6、手動段也寫 6） | **里程碑**：`SCNetworkInterfaceCopyAll()` 出現 `en1 type=IEEE80211`、`listallhardwareports` 有 Wi-Fi、`createnetworkservice "Wi-Fi" en1` 成功。airportd 仍 `Wireless is not available` |
| **v65–v66** | 讀 `[controller+0x200]` 並嘗試從 registry 掃描補進去 | `Ath10kCtrlInfraIvar = No`；registry 掃描版（v67）把 kernel 卡死（§6.9.3 #2） |
| **v67** | 延遲重試裡 `getMatchingServices()` 掃 `IONetworkInterface` | **kernel wedge**（`registry root held busy`），只能主機端換 bundle 救援 |
| **v68** | 在 `createInterface()` 就寫 `[this+0x200]` | **Wi-Fi 硬體埠消失**（`attachInterface()` 因 ivar 非 null 直接 return，§6.9.3 #3） |
| **v69–v70** | 改在 Apple 的 `start()` 之後寫 ivar；同時用 file-static 保存介面指標 | `Ath10kCtrlInfraSeen = No`：連 file-static 都讀不到 → 介面指標來源待釐清（`_venturaInterface` 在 +0x668）。VM 穩定、SC 仍認得 en1=IEEE80211 |
| **v71–v72** | 加 `Ath10kInstanceCreate/Start/Retry`、`Ath10kStaticCallsSeen` 斷點 | 三個 instance 指標**相同**、`CreateIfaceCalls=1`、但重試讀 static 是 **0** → 純時序問題（Apple 的 attach 比 3 秒晚） |
| **v73–v74** | 重試改成每 5 秒 re-arm（最多 8 次）；介面 `attachToDataLinkLayer()` 成功後也補 ivar | `Ath10kCtrlInfraSeen=Yes`、`Ath10kCtrlInfraInstalled=retry`（第 2 次重試）、`Ath10kDataLinkEntered=Yes`/status 0。airportd 仍不認 |
| **v111** | `IOInterfaceName` 只放 vif（讓名字 match 命中 Skywalk 物件）+ `IOUserClientClass` | airportd 打到 vif → `IOServiceOpen 0xe00002c2`；kernel log 出現 `newUserClient: ERROR: invalid interfaceRole: 1`（vif 的 newUserClient 只收 role 6/7） |
| **v112** | 名字放回介面節點，`newUserClient` 轉給 `IO80211SkywalkInterface::newUserClient`（受器 = vif） | 轉呼叫成功（family 的 client 真的開起來），但 airportd 訂閱事件時 **panic** 在 `IO80211SkywalkInterface::createEventPipe + 0x1b`（`[ivars+0x208]` null） |
| **v113** | 拒絕 service 這條（`newUserClient` 回 unsupported），加 `performCommand` 接 `SIOCGA/SIOCSA80211`，payload 用 `copyin` | airportd 打我們 174 次 ioctl，但請求結構被當成 user pointer → `copyin` EFAULT，dispatcher 一次都沒進去（`Ath10kApple80211Requests` 仍 1） |
| **v114** | ioctl 請求結構改成直接讀 kernel 記憶體（`memcpy(arg1)`） | 請求真的進 dispatcher（第一次！），但 `venturaPower()` 內呼叫 live 不存在的 `setLinkState` → panic（`RIP == CR2 == 0xffffff8052c00000`，frame 在 `venturaDispatchRequest + 0x1b6`） |
| **v115** | 拿掉 `setLinkState`，狀態只寫屬性 | **里程碑**：`getairportpower` = `Wi-Fi Power (en1): On`、`Ath10kApple80211Requests = 318`、`IoctlType = 19 (POWER)`、en1 `UP,RUNNING`、Wi-Fi 硬體埠在（§6.15.4） |
| **v75** | `init()` 改走正規 `__ZN16IO80211Interface4initEP19IONetworkController`（失敗才 fallback） | `Ath10kIfInit80211=ok`；en1 與 SC 的 IEEE80211 分類都還在；airportd 仍 `Wireless is not available`。同輪排除 IO80211 plane 與 airportd 快取兩個假設（§6.10.4） |

---

## 9. 檔案地圖

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | `probe()`/`start()`：Ventura 分支建 work loop、備 corecapture、publish `IOPacketFilters`、建 output queue、**只在 `!appleStart` 時**才手動 create→configure→attach→attachToDataLinkLayer→蓋型別→register、`preparePCIeBringupParity()`（ASPM 非致命）、建 vif 並掛 3 秒延遲重試（`_venturaVif` / `_vifPublishCall`）；所有 `Ath10k*Phase` 斷點 |
| `src/kext/ventura/Ath10kVentura.cpp` | `apple80211VirtualRequest`/`apple80211SkywalkRequest`（→ `venturaRecordRequest` 計數 wrapper）→ `venturaDispatchRequest`、`getLogPipes`（會延遲建 corecapture）、`getFaultReporterFromDriver`、`createWorkQueue`/`getWorkQueue`、`publishVenturaVifBsdUnit`（找 ifnet → 命名屬性 → `prepareBSDInterface` → vtable 呼叫 `configureIfnet`/`attachToBpf`）、`runVifPublish`/`vifPublishCall`、`alignVenturaVtable`（純回報）、`createInterface()`、`venturaPower/Scan/Associate…`（**scan/assoc 仍 stub**） |
| `src/kext/ventura/Ath10kVenturaInterface.cpp/.hpp` | `Ath10kVenturaInterface : IO80211Interface`；`init()` 用 mangled `ath10kNetworkInterfaceInit`+`ath10kEthernetInterfaceInit`，attach 前釘 IFT_ETHER；覆寫 `initIfnetParams()`（改 `params->type=71`）與 `attachToDataLinkLayer()`（成功後蓋 `IOInterfaceType=71`）；`inputPacket()` 把 EAPOL 交給 IO80211 supplicant |
| `src/kext/ventura/Ath10kVenturaDecl.inc` | Ventura 專屬宣告：live 順序的 override（兩個 request、`getLogPipes`、`getFaultReporterFromDriver`、`createWorkQueue`/`getWorkQueue`、`hardwareOutputQueueDepth()`、`performCountryCodeOperation(IO80211CountryCodeOp)`）、16 reserved slot、`publishVenturaVifBsdUnit`、`_venturaVif`/`_vifPublishCall`、請求計數器成員 |
| `vendor/itlwm-v1/Airport/IO80211Controller.h` | **已改寫**：`0xc60` 起整段換成 live `IO80211Family` 的虛擬函式順序（原本描述的是 `IO80211FamilyLegacy`）。改動理由與驗證方式見 §6.5，**不要退回舊順序** |
| `src/kext/Ath10kNetwork.cpp` | `configureInterface()`、`createWorkLoop()`、`createOutputQueue()`、`getHardwareAddress()` |
| `src/kext/native/*` | Sequoia（v2）對照實作：WMI 掃描/關聯、`getFaultReporterFromDriver()`、`IO80211InfraInterface` 用法 |
| `tools/ifnet-type.c` | 在 guest 內編譯即印出 kernel 眼中的 `if_data.ifi_type`（判斷單元真型別，別信 ioreg property） |
| `tools/io-match.c` | 在 guest 內編譯即重播 airportd 的 `IOServiceGetMatchingService({IOPropertyMatch={IOInterfaceName}})`：印出命中節點的類別、`IO80211InterfaceRole`、`IOUserClientClass`，並自己 `IOServiceOpen(type 0)`。它沒有 `com.apple.wlan.authentication` entitlement，失敗是預期行為（§6.15.6） |
| `tools/sc-interfaces.c` | 在 guest 內編譯即印出 **`SCNetworkInterfaceCopyAll()` 真正看得到的介面**（名稱/型別/BSD name）。驗收 Wi-Fi 服務就靠這支：介面不在清單裡 = SC 根本沒註冊它，`IOInterfaceType` 再對也沒用（§6.7） |
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

# 2) 部署 + 開機：pull 腳本吃版本號，deploy 腳本吃 bundle 目錄
ssh 100.64.0.152 'bash /tmp/pull_v.sh NN'          # 從 guest 編譯輸出打包成 /tmp/vkNN
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_arg.sh /tmp/vkNN'   # 寫進 ESP 並開機（約 2 分鐘）

# 3) 讀斷點（先 serial、再 ioreg）
ssh 100.64.0.152 'tr -d "\000" < /tmp/ventura-vm.serial | grep -iE "ath10k|panic|Kernel trap" | tail -20'
ssh 100.64.0.152 '/tmp/g.sh "ifconfig -l; ioreg -l -w0 | grep -E \"Ath10k(AppleStart|Publisher|StartPhase|VifPhase|VifBsdSource|VifBsdPhase|VifAttachPhase|VifPublishRetry|ChildCount|FindEn0|RecoveryPhase|Apple80211Requests)\" | head -20"'

# 4) 網路/SC 狀態（**驗收先跑 sc-interfaces**，不要先看 networksetup）
ssh 100.64.0.152 '/tmp/g.sh "cd /tmp && ./sc-interfaces; networksetup -listallhardwareports; networksetup -getairportpower en1"'

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
`install_bundle.sh <dir>` 直接從 Acer 的 `/tmp/vkNN` 取 bundle 寫進 ESP 再開機，完全不依賴 guest。Acer 上留著 vk20…vk62（v48、v52、v53、v62 是本輪常用）。

**本輪新增的可重用腳本（Acer `/tmp/`，密碼一律 `cat ~/sudo.pwd |` 餵 stdin）**：
```bash
bash ~/.hermes/cache/scratch/build_v.sh     # 本機：tar src → Acer → guest 內 make VENTURA=1 -j2 kext（副本在 Acer /tmp/build_v.sh）
ssh 100.64.0.152 'bash /tmp/pull_v.sh NN'   # Acer：從 guest 拉 build 成 /tmp/vkNN（吃版本號，不要再 sed 舊腳本：`vk54→v55` 這種字串 sed 會漏）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_arg.sh /tmp/vkNN'   # 部署任意版本並開機
ssh 100.64.0.152 'printf "system_reset\n" | socat - UNIX-CONNECT:/tmp/ventura-vm.mon'   # 熱重開（不換 kext）
```
注意：`deploy_arg.sh` 用 `read -r PW` 收密碼，**同一個 ssh 指令裡不要先跑會吃 stdin 的腳本**（例如再包一層 ssh 的 pull），否則密碼會被吃掉、sudo 認證失敗。要在 guest 內用 sudo 時：`cat ~/sudo.pwd | ssh 100.64.0.152 'ssh -p 2223 user@127.0.0.1 "sudo -S -p \"\" 指令"'`。

**離線驗證 vtable（不開機，改 header 後必做）**：guest 內 `otool -rv` + `otool -s __DATA __const` + `nm -n`，本機用 KC symtab 重建 live 表比對；腳本在 `~/.hermes/cache/scratch/`（`kc_own.py`、`kc_sym.py`），方法見 §6.5。

---

## 11. 分析工具（可直接重用）

- `/tmp/kc2`：Ubuntu 上的 `BootKernelExtensions.kc` 副本（69MB），不用再從 guest 抓。
- `/tmp/an.py` + venv `/tmp/cs`（capstone 5.0.7）：
  - `list <子字串>` 找符號、`vtable <位址> <entry數>` 解 vtable（含 chained fixup 解碼）、`dis <符號或位址> <bytes>` 反組譯並標註 call 目標、`str` / `ptr` 讀字串指標。
- `/tmp/vt_both.txt`：兩份 `IO80211Controller` vtable 對照（`vt+` 欄位才是 kernel 索引用的值）。
- vtable 反查小抄（本輪新增，`an.py vtable <live ZTV位址> 700` 直接查）：
  - `IO80211VirtualInterface`（`__ZTV` @ `0xffffff80020de968`）：284 `initBSDInterfaceParameters`、285 `prepareBSDInterface`、286 `finalizeBSDInterface`、287 `getBSDInterface`、288 `setBSDName`、289 `getBSDName`、455 `setUnitNumber`、456 `attachToBpf`、457 `configureIfnet`。
  - `IONetworkStack`（`__ZTV` @ `0xffffff8002c71898`）：`interfacePublished` @ `0x2c66786`、`registerNetworkInterface` @ `0x2c675f6`、`attachNetworkInterfaceToBSD` @ `0x2c66f04`、`reserveInterfaceUnitNumber` @ `0x2c67272`；vtable index 104 = `IORegistryEntry::isParent`（就是 §5.3 撞到的那個）。
  - `IONetworkInterface::attachToDataLinkLayer` @ `0x2c638a4`（它自己建 `ifnet_init_eparams` version 2 / len 0x158）。
- **掃 KC 的 `__text` 一定要用 `__TEXT`**：KC 每個子映像的執行段叫 `__TEXT`（不是 `__text`），寫成 `__text` 的掃描腳本會「全部 0 命中」而看似「沒有呼叫者」（本輪踩過）。
- **`IO80211Controller::createVirtualInterface` 只接受 role 7**（`cmp edx,7`），role 7 是 SoftAP/P2P vif，不是主介面（§6.7）。
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
- **`IOService::serviceMatching("IO80211Interface")` 不會回傳子類別實例**（`getMatchingServices` 只給精確類別）；要掃自己的介面得用實際類名 `"Ath10kVenturaInterface"`——但**即使類名正確，也掃不到已發布的介面節點**（v51c/v57 實測；介面不是控制器的子節點，掃 registry 這條路已封，§6.8）。
- **`_venturaInterface` 不是擁有 BSD 單元的那顆物件**：`super::start()` 自己會建／發布介面，`getIfnet()` 在它身上回 null（§6.8）。多介面會讓 SC 連 Wi-Fi 硬體埠都不列 —— 別再加介面，先減（§7.1）。
- **`ifnet_find_by_name()` / `ifnet_list_get()` 在本 kext 回 ENXIO／看不到自己的單元**（連 `en0` 都查不到，§6.8）。**不要**再用名稱或清單去找 ifnet。
- **SC / networkd 看不到的介面，看起來再正常都沒用**：驗收一律先跑 `tools/sc-interfaces.c`（`SCNetworkInterfaceCopyAll()`），不要用 `IOInterfaceType`、`ifi_type`、`NetworkInterfaces.plist` 記錄判斷成敗（三者都已被證明會誤導，§6.7）。
- **`NetworkInterfaces.plist` 決定 BSD 單元號碼**：砍掉某個介面的記錄後，重開機會配到較小的號（本輪 en2 → en1）；不要假設單元號固定，腳本裡的 `enN` 要現查。
- **改完 header 一律先做離線 vtable 比對再開機**：順序錯了不會編譯失敗，只會在開機時 panic 在看起來無關的 framework 函式裡（§6.5）。

---

## 6.20 第十六輪：掃描接上家庭的 scan manager，完成事件走 kern_event（v128–v142）

### 6.20.1 這一輪做完的三件事（全部實測）

1. **真正的 `IO80211ScanManager`**（v128）。`IO80211Controller::getPrimaryInterfaceScanManager()`
   是 `getPrimarySkywalkInterface()` 之後再打 vtable slot 373（`vptr+0xb98`），而 vif 那一格是
   `xor eax,eax; ret`。做法：用 `MetaClass::alloc()`（`__ZNK18IO80211ScanManager9MetaClass5allocEv`）
   建 manager、`initWithControllerAndSkywalkInterface` 初始化、再把 vif 的 vtable **執行期複製
   一份**、只換 slot 373 指向回傳 manager 的 thunk、把 `[vif]` 換成新表。不子類化、不 `new` Apple 類別。
   - init 只要求：`getWorkQueue()` 非 null、`service->GetProvider()` 非 null、而且
     `CARD_CAPABILITIES` 回覆的第 6 byte（`caps[2]`）要有 `0x40`（capability #22）——這一條是本輪
     才發現的硬門檻，沒設 manager 就建不起來。
   - 斷點：`Ath10kScanMgrInit=ok`、`Ath10kScanMgrPatch=patched`、`Ath10kScanMgrVisible=yes`，
     `IOKitDiagnostics` 裡 `IO80211ScanManager` 實例 = 1。

2. **驅動端把 manager 的流程驅動起來**。`SCAN_REQ` 走 `startScan()`（它會建 pid-keyed 的 pending
   request node）、掃完由驅動呼叫 `scanDone(true)`，manager 會**自己**用
   `apple80211_scan_result **`（pointer-out，0x48c，IE 內嵌在 0x8c）連拉結果直到 driver 回非 0。
   斷點：`Ath10kScanMgrStart=ok`、`Ath10kScanMgrDone=ok`、`Ath10kScanServed` 隨每次掃描增加。

3. **完成事件走 kern_event，`-3905` 消失**（v131，本輪關鍵）。airportd 讀的不是 IOUC 管道
   （`postMessage` 回 0 也沒用），而是 `Apple80211EventMonitoringInit2` 用
   `ioctl(fd, 0x800c6502, …)` 註冊的 **kernel-event fd**，讀端是 `read(fd, buf, 0x1018)`（要 ≥0x18
   byte）。真正的入口是 `IO80211Interface::postMessage(ifname, msg, data, len, controller)`
   （live `0x1ff029f`，EXT），它組 `kev_msg{vendor=1, class=6, subclass=1, event=msg,
   dv[0]={16,ifname}, dv[1]={len,data}}` 交給 `IO80211QueueCall::kev_post_msg`（`0x2048596`）→
   xnu `_kev_post_msg`。補上這條之後 `cwscan` 從「重試 85 秒失敗」變成 26 秒回來。
   斷點：`Ath10kKevStatus="done=0 cache=0"`。

### 6.20.2 反編譯得到的兩個語意（不要再用猜的）

- **`IO80211ScanManager::getScanResult`（`0x2013b84`）**：讀 `[manager+0x10]+0xa8` 的 pending req
  node 鏈，逐一比 `proc_selfpid()`（節點內 `+0x2a60` 是結果鏈、`+0x2a50` 是完成旗標）。命中才回
  `*result = [entry+0x18]+0x14` 並把該節點游標推進，否則回 **5**。也就是說「回 5」的意義是
  「沒有屬於我的節點」，**不是**「cache 空」；kernel 執行緒（pid 0）去呼叫永遠會拿到 5。
- **`IO80211ScanManager` 的快取年齡**：`setCacheAgeThreshold` 寫 `[ivars+8]`（`procIsSuperuser`
  才能寫），`cacheAgeThresh` 讀它，direct/broadcast 在 `[ivars+0x14]` / `[ivars+0xc]`。
  本機原本 `direct=0 bcast=0`，已設成 30/300（`Ath10kScanMgrAges` / `AgesAfter` 斷點）。

### 6.20.3 現在的真實狀態（v141 穩定，v142 已回退）

- **驅動端掃描完全可用**：guest 內 `airport -s` 與 `tools/apple80211-scan.c` 都列出十多個真實 AP
  （SSID／BSSID／頻道／RSSI／速率／內嵌 RSN IE 全正確，`total=14` 級別）。
- **airportd／CoreWLAN 仍是 0 筆網路**，失敗點已鎖定到單一斷點：
  `Ath10kIoctlFall="type=11 st=66"` —— 有一個 `SCAN_RESULT`（type 11）落到 Apple 原實作、回
  `0x66`（= 它回報的 `error=102`）。`Ath10kResultShape="ch=… fl=8a nr=12 ie=22"`、
  `Ath10kResultReqLen=1164`（= 0x48c）證明**驅動給的結構是完整且長度正確的**。
- **v142 的死路（別重做）**：在 `apple80211SkywalkRequest` 的 type-11 分支「pointer-out 失敗就改填
  一般緩衝」會讓 airportd 的讀取迴圈卡死（`cwscan` >290 秒無回應），已回退到 v141。

### 6.20.4 下一輪的第一步（明確）

不要在 fall-through 之後記錄，要在**入口**分辨 airportd 的 type-11 是三條通道中的哪一條：
`performanceCommand`（BSD ioctl 40 byte）、slot 406 的 getter 慣例（cmd 在 arg1）、slot 408
`apple80211SkywalkRequest`。在三個入口各自記一組 `type/verb/req_len` 斷點，一次 `cwscan` 就能看出
它走哪條，再只修那一條（v142 的教訓：混著改會卡死）。伺服器端不需要任何改動。

### 6.20.5 這輪新增/留下的斷點

`Ath10kScanMgrInit|Patch|Visible|Start|Done|Dones|Serves`、`Ath10kScanMgrAges|AgesAfter`、
`Ath10kKevStatus`、`Ath10kPostStatus`、`Ath10kResultShape|ResultReqLen`、`Ath10kCursorStatus`、
`Ath10kIoctlFall`、`Ath10kScanServed|ScanEnds|ScanCached`。

### 6.20.6 部署備忘（這輪踩到的）

- Acer 上的工具在 `~/qca-tools/`（`pull_v.sh`、`deploy.sh`、`oc_enable.py`…），不是 `/tmp`；
  HANDOFF 早期章節寫的 `/tmp/pull_v.sh`、`/tmp/deploy_arg.sh` 已經不在了。
- guest 的 `/tmp` **每次重開都會清空**，`cwscan`／`a11scan`／量測腳本要每次重傳。
- 部署固定式：`bash ~/.hermes/cache/scratch/build_v.sh` → `ssh 100.64.0.152 'bash ~/qca-tools/pull_v.sh NN'`
  → `cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/deploy.sh /tmp/vkNN'`。

---

### 6.21 第十七輪（v143–v149）：airportd 的掃描從 0 筆變成 14–16 筆

#### 6.21.1 用三入口斷點把「哪條通道」量出來（§6.20.4 的做法）

新增 `Ath10kEnt_perf` / `Ath10kEnt_slot406` / `Ath10kEnt_slot408`（各自 type-11 的命中數、`len`、
最後狀態）與 `Ath10kScanServeTrace`（最近 8 筆 `pid:index`）。一次 `cwscan` 就得到答案：

- **type-11 只走 slot406（getter 慣例）與 slot408（manager 的 pointer-out）**，兩邊各 91–95 次、
  `len=1164 (0x48c)`；`performCommand`（BSD ioctl）那條 **0 次**。
- 讀者是 **airportd 本人**（`ScanServeTrace` 的 pid = airportd），不是 cwscan；cwscan 透過 XPC 請它掃。

#### 6.21.2 死路（全部實測，勿重做）

| # | 做法 | 結果 |
|---|---|---|
| 1 | pointer-out 用完時「掉進一般緩衝分支」當後備 | **panic**：在呼叫端只有 8 byte 的 local 上 `memset` 0x48c → 撞堆疊守衛頁（`IO80211ScanManager::scanDone` → `apple80211SkywalkRequest+0x6b`，RIP 在 memcpy，CR2 = 堆疊頂、RDX = 0x48c）。pointer-out 成立時**結尾直接回傳，不碰緩衝** |
| 2 | manager 與 client 共用一個 `_venturaScanIndex` | 半份清單：manager 吃掉 0–10，airportd 從 11 開始只拿到 11–19。改成 **per-pid 游標**（`venturaReaderSlot()`）才讀完整份 |
| 3 | 結束碼用 `kIOReturnNotFound`(0xe00002f0) | airportd log「Failed to perform Wi-Fi scan, returned error code **-536870160**」→ networks=0。**airportd 把結束碼原封不動當掃描結果** |
| 4 | 跨檔 helper 只在一端寫 `extern "C"` | kext 允許 undefined symbol → build 照過，但 **OpenCore 靜默跳過整個 kext**（序列埠零 ath10k、`kmutil showloaded \| grep -c ath10k` = 0、ioreg 完全沒有 Ath10k 屬性）。兩端都要寫；驗證用 `nm -u \| grep <name>` |

**正解**：結束碼用 **`EIO(5)`**（classic ioctl 通道本來就產生 5，itlwm 也是 5/12）→ airportd log 變
`Completed scan … error=0`。

#### 6.21.3 實測（v149，Acer `/tmp/vk149`，sha256 `f517336a…`）

```
./cwscan en1
interface=en1 power=1
networks=14                     ← 第二次跑 16
  ssid='Qoonet'         bssid=(null) ch=10 rssi=-67 sec(wpa2=1 wpa3=0 wpa=0)
  ssid='Jane'           bssid=(null) ch=3  rssi=-72 sec(wpa2=1 …)
  ssid='142-wifi6'      bssid=(null) ch=6  rssi=-72 sec(wpa2=1 …)
  …（共 14 筆，SSID／頻道／RSSI／WPA2 全對）
airportd log: Completed scan for pid 373 (cwscan) (error=0, duration=22.01s, …)
```

- **BSSID 顯示 `(null)` 不是驅動問題**：airportd 對沒有 location 授權的行程會剝掉 BSSID 與國碼
  （log：`Filtering BSSID and country code from scan results because location services not authorized
  for pid N`）。同一份資料在 `airport -s` 與 `tools/apple80211-scan.c` 都印得出完整 BSSID。
- 掃描本身 22 秒：airportd 把 5GHz 拆成 3 個 channel subset（6 個 channel 一次）各掃一輪，不是我們慢。

#### 6.21.4 這輪的檔案變更

- `src/kext/ventura/Ath10kVentura.cpp`：三入口斷點與 `Ath10kScanServeTrace`、pointer-out 結尾
  不再掉進一般緩衝、pointer-out 走 per-pid 游標、`venturaScanResult()` 結束碼改 `EIO`、type-11
  在 slot406 不再 fall-through 給 Apple（那只會回 0x66 = airportd 的 error 102）。
- `src/kext/ventura/Ath10kVenturaInterface.cpp`：`performCommand` 也掛 slot 0 斷點。

#### 6.21.5 下一步

S1 完成，接 §7.3.2（WPA2-PSK 關聯）：PBKDF2 → `_connection` 綁 BSS → `venturaAssociate()`。

---

### 6.22 第十八輪（v150–v151）：開放網路關聯成功，上行通、下行還沒

#### 6.22.1 測試環境（可重現，不需要使用者家的密碼）

本機（跑 agent 這台，不是 Acer）用 hostapd 開一個開放 AP，SSID `QCA-TEST`、2.4 GHz ch6、
`192.168.77.1/24`，dnsmasq 只發 IP（`--dhcp-option=3 --dhcp-option=6` 不派 gateway/DNS，
避免動到 VM 的預設路由）：

```
nmcli dev set wlp9s0 managed no
ip addr add 192.168.77.1/24 dev wlp9s0; ip link set wlp9s0 up
hostapd -B docs/hostapd-qca.conf          # ssid=QCA-TEST hw_mode=g channel=6 wpa=0
dnsmasq --port=0 --interface=wlp9s0 --bind-interfaces \
        --dhcp-range=192.168.77.50,192.168.77.80,255.255.255.0,1h \
        --dhcp-option=3 --dhcp-option=6 --log-dhcp
```

#### 6.22.2 `venturaAssociate()` 原本只是 stub → 接上共用狀態機

- 解析 `apple80211_assoc_data`：用 `ad_ssid/ad_ssid_len` + `ad_bssid`；BSSID 全零時用
  `_wmiBSS` 快取裡同 SSID 中 SNR 最好的那筆（family 的掃描快取在這裡讀不到）。
- 填 `_connection`（ssid/bssid/local/txPowerHalfDbm 20）→ `stage=Scan`，之後
  `serviceConnection()` 自己跑 vdev start → peer → auth → assoc。
- 開放網路 `secure=false`：跳到 Connected 時狀態機自己 `portAuthorized=!secure`、
  `networkLink(true)`。
- WPA2-PSK 目前明確拒絕並留麵包屑 `Ath10kAssocPhase="psk-not-supported …"`（這個 personality
  沒有核心內 supplicant）。

#### 6.22.3 兩個實測陷阱

| # | 假設 / 做法 | 實測結果 |
|---|---|---|
| 1 | `APPLE80211_IOC_ASSOCIATE` 的 type 當成 5 | **不是**：`apple80211_var.h` 是 **20**（STATE 是 13，不是 4）。送 5 會落到「相容清單」直接回 success，看起來像「關聯沒發生」，`Ath10kRequestTrace` 只會出現 `s5=0` |
| 2 | 進關聯時把 `_connection.error` 清成 `nullptr` | **panic**：`publishConnection()` 把它直接丟給 `setProperty()`，`OSSymbol::withCString(NULL)` 在 `strnlen` 取址 0（RIP 在 `_strnlen+0x20`、CR2=0，frame 是 `publishConnection+0xef4` ← `serviceConnection+0xe78`）。要寫 `"none"` |

#### 6.22.4 實測（v151，Acer `/tmp/vk151`，sha256 `f5e1b472…`）

```
./a11assoc QCA-TEST en1 20
ASSOCIATE accepted for 'QCA-TEST' on en1
  state=1
  state=4 (RUN)                      ← APPLE80211_S_RUN
```

- 驅動：`Ath10kConnectionState="connected-open"`、`AID=1`、`Channel=6`、`PHY="HT20-1SS"`、
  `PHYReason="ht-common-capabilities"`、`PeerCount=2`。
- AP 端（`iw dev wlp9s0 station dump`）：`authorized/authenticated/associated: yes`、
  `signal -35 dBm`、`last ack signal -33 dBm`、`rx bytes 6994` ← **上行真的通了**。
- 串流指令：`0x5003`(vdev start) → `0x5008`(peer create) → `0x6004`(peer assoc) → `0x1d001`。
- **下行未通**：`HTTRxIndications=0`、`DataRxPackets=0`、`HTTRxPoolPosted=1023`（韌體從沒把 RX
  MPDU 交給主機），`ping6` 從 AP 打 guest 的 link-local 0/3。這是下一關。
- `en1` 只有 link-local IPv6：`ipconfig set en1 DHCP` 回 `invalid operation`（en1 在 SC 裡還沒有
  DHCP 服務），要先把 "Wi-Fi" 服務設成 DHCP 才有 IPv4。

#### 6.22.5 下一步

1. 查 HTT RX：`HTTRxIndications=0` 但 pool 已 post 1023 → 檢查 RX ring 的 fill/arm 與
   `rxSetup` 之後的 re-fill 是否只在 native 路徑跑。
2. DHCP：`networksetup -setdhcp "Wi-Fi"` 之後再驗 IPv4 + ping。
3. 之後才是 WPA2-PSK（supplicant 搬進 Ventura）。

---

### 6.23 第十九輪（v154–v160）：WPA2-PSK 通了，加密鏈路可上網

#### 6.23.1 這輪補上的東西

- `venturaAssociate()` 不再拒絕有金鑰的網路：WPA2-PSK（AKM 2）與 PSK-SHA256（AKM 6）走核心內
  supplicant；802.1X/EAP 仍明確拒絕（`enterprise-not-supported`）。
- 新增 `Ath10kVenturaSupplicant`（`AthSecurity::Supplicant<Ath10kKernelCrypto,true,true>`
  + PMK + reply/keys 暫存），`venturaSecurityArm/Accept/Service/Stop`；
  金鑰仍交給共用狀態機的 securityPhase 階梯安裝（`buildSecurityWMI`），不自己寫 WMI。
- `Ath10kNetwork.cpp` 的 `serviceConnection()` 在 `#ifndef ATH10K_NATIVE` 下先跑
  `venturaSecurityService()`，讓握手結果比安裝階梯早一步就緒。
- `tools/apple80211-assoc.c` 支援帶密碼（argv 或 stdin），並用 SDK 的真實位移填 `ad_key`。

#### 6.23.2 三個只看得到「AP 丟掉 M2」的坑

| # | 坑 | 症狀 → 修法 |
|---|---|---|
| 1 | PMK 用 PBKDF2-HMAC-**SHA256** | hostapd log：`PTK derivation using PRF(SHA1)` 之後 `invalid MIC in msg 2/4`，三次重傳後 deauth reason 15。WPA2-PSK 的 PMK 是 **SHA1**（4096 輪、salt=SSID、32 byte = 兩個 20-byte block） |
| 2 | 初次安裝金鑰時要求 `securityPhase != 0` | 死鎖：金鑰只在 phase 0 該裝，條件寫反就永遠不裝 → 一樣是 reason 15。正確條件是 native 的 `initial && !securityPhase` |
| 3 | 直接呼叫 corecrypto `ccpbkdf2_hmac` | MacKernelSDK 把這個符號宣告成 **C++ 修飾名**（`__Z13ccpbkdf2_hmac…`），核心匯出的是 C 名 → 未解析符號 → OpenCore 整個 kext 靜默跳過（序列埠零 ath10k、`nm -u` 看得到修飾名）。改成用 supplicant 已連結的 HMAC 自己實作 PBKDF2 |

#### 6.23.3 驗證（v160，Acer `/tmp/vk160`，sha256 `cd3c3548…`）

驗證方式：本機 hostapd（`QCA-TEST`、WPA2-PSK/CCMP、passphrase 為一次性測試值）＋ guest 內
`a11assoc QCA-TEST en1`（密碼走 stdin）。

```
ASSOCIATE accepted for 'QCA-TEST' on en1 (wpa2-psk)
  state=4 (RUN)
```
- AP 端（hostapd `-d`）：`received EAPOL-Key frame (2/4 Pairwise)` → `PTK derivation using PRF(SHA1)`
  → `sending 3/4 msg` → `Set STA flags … authorized=1` → **`AP-STA-CONNECTED`**；
  `iw station dump`：authorized/authenticated/associated = yes、signal −33 dBm、rx 5614 bytes。
- 驅動端：`Ath10kConnectionState="connected-wpa2"`、`Ath10kSecurityPhase=5`、
  `Ath10kSecPhase="keys-in initial=1 pair=0 gtk=1"`、`DataTxPackets=24`。
- 加密鏈路上網：`ping 1.1.1.1` 2/2、**`https://example.com` → HTTP 200**。
- 中間用兩個指紋把問題釘死（只在 ioreg，不輸出金鑰）：`Ath10kSecCheck`（passphrase 長度／前 2 byte
  ＋ PMK 前 2 byte）與 `Ath10kSecPtkFp`（PTK 前 2 byte）。兩者都與本地獨立算的
  PBKDF2-SHA1、PRF-384 一致 → 證明問題在別處，最後才鎖定「phase 0 才該裝金鑰」這個條件。

#### 6.23.4 還沒做的

- 802.1X/EAP（需要 Apple 的 user-space supplicant 或 802.1X 堆疊，不是核心內能解決的）。
- WPA3-SAE（native 已有 `Ath10kSAE*` 那套；Ventura 還沒接）。
- PMF（AP 要求 MFPR 時的 IGTK/BIP）。
- DHCP：`ipconfig set en1 DHCP` 在 en1 仍回 `invalid operation`（`networksetup -setdhcp "Wi-Fi"`
  之後還沒生效），目前用靜態 IP + 預設路由驗證；要讓系統自己拿 IP 還得修這條。
- 省電：工作站睡著時 AP 會壓住封包（`HTTRxIndications=0`），一有 TX 就恢復；要徹底解得關韌體 PS。

---

### 6.24 第二十輪（v161–v164）：WPA2-PSK 穩定化、DHCP 卡點定位、企業／SAE 的社群結論

#### 6.24.1 這輪修好的

- **關聯復原**：掃描被取消／失敗後 `_wmiScan.completed` 是 false，而 Scan 階段的第一道條件就是它，
  於是每次關聯都在 5 秒後 `connection-stage-timeout` → 卡在 Failed，之後的關聯全部 EIO。現在
  `venturaAssociate()` 進來先看 `Ath10kAssocScanRearm`：有 BSS 快取就沿用（`cache-reuse`），
  有完成但被取消就重新 arm（`yes`）。
- **上一輪「PSK 突然失效」其實是測試腳本的問題**：`joinlease.sh` 要把密碼當 `$1` 傳，漏傳就變成
  「用開放網路去連 WPA2 AP」，AP 當然不收，機器回 `protected-network-not-supported`。
- **新增 `Ath10kAssocRaw` 麵包屑**：把 ASSOCIATE 請求真正收到的欄位直接印出來，不必再猜。
  實測 WPA2-PSK 請求：`ver=1 len=8 upper=0008 lower=0000 keylen=12 rsnlen=0`；搭配
  `Ath10kSecCheck`（passphrase／PMK 指紋）與 `Ath10kSecPtkFp`（PTK 指紋）可以一次分辨
  「請求錯」、「PMK 錯」、「PTK 錯」還是「金鑰沒裝」。

#### 6.24.2 DHCP 的真正卡點（還沒解完）

症狀：服務確實是 DHCP（`Setup:/Network/Service/DFAB0457-…/IPv4 = ConfigMethod DHCP`，介面
`DeviceName: en1`、`Hardware: AirPort`）、`State:/Network/Interface/en1/Link = Active: TRUE`、
`networksetup -setmanual` 也能成功把 IP 設上去——但 **DHCP client 從來沒啟動**：
`ipconfig getpacket en1` = not found、`ipconfig getsummary en1` 只有 IPv6、AP 端 dnsmasq 一個
DISCOVER 都沒有。

決定性證據（`configd` 的 IPConfiguration，`ipconfig setverbose 1` 後）：

```
arp_client_init(en1): unsupported network type
MANUAL en1: arp_client_init failed
```

即 **IPConfiguration 的 ARP client 不接受這個介面的 `if_data.ifi_type`**；沒有 ARP client
就不會啟動 DHCP client。IPv6 不受影響（`RTADV en1: sending Router Solicitation` 照跑），
所以先前看起來「IPConfiguration 有在動」，其實只動了 v6。

這輪的處理：ifnet 從 `IFT_IEEE80211(71)` 改成 **`IFT_ETHER(6)`**（v164）。airportd 判斷
「這是不是 Wi-Fi」看的是 `SIOCGIFMEDIA`（`kIOMediumIEEE80211Auto`），與這個欄位無關，所以不會
把介面弄掉。**v164 實測 DHCP 仍未啟動**，代表還有第二層條件，或 `ifi_type` 沒真的改到——
下一步需要一支能讀 `if_data.ifi_type` 的小工具確認實際值（`tools/if-hwtype.c` 兩個寫法在 SDK
都編不過，需修：`SIOCGIFHWADDR` 不存在、`SIOCGIFDATA` 沒有公開常數）。

#### 6.24.3 順帶更正一件事

先前寫的「加密鏈路 HTTP 200」其實是走 **en0（QEMU user-net）**，不是 Wi-Fi：`netstat -rn` 的預設
路由是 `10.0.2.2 en0`。Wi-Fi 這邊只證明到「ping 得到 AP `192.168.77.1`」。要驗 L3 上網，必須
把預設路由指到 `192.168.77.1` 並確認封包真的從 en1 出去。

#### 6.24.4 社群結論（企業 802.1X／WPA3）

- itlwm #784：「Monterey 版關掉 Apple RSN 在 Ventura 正常；**開 Apple RSN 會 kernel panic**。」
  → 走 Apple 自家 supplicant 來支援企業／WPA3，在 Ventura 是已知地雷。
- WPA3／Wi-Fi 6-7 的 AirportItlwm 分支（laobamac、Ersiter）都只支援 Sequoia/Tahoe，靠 WCL
  （native personality），不是 Ventura 這條 v1 介面。
- 因此：Ventura 這支若要支援 SAE，得自己把 `Ath10kSAE*`（P256／SAE exchange）搬進這條
  personality，並補 PMF（IGTK/BIP）；企業 802.1X 只能冒 panic 風險試 Apple RSN，或自帶
  802.1X 堆疊，屬於獨立一輪的工作。

#### 6.24.5 測試環境（重現用）

- host：hostapd `QCA-TEST`（2.4 GHz ch6、WPA2-PSK/CCMP、密碼為一次性測試值）、dnsmasq
  `192.168.77.50-.80`（router 192.168.77.1／DNS 1.1.1.1）、`iptables MASQUERADE` 出 eno1、
  `net.ipv4.ip_forward=1`。
- guest：`a11assoc QCA-TEST en1 20 <passphrase>`（密碼走 argv 或 stdin；`joinlease.sh` 一定要
  帶參數，否則會用開放網路去連 PSK AP）。
- 判讀重點：`Ath10kConnectionState`、`Ath10kSecurityPhase`、`Ath10kSecPhase`、`Ath10kAssocRaw`；
  AP 端 `iw dev wlp9s0 station dump` 的 `authorized` 與 hostapd `-d` 的 `2/4 → 3/4` 序列。

#### 6.24.6 下一步

1. **DHCP**：確認 `ifi_type` 是否真的變成 6；若已是 6 仍失敗，往 IPConfiguration 的第二層條件查
   （服務狀態、`AUTOMATIC-V4` 是否被建立、是否被 CaptiveNetworkSupport 的「interface en1 is not
   active」擋住）。
2. 拿到 IPv4 後把預設路由指到 `192.168.77.1`，用 `netstat`/AP 端計數確認封包真的從 en1 出去。
3. WPA3-SAE：搬 `Ath10kSAE*`；企業 802.1X：先量 Apple RSN 的 panic 風險再說。

---

### 6.25 第二十一輪（v165）：DHCP 通了 —— 型別 71 是自己釘的，不是 airportd 要的

#### 6.25.1 反組譯 `arp_client_init`（`IPConfiguration.bundle`，不是猜的）

guest 裡 `/System/Library/SystemConfiguration/IPConfiguration.bundle/Contents/MacOS/IPConfiguration`
（1.05 MB，x86_64 slice 在磁碟上，不必碰 dyld cache）。字串 `arp_client_init(%s): unsupported network type`
在 VA `0x5c5f0`，唯一 xref 在 `0xa1ce`，函式本體 `0xa022`：

```
a0a6  call <ifi_type(if_p)>
a0ab  lea  ecx, [rax - 0x87]     ; 135
a0b1  cmp  ecx, 2
a0b4  jb   0xa0e2                ; 135 (IFT_L2VLAN) / 136 (IFT_IEEE8023ADLAG) → 收
a0b6  cmp  eax, 6                ; IFT_ETHER → 收
a0b9  je   0xa0e2
a0bb  cmp  eax, 0x90             ; 144 IFT_IEEE1394 → 走 firewire 取位址
a0c0  jne  0xa16e                ; 其餘 → "unsupported network type"
```

也就是 **只接受 6 / 135 / 136 / 144**，71 一定被拒。真機 Wi-Fi 拿得到 DHCP，代表 Apple 自己的 Wi-Fi
ifnet 就是 `IFT_ETHER`（`IOEthernetInterface::initIfnetParams` 的 `cmp eax,6` 也印證）。

#### 6.25.2 為什麼 71 之前被當成必要條件（錯誤推論的來源）

§6.11 量到的是 `SIOCGIFMEDIA` 要帶 `IFM_IEEE80211`（媒體，不是型別），§6.14 把它誤記成
`getifaddrs` 只收 `ifi_type == 71`，於是 `initIfnetParams()` 在 base 之後多寫了
`params->type = 0x47`。結果 airportd 沒問題、IPConfiguration 的 ARP client 卻永遠建不起來
→ 沒有 DHCP client，只有 IPv6 在動。

#### 6.25.3 v165 的實測（Acer `/tmp/vk165`，sha256 `86ff4b92…`）

改動只有一行：`Ath10kVenturaInterface::initIfnetParams()` 的 `params->type` 由 `0x47` 改成 `0x6`
（airportd 端不再靠型別）。

| 檢查 | 結果 |
|---|---|
| `tools/ifnet-type`（guest 內編譯） | `en1 type=0x06 (6)` |
| `networksetup -getairportpower en1` | `Wi-Fi Power (en1): On`（**沒有退化**） |
| `cwscan en1`（airportd 真實路徑） | `networks=13`，清單與之前相同 |
| 關聯（`a11assoc QCA-TEST en1`，密碼走 stdin） | `state=4 (RUN)`、`Ath10kConnectionState=connected-wpa2`、`SecurityPhase=5` |
| DHCP | `dhcp_bound(en1) 192.168.77.75`、`ipconfig getpacket en1` = BOOTREPLY、`RouterARPVerified : TRUE`（ARP client 這次真的活著）、`ConfigMethod : DHCP`、租約 1h |
| L3（`route change default 192.168.77.1`） | `ping 1.1.1.1` 3/3、avg 8.7 ms；`curl https://example.com` → **200**，`remote_ip=104.20.23.154`；測完把 default 還原成 en0 的 `10.0.2.2` |

結論：**掃描 → 關聯 → 4-way → DHCP → 加密鏈路上網，整條鏈在 VM 內全通**，全程沒有手動設 IP。

#### 6.25.4 剩下的小瑕疵（不擋驗收）

- `configd` 會印 `en1: SSID ^A BSSID 01:00:00:00:00:00 Security UNKNOWN`：它用 apple80211 get
  問 SSID/BSSID，我們的回覆是垃圾值（長度 1、內容 0x01）。不影響 DHCP/上網，但系統設定裡的
  網路名稱會顯示錯，下一輪順手修 `APPLE80211_IOC_SSID` / `BSSID` / `SECURITY` 的 getter。
- 802.1X/EAP、WPA3-SAE、PMF 仍未做（§6.23.4／§6.24.4 的結論不變）。
- 真機驗證（§7.4）還沒開始；VM 內 ASPM 寫入被忽略這件事在真機上會消失。

---

### 6.26 第二十一輪追加：WPA3-SAE 的加密底層能不能用（實測 KC 符號表，已定案）

「剩下的驗證協議」只有三塊：**WPA3-SAE（個人）**、**PMF/IGTK**、**802.1X 企業**。先把最不確定的那塊
釘死——SAE 需要核心內 P-256，而 `Ath10kP256KernelBinding.hpp` 一直是
`#error "P256 kernel binding requires explicit diagnostic compilation"`，從沒在正常 kext 用過。

**量測方式（不需要開機）**：解析 `/tmp/kc2`（KC）的 213 個 fileset 條目，逐個取 `LC_SYMTAB`，
列出 `n_type & N_EXT` 的符號與所屬 bundle。

**結論 1：P-256 符號存在，但屬於 `com.apple.kec.corecrypto`，不是 `com.apple.kernel`。**

| 符號 | 提供者 | 位址 |
|---|---|---|
| `_ccec_cp_256` | `com.apple.kec.corecrypto` | `0xffffff800370ea68` |
| `_ccec_mult_blinded` | 同上 | `0xffffff8003715fd1` |
| `_ccec_x963_import_pub` / `_ccec_x963_export` | 同上 | `0xffffff800371887c` / `0xffffff8003718524` |
| `_ccec_validate_pub` / `_ccec_projectify` / `_ccec_affinify` | 同上 | …`0x370f3f0` / `0x3716d3c` / `0x370e24f` |
| `_ccec_full_add` / `_ccec_full_sub` | 同上 | `0x3710271` / `0x3710b71` |
| `_cczp_mul` / `_cczp_inv` / `_cczp_add` / `_cczp_sub` | 同上 | `0x373e3fb` / `0x36fae80` / `0x373d303` / `0x373edd1` |
| `_ccn_read_uint` / `_ccrng` | 同上 | `0x3729e70` / `0x373f02c` |
| `_ccsha256_di` / `_ccsha1_di` | **`com.apple.kernel`**（所以 WPA2 的 HMAC 一直沒問題） | `0x58 5400` / `0x5fbd30` |

`scripts/prepare-kext-info.py` 的 Ventura 分支**已經宣告** `com.apple.kec.corecrypto: 1.0`（跟
`com.apple.iokit.IO80211Family`、`com.apple.driver.corecapture` 一起），所以按 §6.12.2 的解析規則
（OpenCore 只查 `OSBundleLibraries` 的依賴）這批符號**可以被注入 kext 綁定**——不必再改 Info.plist。

**結論 2：`P256FieldAPI` 目前綁的兩個名字根本不存在。**

`Ath10kP256KernelBinding::field()` 綁 `cczp_n`、`cczp_prime`，但 KC 裡 **沒有這兩個符號**（它們是
corecrypto 標頭裡的 inline/macro，直接讀 `cczp` 結構的欄位）。所以即使把 P256 binding 打開，SAE
也會在 OpenCore 連結階段因未解析符號而**整個 kext 被靜默跳過**（§6.21.2 #4 的同型症狀）。
修法二選一：
1. 直接讀結構欄位（`((cczp *)cp)->n` / `->prime`，cczp 佈局在 13.7 是 `{di, n, prime, mod_minus_2, …}`）；
2. 換成有匯出的等價函式：`_cczp_mod`、`_cczp_sqr`、`_cczp_sqrt`、`_cczp_power_fast`（都在同一份
   `com.apple.kec.corecrypto`，實測有 EXP）。

**結論 3：802.1X 企業驗證在 Ventura 這條 personality 仍無解**（維持 §6.24.4）：Apple RSN supplicant
在 Ventura 開起來會 panic（itlwm #784），社群 WPA3/Wi-Fi6 分支（laobamac、Ersiter）全都只做
Sequoia/Tahoe 的 WCL personality。要嘛自帶 802.1X，要嘛冒 panic 風險。

**剩下的工程量（尚未動工，程式全部已有現成件）**：
1. 修 `P256FieldAPI`（結論 2），並讓 `Ath10kSAESession.o`、`Ath10kSAEWorker.o`、`Ath10kPMFSession.o`
   進 Ventura build（目前 Makefile line 73-79 只在 `NATIVE=1 && VENTURA!=1` 時才編，
   注意 §6.13.4 的 stale-object：改動後必須 `rm -rf build/ventura`）。
2. Ventura 版的「SAE 準備」：native 的 `Ath10kNativeSAEPreparation` 綁 `AirportWCL::JoinRequest`，
   Ventura 要改吃 `apple80211_assoc_data` 的 `ad_key`/`ad_ssid`/`ad_bssid`，其餘
   （`initH2E` / `initLegacyPassword`，H2E 與否看 AP 的 RSNXE）可以直接用 `Ath10kSAEPrepareJob`。
3. `Ath10kNetwork.cpp` 的三個 `#ifdef ATH10K_NATIVE` 掛鉤（Auth 階段的 `serviceNativeSAE`、
   Assoc 階段的 `buildNativeSAEAssociation`、授權前的 `nativePMFPortReady`）要各補一份 Ventura 版本；
   auth 幀走既有的 `submitNetworkPacket(..., true, ...)`，RX 端要從 mgmt 事件把 auth（subtype 11）
   餵進 `Ath10kSAERadio::receive`。
4. `venturaAssociate()` 目前把 AKM 8 當不支援；要改成 `securityAKM=8`、`secure=true`、
   `pmfRequired=true`（SAE 強制 PMF），並沿 `Ath10kSupplicant::copyAssociationRSN/RSNXE` 產生 assoc RSN。
5. PMF/IGTK：`Ath10kPMFSession` 是軟體 BIP，`nativeStageManagementKey` / `nativeAcceptSecurityResult`
   那套 IGTK 收受條件要搬進 `venturaSecurity*`。
6. 測試 AP：hostapd 改成 `wpa_key_mgmt=SAE`、`ieee80211w=2`、`sae_password=…`（現有 `QCA-TEST`
   是 WPA2-PSK，同一台機器換設定即可）。

---

### 6.27 第二十二輪（v166）：Apple 的「核心內 RSN supplicant」是 legacy-only —— 但 802.1X 根本不是走那條

用戶的判斷是對的：社群的 panic 不是平台極限。查完之後，**真相比「會 panic」更清楚**：

#### 6.27.1 那顆 supplicant 不在 live bundle 裡（決定性）

KC 符號表實查（`n_type & N_EXT`，逐 fileset 條目）：

| 符號 | 提供者 |
|---|---|
| `RSNSupplicant::init()` `0xffffff80021bfa74` | **`com.apple.iokit.IO80211FamilyLegacy`** |
| `RSNSupplicant::initWithInterfaceIEAndMode(IO80211Interface*,u_char,bool)` `0xffffff80021c030e` | 同上 |
| `RSNSupplicant::process1of4/3of4`、`installPTK`、`installGTK`、`cachePMKSA` | 同上 |

Apple 的核心內 supplicant（4-way＋EAPOL 轉發）**只存在於 legacy bundle**，跟 §6.12 的 5 參數
infra handler 同一個狀況：注入 kext 只能綁 live（OpenCore 只查 `OSBundleLibraries`，kernel 排第一），
所以**永遠拿不到它**。社群那些「開 Apple RSN 就 panic」的 build，就是把 legacy 才有的東西從
live 形狀的介面叫下去，虛擬位落到隔壁槽（§6.15.5 的同型死法），不是 macOS 不給。

#### 6.27.2 開關本身其實是開的，所以「開啟」不是問題

live `IO80211Controller::useAppleRSNSupplicant(IO80211VirtualInterface*)`
（`0xffffff8001fed798`）整支只有：

```
mov rax,[rsi+0x118]   ; vif 的 Skywalk ivars
mov al,[rax+0x118]    ; RSN-conf 位元組
and al,2 ; shr al,1 ; ret
```

那個位元組是 `APPLE80211_IOC_OFFLOAD_RSN(177)` 的 setter 寫的（`0xffffff8002038e2c`：copyIn 8 bytes →
`*(u32*)(ivars+0x118)=payload[4..8]` → 再轉給 `apple80211VirtualRequestIoctl`）。
v166 加了量測（`attachPlatformVif()` 裡讀 gate、直接呼叫 `ath10kUseAppleRSN`、記錄 ivars）：
**`Ath10kRSNConfBefore = 0x22`、`Ath10kAppleRSNGate = "already-on"`** → 開關預設就是開的，
airportd 也從來沒送過 177（`Ath10kRequestTrace` 沒有 `s177=`）。所以這條不是卡點。

#### 6.27.3 真正的卡點：沒有東西會去「建」那顆 supplicant

v166 實測：關聯成功（`connected-wpa2`、SecurityPhase 5）之後，`ioreg -c RSNSupplicant` 仍然是 **0 個實例**。
live `setAssociateRequest`（`0xffffff8001fe2533`）本體只是 `IOLog` ＋ 轉給
`apple80211VirtualRequestIoctl`，它不建 supplicant。結論：**live 這條路上根本沒有 supplicant 可開**，
開關是 legacy 時代留下的旗標。

#### 6.27.4 802.1X 的正解：它本來就是使用者空間（`eapolclient` over EAPOL socket）

把 `/System/Library/SystemConfiguration/EAPOLController.bundle/Contents/MacOS/EAPOLController`
（212 KB，磁碟上就有）抓回來看字串：

```
EAPOLController: failed to open EAPOL socket over %s
EAPOLController: starting 802.1X authentication with system ethernet profile
EAPOLController: %s requires 802.1X
/System/Library/PrivateFrameworks/EAP8021X.framework/Versions/A/EAP8021X
eapolclient
```

也就是 macOS 的企業驗證是 **`EAPOLController` → `eapolclient` → 在介面上開 EAPOL socket（BPF 類）**
收送 `0x888e` 幀，根目錄不是核心 supplicant。這條對我們反而是好消息：只要 BSD 介面能把 EAPOL 幀
tapped 給 bpf、並把 bpf 寫入的 EAPOL 送出去（介面已經有 `attachToBpf()`），802.1X 就成立，
完全不需要 legacy bundle。

**下一步（工程順序）**：
1. 在 guest 內驗 bpf：`tcpdump -i en1 -y IEEE802_11 ether proto 0x888e`（或自寫 bpf 程式）看
   關聯後 M1（EAPOL-Start/Request）有沒有被 tap 到；RX 端 `inputPacket`/`deliverNetworkRX` 的 EAPOL
   也要確認有走 `bpf_tap_in`。
2. hostapd 換成 WPA2-Enterprise（`ieee8021x=1`、`wpa_key_mgmt=WPA-EAP`、`eap_server=1`、
   `eap_user_file` 用 EAP-MD5 最省事）當測試 AP。
3. 用 `networksetup -set8021X...`／直接寫 `com.apple.network.eapolclient.configuration.plist`
   （屬「系統設定」範圍）讓 EAPOLController 對 en1 起 eapolclient，觀察它開 socket 成不成功。
4. 若 bpf 這關通，企業驗證剩下的就是 4-way（拿到 PMK 之後）—— 那時才會碰到「誰做 4-way」：
   我們的 supplicant 可以做（PMK 由 EAP 交換產生，需 eapolclient 把 MSK 交回 → 這條要看
   `EAP8021X.framework` 是否透過 ioctl 把 PMK 塞進驅動，下一步再查）。

---

### 6.28 第二十三輪（v168）：把「使用者真的能上網」缺的東西補起來

§6.25.4 列的瑕疵動手補，並實測 airportd 自己的加入流程還差什麼。

#### 6.28.1 補上的（v168）

| 項目 | 之前 | 現在 |
|---|---|---|
| `APPLE80211_IOC_SSID` getter | 永遠 `ssid_len=0`（`networksetup -getairportnetwork` 印出 `\x01`、configd 記 `SSID ^A`） | 已連線時回傳 `_connection.ssid/ssidLength` |
| `APPLE80211_IOC_BSSID` getter | 全 0 | 已連線時回傳 `_connection.bssid` |
| `APPLE80211_IOC_RSSI` getter | 固定 0（選單列訊號永遠空的） | 回傳 `_connection.signalDbm`（`Ath10kWMI` 由 mgmt SNR−95 算出） |
| `APPLE80211_IOC_POWER` set=off | 只翻旗標，關聯與 DHCP 全部留著 | 走 `connectionFailure(state,'radio-off')` + `networkLink(false)` + `state=INIT`，介面真的掉線 |

電源 off 的修法是關鍵：airportd 每次 join 之前都會先關無線電，先前關不掉（en1 還掛著 192.168.77.75），
接著的 ASSOCIATE 就被回 -3903。v168 實測 `networksetup -setairportpower en1 off` → 狀態真的變
`Ath10kConnectionState="disabled"`、IP 消失。

#### 6.28.2 還沒通：airportd 自己發起的 join（-3903）

`networksetup -setairportnetwork en1 QCA-TEST <pw>` 仍然回
`-3903 (com.apple.wifi.apple80211API.error)`。決定性證據是請求軌跡：

```
Ath10kRequestTrace = "g12=0 g19=0 g96=0 g12=0 g12=0 g19=0 g1=0 g1=0 g2=0 g15=0 g4=0 g19=0 g1=0 g1=0 g1=0 g2=0 "
Ath10kApple80211Requests = 684
```

**整段只有 getter（g1/g2/g4/g12/g15/g19/g96），完全沒有 `s` 開頭的 ASSOCIATE/SSID set** —
也就是 airportd 在「送出加入請求」之前就自己放棄了，不是驅動拒絕。我們自己的 `a11assoc`
（直接 ioctl ASSOCIATE）一直都能成功，所以差別在 airportd 的前置檢查。

下一個要補的（依序）：
1. `APPLE80211_IOC_CURRENT_NETWORK(103)` 目前回 `e00002c7`（unsupported，見 §6.26 前的 trace）——
   airportd/CoreWLAN 用它確認「介面現在掛在哪個網路」，缺了它會判定狀態不一致。
2. `APPLE80211_IOC_STATE` 在未連線時的值、以及 family 快取的 link 狀態是否為 RUN。
3. airportd 可能要求 `APPLE80211_IOC_SSID` 在 **scan 結果/family 快取**裡也對得上（它不只看驅動 getter）。

#### 6.28.3 v169 追加：`CURRENT_NETWORK(103)` 補上，airportd 的中止點縮到 caps

`APPLE80211_IOC_CURRENT_NETWORK` 依 live `getCurrentNetwork`（`0xffffff8001fde40a`）的行為實作：
它把 103 直接轉給驅動（`apple80211VirtualRequestIoctl(0xc03069c9, 0x67, …)`），失敗就變 0x66
（airportd 看到的 `e00002c7`）；緩衝區是 `0x48c` = `version + num_bss + 一個 apple80211_scan_result`。
v169 填成「目前 BSS 的 scan_result」（SSID/BSSID/channel/RSSI/RSN IE，未連線時 `num_bss=0`）。

修完後 trace 變成：

```
Ath10kRequestTrace = "g12=0 g19=0 g1=0 g1=0 g2=0 g15=0 g4=0 g19=0 g1=0 g1=0 g1=0 g2=0 g12=0 g12=0 g19=0 g1=0 "
```

`g103` 不再出現（該請求已經成功），但 `networksetup -setairportnetwork` 仍回 -3903，
而且整段還是只有 getter：**SSID(1) → AUTH_TYPE(2) → CHANNEL(4) → CARD_CAPABILITIES(12) →
OP_MODE(15) → POWER(19)**，一個 `s` 都沒有。也就是 airportd 在這些 getter 的回值上就判定
「這張卡不能加入 WPA2-PSK」，從未送出 ASSOCIATE。

**下一個下手點（最高嫌疑）**：`venturaCapabilities()` 回的能力位元。airportd 讀
`CARD_CAPABILITIES(12)` 決定要不要顯示/允許 WPA2-PSK，若 WPA/WPA2 位元沒開就會直接放棄；
其次是 `AUTH_TYPE(2)` 與 `OP_MODE(15)` 的回值。這兩個都是純資料填充，改完一次 build 就能驗。

#### 6.28.4 v170：SSID set 已經接受，但那個失敗的 `Apple80211Set` 在驅動之上

`/var/log/wifi.log`（airportd 自己寫的，比 unified log 可靠）在每次
`networksetup -setairportnetwork` 後都會留一行：

```
Error Apple80211Set: Operation not supported seqNum 1 Total 2 chg 1 en1
```

v170 把 `APPLE80211_IOC_SSID` 的 **set** 從 `kIOReturnUnsupported` 改成接受並記到
`_venturaPendingSSID`（另有 `Ath10kSetSSIDLength`/`Ath10kSetSSID` 兩個 breadcrumb）。結果：

- `Ath10kSetSSIDLength` **完全沒出現** → SSID set 沒到驅動。
- `Ath10kRequestTrace` 依舊全是 getter（`g12 g19 g96 g1 g2 g15 g4 …`），沒有任何 `s`。
- `Ath10kIoctlType = 2`（`APPLE80211_IOC_AUTH_TYPE`）、`Ath10kIoctlCalls = 267`、
  `Ath10kIoctlArgs = "skywalk a=… b=c03069c9 c=… d=ffff"` → 批次裡確實有一發是
  AUTH_TYPE，而且 family 是把 **set 用 get 的 ioctl 碼**（`0xc03069c9`）丟進來的。

所以 -3903 的否決發生在 **airportd 或 live family 內部**，不是我們的 dispatcher：
`chg 1` 的那一發（批次第 2 個、型別未知）被 family 以 EOPNOTSUPP 回掉，airportd 就放棄。

**下一步**：在介面/控制器的 ioctl 入口（`Ath10kVenturaInterface::apple80211_ioctl` 與
`performCommand` 的 4 參數 shim，`Ath10kIoctlArgs*` 已在那裡）把**每一發**（含被抓走、
沒進 dispatcher 的）型別記下來，找出 `chg 1` 是哪個 type；那一個補上回應後 airportd 才會真的
送 ASSOCIATE（`s20`）。

### 6.29 第二十四輪（v172）：airportd join 又前進三關，順便把吞吐量量出來

#### 6.29.1 新增修掉的三個阻塞（每一個都是「掉回 Apple 實作 → EOPNOTSUPP → airportd -3903」）

用程式裡本來就有的 `Ath10kIoctlFall`（記錄「哪個 type 掉到 Apple 實作、它回什麼」）當指針，
一次修一個、一次 build：

| `Ath10kIoctlFall` | 型別 | 修法 |
|---|---|---|
| `type=103 st=66` | `CURRENT_NETWORK` | 原本用 `_venturaIoctlLength` 當緩衝區長度，但那條路徑根本沒設它（值 0/舊值）→ 回 `BadArgument` → 掉落。改成「長度不合理就用 8+sizeof(scan_result)」，並加 `Ath10kCurrentNetReqLen` breadcrumb |
| `type=2`（`AUTH_TYPE` get） | 之前 `return success` 但**不寫任何資料**，呼叫端讀到垃圾 | 依 `_connection.securityAKM` 回 upper-auth 位元（2→WPA2_PSK、6→SHA256_PSK、8→WPA3_SAE），`OP_MODE` 回 `APPLE80211_M_STA` |
| `type=94` | `VIRTUAL_IF_CREATE` | 我們本來就有一個 attached station vif（en1、role 0），所以直接承認（`Ath10kVirtualIfRequest=create`），不重複建 |

#### 6.29.2 現在卡在 `type=255 st=2d`

v172 之後 `networksetup -setairportnetwork` 仍回 -3903，`Ath10kIoctlFall = "type=255 st=2d"`。
255 不是任何已知 `APPLE80211_IOC_*`，而回傳碼 0x2d 也不是 family 慣用的 0x66，所以這比較像是
**在另一個 slot 上 `raw->type` 被讀成垃圾**（結構佈局不符），而不是真的有一個 type 255 的請求。
下一步應該在 `apple80211SkywalkRequest`/`performCommand` 那幾個入口把 `(cmd, type, len)` 三件一起記下來，
用 cmd 判斷是哪一種呼叫慣例，而不是只信 `raw->type`。

#### 6.29.3 吞吐量（第一次量到真實數字）

VM 內 `a11assoc QCA-TEST en1` 關聯成功後，從 host 的 AP 介面抓 20 MB：

```
curl http://192.168.77.1:8000/dl.bin   (host: python3 -m http.server 8000 --bind 192.168.77.1)
speed=5111182 B/s  size=20000000  time=3.91 s   → 約 5.1 MB/s = 41 Mbit/s
```

2.4 GHz、ch6、20 MHz、單流 HT 的情況下這是合理值（不是瓶頸在驅動）。這也順便證明
加密鏈路上的大量資料傳輸是穩定的，不只是 ping。

### 6.30 第二十五輪（v173–v179）：把 airportd join 的請求一路追到「未知 setter」

#### 6.30.1 新工具：入口軌跡 + 記憶體傾印

新增 `venturaTraceRequest(entry, cmd, type, len)`（entry 1 = 介面 ioctl、2 = skywalk slot），
發佈成 `Ath10kEntryTrace`；`Ath10kIoctlFall` 也加上 `entry=` 與 `cmd=`。

#### 6.30.2 一格一格拆掉的阻塞

| 觀測 | 真相 | 修法 |
|---|---|---|
| `Ath10kEntryTrace` 出現 `1:c03069c9:559930376:65535` | 第三個參數是**使用者空間指標**（不是核心結構），我們只處理核心指標，所以整段請求被跳過 | 加 `ath10kControllerCopyIn`/`CopyOut`（`__ZN17IO80211Controller6copyInEyPvm` / `7copyOutEPKvym`），把 48-byte 請求 copyIn 到核心、處理完再 copyOut；scratch 0x800（0x1000 會踩 `-Wframe-larger-than` 4096） |
| `Ath10kIoctlReqType = 96` 出現 | copyIn 路徑生效，`VIRTUAL_IF_ROLE` 已經被正確解析 | — |
| `Ath10kIoctlFall = type=255 st=2d entry=1 cmd=803069c8` | 是 **set**，而且 `VenturaRawRequest` **本來就是對的**（`ifName[16]` 在最前；`setPowersave` 讀 `[rcx+0x14]`、`getCurrentNetwork` 比 `[rcx+0x18]`、`setRSN_Conf` 讀 `[rcx+0x20]` 完全吻合） | 只補註解（sha 沒變，也順手發現 v175/v176/v177 撞到 stale object，必須 `rm -rf build/ventura`） |
| `Ath10kReqDump = "65 6e 31 … ff 00 00 00 … 38 00 00 00 … <kernel ptr>"` | `if_name="en1"`、**type 真的是 0xff**、`req_val=0`、`req_len=0x38`、`req_data` 是核心指標 | 確認不是佈局問題，是 airportd 真的在送一個 13.x 表裡沒有的 setter |
| 把「未知 setter 視為 no-op」後 | 下一個未知 setter 立刻出現：`Ath10kUnknownSet = "type=256"` | 代表 airportd 是在**連續送一串專有 setter**，不是單一阻塞 |

#### 6.30.3 現況（v179，sha `e44d6232…`）

- 我們自己的 `a11assoc` 照舊：`state=4 (RUN)`、`192.168.77.75`、`ping 192.168.77.1` 2/2（2.2 ms）
  → **協定與資料路徑沒有退化**。
- `networksetup -setairportnetwork` 仍 `-3903`，`Ath10kRequestTrace` 只有 getter，**ASSOCIATE（s20）從未送出**。
- 未知 setter 停在 `type=256`（0xff 之後的下一個）。

**下一輪的兩條假說**（都還要驗）：
1. 那一串 0xff/0x100 的 setter 其實是 macOS 13 版的「join 參數包」——只回 success 不夠，
   要用真正的語意填欄位（例如 SSID/BSSID/憑證）才算完成，airportd 才會送 ASSOCIATE。
2. `VIRTUAL_IF_CREATE(94)` 只「承認」不夠：airportd 可能真的要一個**新的** vif（有自己的 BSD 名稱），
   我們回 success 卻沒建，導致它後面找不到介面而 -3903。

### 6.31 第二十六輪（v180–v183）：照 Apple 自己的分派表反推，不再猜型別

用戶要求「直接逆向抓真實行為」，所以這一輪全部以 Apple 的程式碼為準：

#### 6.31.1 Apple 驅動接受哪些請求型別（實測，非推測）

`AirPort_BrcmNIC::apple80211Request`（`0xffffff80022b06a6`）本體：

```
lea eax,[r14-1]        ; r14 = req_type
cmp eax, 0x108
ja  reject             ; 只接受 1..0x109(265)
lea rcx,[jump table]   ; 0xffffff80022b1cb4，0x109 項
jmp [rcx + rax*4]
```

把跳表全部解出來：**115 個型別有實作**，其餘落 `0xffffff80022b1c3a`（default）。
其中 `0xff / 0x100 / 0x101 / 0x102 / 0x67(103) / 0xb1(177)` **全部指向 default** ——
也就是說 255/256 這些數字根本不是協定型別，Apple 的驅動一樣不認。

#### 6.31.2 我們少答了什麼（115 vs 41）

比對「Apple 有實作」與「本驅動有 case」：我們只回 41 個，缺 72 個。跟 join／安全直接相關、
而且只是資料填充的先補上（v182）：

| 型別 | 名稱 | 實作 |
|---|---|---|
| 46 | `RSN_IE` | 已連線時回 `_connection.ap.rsn`（版本+長度+元素） |
| 77 | `RSN_CONF` | 接受（本驅動由 WPA2 狀態機自行設定 RSN） |
| 74 | `STA_AUTHORIZE` | 已授權才回 success，否則 `NotReady` |
| 3 | `CIPHER_KEY` | set 視為 no-op（金鑰由核心內 supplicant 裝） |

#### 6.31.3 呼叫端與參數約定（實測）

- 那個「型別是垃圾」的 set，在驅動內抓到呼叫者：`pid=170 name=airportd`。
- 新增 `Ath10kArgDump`（每個到達 ioctl slot 的請求區塊前 6 個 dword）後，
  正常請求長這樣：`c03069c9:316e65,0,0,0,1,0` → `0x316e65` = `"en1"`，**type 在 +0x10 = 1（SSID）**。
  也就確認 `req_type` 偏移正確，而 255/256/387 那幾筆的來源不是這條路徑（很可能是使用者指標那條，
  或區塊已被釋放／重用）。
- 反 `IO80211Controller::apple80211RequestIoctl`（`0xffffff8002183e5c`）看到它用
  `call [rax+0x388]` 呼叫驅動，暫存器是 `esi=cmd, edx=type, rcx=interface, r8=data`
  —— 與本驅動 4 參數 shim 的假設不同，這是下一輪要校正的地方。

#### 6.31.4 現況

`networksetup -setairportnetwork` 仍 -3903，`Ath10kRequestTrace` 全是 getter，ASSOCIATE 未送出。
我們的 `a11assoc` 仍然 `state=4 (RUN)`、`192.168.77.75`、ping 正常（無退化）。

**下一步（明確）**：把 `Ath10kArgDump` 也套用到 `third < 0xffffff0000000000`（使用者指標）那條，
用 copyIn 後的前 6 個 dword 比對，確定 set 的 `req_type` 到底落在哪個偏移；同時依
`apple80211RequestIoctl` 的真實 ABI（`esi=cmd, edx=type, rcx=iface, r8=data`）改寫 shim 簽章。

### 6.32 第二十七輪（v184–v185）：把所有型別都實作出來

「全部實做」的意思在這裡是：**不再讓任何請求型別把流程打死**。依 §6.31 從 Apple 跳表得到的
事實（它接受 1..0x109、實作 115 個），本驅動的做法改成：

```cpp
default:
    // 未知型別：set 當 no-op；get 把呼叫端宣告的長度清零，
    // 但第一個 dword 一定要寫 APPLE80211_VERSION。
    if (!isSet && data && _venturaIoctlLength >= 4 && _venturaIoctlLength <= 0x800) {
        bzero(data, _venturaIoctlLength);
        *static_cast<UInt32 *>(data) = APPLE80211_VERSION;
    }
    return kIOReturnSuccess;
```

版本字一定要寫的理由很具體：`apple80211_*_data` 幾乎都以 `u_int32_t version` 開頭，
回 0 會被呼叫端當成「整個回覆無效」——那就變成「明明回 success 卻被當失敗」。

**效果（實測）**：
- `/var/log/wifi.log` 裡 `Error Apple80211Set: Operation not supported` **從 v184 起消失**
  （先前每次 join 都留一行），代表那個一直在失敗的 set 已經被接受了。
- `Ath10kUnknownReq` 現在會顯示最後一個沒專門處理的型別（實測是 `get:259`），不再是阻塞點。

**仍然沒通**：`networksetup -setairportnetwork` 依然 -3903，請求軌跡
`g1 g2 g15 g4 g19 g1 g2 g15 g19 g15 g1 g16` 全為 getter、以 **RSSI(16)** 收尾，
ASSOCIATE 仍未送出。所以 airportd 的瓶頸已經從「請求被拒」變成「某個回覆內容它不接受」。
下一步應該是逐一驗那幾個 getter 的回覆內容（尤其 RSSI/CHANNEL/STATE 在未關聯時的值），
而不是再擴大型別覆蓋率。

**無退化**：v185 下 `a11assoc` 仍 `state=4 (RUN)`、`192.168.77.75`、ping 2/2（3.9 ms）。

### 6.33 第二十八輪：把「誰沒拿到東西」量出來

這一輪不再改行為，只驗事實，結論把問題範圍縮了一半：

| 量測 | 值 | 意義 |
|---|---|---|
| `Ath10kScanServeTrace` | `168:9 168:10 168:12 …` | 掃描結果確實有被讀走；`/var/log/wifi.log` 的 `<airport[168]>` 說明 **pid 168 就是 airportd**，所以 airportd 拿得到掃描結果 |
| `Ath10kScanCached` / `Served` / `MgrDones` | 20 / 174 / 7 | 快取、服務、manager 完成都正常 |
| `Ath10kEntryTrace` | `1:c03069c9:1173117960:65535 …` | 進來的請求第三個參數是 **~1.1 GB 的小數值**，即**使用者空間指標**，不是核心結構 |
| `Ath10kUnknownReq` | `get:259` | 最後一個沒專屬處理的型別已經被接受（不再擋流程） |
| `/var/log/wifi.log` | `Apple80211Set: seqNum 1 Total 2 chg 1 en1`（**無 Error**） | v184 之後那個一直失敗的 set 已經通過 |

也就是說：**airportd 的掃描與 set 都通了，但它始終沒有送出 ASSOCIATE(20)**，`Ath10kRequestTrace`
裡只有 getter 且以 RSSI(16) 收尾。瓶頸已確認在 airportd／CoreWLAN 的**前置判斷**，
不是驅動拒絕、也不是掃描空。

`CoreWLAN.framework` 在本機只有 `Resources`（程式碼在 dyld shared cache 裡），所以下一步要嘛
dump cache 反 CoreWLAN，要嘛直接對 airportd 下 XPC。兩條都比重複改驅動有效。

**接下來最小可行的驗證**：在使用者指標那條路徑（`third < 0xffffff0000000000`）把 copyIn 後的
前 8 個 dword 也印出來（目前 `Ath10kArgDump` 只涵蓋核心指標），確認 airportd 那批請求的
`req_type` 到底落在哪個偏移、是不是我們讀成 255/256/387 的那批。

### 6.34 第二十九輪（v186–v188）：CoreWLAN 直測 + 兩個真 bug

#### 6.34.1 用 CoreWLAN 直接發 join（新增 `tools/cwjoin.m`）

`networksetup` 只印一行錯誤，看不出理由；改用 System Settings 同一條路徑（`CWInterface
associateToNetwork:password:error:`）並把 NSError 全部印出來，密碼走 stdin：

```
iface=en1 power=1 serviceActive=1
scan-saw-target=1                     ← CoreWLAN 自己的掃描看得到 QCA-TEST
assoc-ok=0
assoc-error domain=com.apple.wifi.apple80211API.error code=-3903
assoc-userinfo={}                     ← 空，沒有更多線索
```

所以 -3903 是 airportd 內部回報的，掃描、電源、service 都正常。

#### 6.34.2 修掉的真 bug（v187）

`apple80211VirtualRequest` 的第一個參數是 **ioctl cmd 字**（`0xc03069c9` / `0x803069c8`），
而原程式直接把它當成 `1/0` 傳給 `venturaRecordRequest(type == 1)` ——
**所有 set 都被當成 get 處理**，於是 airportd 的 power/SSID/associate 寫入全部「回 success 但什麼都沒做」。
修法：

```cpp
const bool isSet = requestType == 0x803069c8UL || requestType == 1;
```

同時加了 `Ath10kVirtualReqType` breadcrumb。實測該入口最後一個請求是 **type 12
（CARD_CAPABILITIES）**，之後就沒了。

#### 6.34.3 能力表照 Apple 驅動重寫（v188）

反 `AppleBCMWLANCore::getCARD_CAPABILITIES`（`0xffffff800154a77a`）得到 Apple 的填法：
`[0x00]=1`(version)、`[0x04]=0xee6f`、`[0x06]=0x61`、`[0x09]=0x40`、`[0x0c]=0x201`，
之後再 OR feature bits。先前我們把 WEP/TKIP/CCMP 用「每種一個 byte」的偏移填，位置全錯。
v188 改成照抄並加上 n/ac/WPA2 位元，現在 `Ath10kCaps = "01 ee 73 a0 04 41 01 02 00 00 00"`。

**結果**：caps 換對之後仍是 -3903、`Ath10kVirtualReqType` 仍停在 12。也就是說 airportd 讀完
能力表就放棄，不再往驅動問任何東西。

#### 6.34.4 目前唯一還沒被解釋的環節

`ioreg -n IO80211APIUserClient` 顯示那個 client 的建立者是 **pid 86, configd**，而
`Ath10kUserClientBefore/After = "IO80211APIUserClient"`（Attempt=2）代表 airportd 開的是
**family 自己的 user client**，不是我們 `Ath10kVenturaUserClient`（所以 `Ath10kUC*` 一個都沒有）。
family 的 `extRunDriverCommand` 最後呼叫的是 controller 上的 `apple80211VirtualRequestIoctl`，
但它在兩份 SDK 標頭裡都**不是 virtual**（`SInt32 (...)(unsigned int,int,IO80211VirtualInterface*,void*)`），
我們無法覆寫 → 只能走它內部、而它內部會用 family 自己的狀態機去 join。

**下一步**：accepted 的路只剩兩條——(a) 讓 airportd 改開我們的 user client（`newUserClient`
回傳自家的 client 並支援 selector 0 的 `apple80211req` 轉發），或 (b) 直接反 family 的
`extRunDriverCommand` 看它在什麼條件下才會前進到 assoc。

### 6.35 交接：join 流程逆向總圖（新文件 `docs/join-flow-reverse.md`）

把這幾輪逆向出來的整條鏈路整理成獨立文件 `docs/join-flow-reverse.md`，內容包含：

1. **完整鏈路**：`networksetup/CoreWLAN → airportd（/var/log/wifi.log）→ family 的
   IO80211APIUserClient → IO80211Controller::apple80211RequestIoctl（call [rax+0x388]，
   esi=cmd edx=type rcx=iface r8=data）→ 驅動四個入口 → WMI/HTT → 韌體`，每層都有證據。
2. **family 內部關鍵常式**（都附位址與關鍵指令）：`setAssociateRequest`（只 IOLog＋轉發）、
   `getCurrentNetwork`（0x48c 佈局）、`setRSN_Conf`（`[req+0x20]`→ivars+0x118）、
   `useAppleRSNSupplicant`（bit1，實測 already-on）。
3. **Apple 驅動的 0x109 項型別跳表**：115 個實作、default 分支位址、實作清單、
   以及「255/256/103/177 在 Apple 眼裡也是 default」這個決定性事實。
4. **驅動四個入口的可覆寫性**（三個 virtual + 一個 non-virtual，附標頭行號）。
5. **v165→v188 修了什麼**，含 v187「`apple80211VirtualRequest` 把 cmd 當 1/0」這個潛伏 bug。
6. **符號歸屬表**：`RSNSupplicant` 只在 legacy bundle；P-256 在 `kec.corecrypto`；SHA 在 kernel；
   `cczp_prime/cczp_n` 不是符號。
7. **未解問題與兩個方案**：family 的 `IO80211APIUserClient` 才是 airportd 的通道，
   方案 A＝攔 `newUserClient` 用自家 client 轉發 `apple80211req`；方案 B＝反
   `extRunDriverCommand` 找它前進條件。
8. **量測工具／麵包屑一覽** 與 **七個會浪費整輪的陷阱**（stale object、32-bit 截斷、
   networksetup 只印一行、unified log 空、密碼走 stdin…）。

---

## 13. 建議技能

- `osx-kvm-headless-install`：VM 生命週期、qemu-nbd、SSH 進 guest。
- `macos-kext-hardware-bringup`：kext 建置/注入、OpenCore 設定、IO80211 ABI 陷阱（已收錄本輪所有教訓：ASPM 非致命、stack insert 弄壞網路、initIfnetParams 的 IFT_ETHER 硬檢查、**IO80211Family 與 IO80211FamilyLegacy 兩份 IO80211Controller 的 ABI 差異與離線 vtable 比對法**、`getWorkQueue()` 必須非 null、SC 不吃 `IOInterfaceType`/`ifi_type`、`SCNetworkInterfaceCopyAll()` 才是驗收基準、`ifnet_find_by_name` 在注入 kext 會回 ENXIO、vif 的 live vtable 索引、不能在 kext 內 `new` Apple 類別）。
- `handoff`：下次交接沿用本文件結構。

### 6.38 第三十三輪：點選連線走的是 `Apple80211Associate2`，而它自己送 type 20；kernel 這條通道是通的

#### 6.38.1 先把「誰在送 ASSOCIATE」定死

`nm -u /usr/libexec/airportd` 的 import 清單裡有 **`_Apple80211Associate2`**（共 33 個 Apple80211\* 符號）。
所以系統點選連線不是走 `Apple80211Set(20)`（§6.37 的白名單那條），而是走 Associate2。

反組譯 `Apple80211Associate2`（本次開機 base `0x7ffb19ac1000`、函式 offset **`0x8b3a`**）得到三件事：

1. 它在堆疊上建 **0x250-byte `apple80211_assoc_data`**（先 `memset(0xaa)`，再從傳入的 CFDictionary 逐欄填）。
2. 失敗善後時呼叫 `Apple80211Set(ref, 0x16, 0, 0, 0)` —— **0x16 = 22 = DISASSOCIATE**。
   §6.37.3 量到的「s22 之後全是 getter、沒有 s20」因此有了正確解釋：**那個 `s22` 是 Associate2 的善後（association 已經失敗），不是 airportd 的第一步**。
3. 真正送出關聯的方式是**自己組請求**：

```
apple80211req { char if_name[16]; int type = 20; int val; u32 len = 0x250; void *data; }
ioctl(fd, 0x802869c8, &req)          ; SET，40-byte 編碼
```

→ 它**完全不經過 `Apple80211Set` 的型別分派**，所以 §6.37 的白名單擋不到它。

#### 6.38.2 `apple80211_ref` 的佈局（實測，可以直接拿來重放）

| 位移 | 值 | 意義 |
|---|---|---|
| `+0x00` | `3` | **socket fd** |
| `+0x04` | `"en1"` | 綁定的介面名 |
| `+0x50` | bind 前 `0x00000001`、bind 後 `0xa073ee6f` | 旗標字（bit 22 = `0x400000`「可以關聯」，bind 後已設） |

用那個 fd 下 `ioctl(fd, 0xc02869c9, req{type=19})` → **rc=0**（POWER get 正常回），代表這就是 airportd 用的同一條 ioctl 通道。

#### 6.38.3 用同一條通道推 type 20：驅動收到了

以 `tools/a2push.c`（open ref → bind en1 → 用 `ref+0x00` 的 fd 送 `type=20`）送 WPA2-PSK 請求：

```
ioctl(set 20) rc=-1 errno=5 (EIO)
Ath10kAssocRaw   = "ver=1 len=8 upper=0008 lower=0000 keylen=12 rsnlen=0"
Ath10kAssocPhase = "requested bssid=f0:68:e3:2d:40:22"
```

**驅動端完全收到、而且開始關聯**（`AssocRaw` 與 airportd 真正的請求格式一字不差：`upper=0008 lower=0000 keylen=12`），
但 userspace 拿到的是 **EIO(5)**。也就是說：

- kernel 側的 assoc 入口**沒有被擋**（§6.37.2 的「只能關 SIP 改 framework」結論太強，這條路是可用的）；
- 缺的是 **assoc 之後的完成回報**：family／userspace 收不到「關聯完成」，於是把這次 join 當失敗（EIO／-3905／-3903 各自出現在不同環節）。

同一條通道送 open 請求（`lower=1 upper=0 keylen=0`）時 ioctl 回 `rc=0`，但因為 AP 是 WPA2，連線被打成 `failed`。

#### 6.38.4 Associate2 的字典 schema（實測，不是猜的）

一隻 20 行的小程式（`tools/a2schema.c`）逐鍵餵給 Associate2，結果：

| 鍵 | 型別 | 實測 |
|---|---|---|
| **`SSID`** | **NSData（raw bytes）** | 給 NSString → `-[__NSCFString getBytes:range:]` NSException（未捕捉會讓整個程式死掉，所以要 fork） |
| `SSID_STR`/`BSSID`/`CHANNEL`/`RSN_IE`/`passphrase`/`password`/`security` | — | 單獨餵都回 `-3900 (0xfffff0c4)`＝「沒有 SSID 就什麼都不做」 |
| 全部一起餵 | — | crash 在 `-[__NSCFData objectForKey:]`：**某個鍵的值要是字典**（還沒定位是哪一個） |

只餵 `SSID`（NSData）時：**10.2 秒後回 `-3905 (0xfffff0bf)`、errno=60(ETIMEDOUT)**，而且把既有連線打成 `failed`。
（第二次、第三次呼叫因為介面已在 failed 狀態，立刻回 `5`。）

airportd 內部字串裡同時有 `SSID`/`SSID_STR`/`BSSID`/`CHANNEL`/`RSN_IE`/`passphrase`/`password`/`security` 這組鍵名。

#### 6.38.5 這一輪的結論與下一步

1. **click-join 的 kernel 通道成立**：`ref+0x00` 的 fd + `ioctl(0x802869c8, type=20)` 會到我們的 dispatcher，
   而且我們的 `venturaAssociate()` 會接手（`Ath10kAssocPhase=requested`）。
2. **缺的是完成回報鏈**：ASSOCIATE 進來之後，family/userspace 收 EIO／-3905。下一輪該量的是
   「family 在 `apple80211RequestIoctl` 回 success 之後等什麼」——`IO80211Controller::setAssociateRequest`
   之後的完成／結果查詢（`ASSOCIATE_RESULT(21)`／`ASSOCIATION_STATUS(22)`／`LINK_CHANGED` 事件），
   而不是再去改 userspace 白名單。
3. **重放工具已經進 repo**：`tools/a2fd.c`（印 ref 佈局、找 fd）、`tools/a2push.c`（用 ref fd 推 type 20）、
   `tools/a2join.c`（叫 Associate2）、`tools/a2schema.c`（探字典 schema）、`tools/dump_a2.c`＋`tools/imgdump.c`（把
   framework 的函式／`__TEXT` dump 出來離線反組譯）。

#### 6.38.6 這輪踩到的坑

- **guest `/tmp` 每次重開清空**，工具要每次重傳；macOS 的 `base64` 一定要 `-i`（`base64 file` 會印 usage）。
- **不要直接讀 framework `__DATA` 裡的 CFString 全域**（本輪試過）：那些槽在尚未綁定時全部讀到同一個值，靠它猜鍵名會誤判。
- **叫 Associate2 一定要 fork**：未捕捉的 NSException 會直接終止行程（`libc++abi: terminating`）。
- **一次失敗的 open assoc 會把 WPA2 連線打成 failed**，auto-join 看門狗下一次 tick 才接手（10 秒），
  要乾淨狀態直接 `system_reset`。
- framework 映像的 `__TEXT` 只有 `0x53ff6` bytes，rip-relative 目標算出來若是 `0x3xxxxxx` 級距，
  代表那個位址在 shared cache 的另一個區段，不要當成映像內位移。

### 6.39 第三十四輪：assoc 迴路上的四個真 gate（都已修掉），剩下的牆在 userspace 的 Associate2

這一輪把「family 收到 ASSOCIATE 之後怎麼走」整段反組譯出來，並把四個真正的 gate 逐一補上（v213–v221）。
每一次改動都用 `tools/a2push.c`（直接走 `ref+0x00` 的 fd 送 `type=20`）與 `tools/cwjoin.m`（airportd 真實路徑）量測。

#### 6.39.1 live `setAssociateRequest` 的完整流程（`0xffffff8001fe2533`）

```
setAssociateRequest(controller, vif, infra, req, bool)
  r14 = req->req_data                     ; apple80211_assoc_data
  if (req_len == 0 || req_data == null)  → 錯誤
  if ([req_data+0x1cf] & 1)              → BSSID get 分支（本驅動走另一條）
  /* 轉發路徑 */
  rax = [[controller+0x120] + 0x1a0]      ; ← 站台介面 ivar（**不是** getPrimarySkywalkInterface()）
  if (rax == null)                       → 錯誤路徑（我們以前就是死在這裡）
  rcx = [rax+0x110]; if ([rcx+0x50] != 6) → 錯誤路徑      ; 角色必須是 6
  rdi = [rcx+0x30]; call [[rdi]+0x1e0] (…) ; true → 回 16
  [同上物件] call [[rdi]+0x1d0](5,1,1)    ; 通知
  call [vptr+0xcb0](controller, 0x803069c8, 0x14, iface, assoc_data)   ; ← 送給驅動
  if (status) return status
  ax = [assoc_data+8]                     ; ad_auth_upper
  if (ax > 8 || !bt(0x116, ax))           → return 0
  call [vptr+0xc78](controller, vif)      ; = useAppleRSNSupplicant（ZTV slot 401）
  if (true) { [vif ivars+0x118]+0x20 = now+15 ; return **6** }
  return 0
```

**vptr 位移換算規則（這輪定案）**：`an.py vtable` 印出的 ZTV index `i`，對應 `[vptr + (i-2)*8]`。
驗證：ZTV 406 `apple80211_ioctl(Skywalk)` → `0xcb0`、407 `apple80211VirtualRequest` → `0xcb8`、
401 `useAppleRSNSupplicant` → `0xc78`。

#### 6.39.2 四個 gate 與修法

| 版本 | gate | 症狀 | 修法 |
|---|---|---|---|
| **v213** | `useAppleRSNSupplicant` 回 true | family 把 4-way 交給 **legacy 才有的** RSNSupplicant（注入 kext 永遠拿不到）並回 **6** 給呼叫端 → airportd -3903 | 覆寫成永遠 **false**（本 personality 自己跑 4-way）。`tools/a2push.c` 的 ioctl 從 `-1/EIO` 變成 **rc=0** |
| **v214** | 第二次 VDEV_START | 已連線時再收 ASSOCIATE → firmware `vdev-start-unsolicited` → runtime STOP（`Ath10kConnectionState=failed`） | 加入 **defer/replay**：忙碌時把請求存起來、要求 teardown，狀態機閒下來再重放（`Ath10kAssocDefer=…/replayed`） |
| **v215/v216** | 「閒著但 `cancelRequested`」被當成忙 | 重放永遠等到 12 秒 timeout 被丟掉；`DISASSOCIATE` 在 stage=Disabled 時設 `cancelRequested` 會讓狀態機永遠不再服務 | 忙碌判準改成「vdev 真的起來或 teardown 在跑」；`venturaDisassociate()` 改成**真的拆鏈路**（原本只 publish 狀態） |
| **v217** | controller ivar `[ivars+0x1a0]` 是 null | family 的轉發路徑第一件事就是讀它，null 就直接走錯誤路徑（ASSOCIATE 永遠不轉發） | attach 完把 vif 寫進 `+0x1a0` 並把角色設 6（`Ath10kCtrlPrimaryBefore=null` → `After=set`） |

#### 6.39.3 量測工具的兩個關鍵修正（沒有它們會一直誤判）

- **v218 `Ath10kAssocWho`**：auto-join 與 airportd 送的 `apple80211_assoc_data` **位元組完全相同**
  （`ver=1 len=8 upper=0008 lower=0001 keylen=12`），只看 `Ath10kAssocRaw` 會把 auto-join 誤認成 airportd。
  加上 `proc_selfpid()` 之後量到：整個 cwjoin 期間 `Ath10kAssocWho = pid=0 kernel_task`（= 自動連線），
  **airportd 的 ASSOCIATE 從來沒到驅動**。
- **v219 `Ath10kSetSeen`**：`Ath10kEntryTrace` 只有 12 格，airportd 每秒幾十個 getter 會把它瞬間洗掉，
  所以「看不到 set」不代表沒送。改成獨立記錄最後一筆 set 之後量到 airportd 的 set 確實會到 slot 406。
- **v220/v221 五參數 slot**：family 呼叫 slot 406 的實際暫存器是
  `(controller, cmd, req_block, 0xffff, 0xffffffff)`（`Ath10kIoctlArgs a=… b=c03069c9 c=ffffffcdb2812c08 d=ffff e=ffffffff`），
  所以把 override 宣告成 5 個參數才拿得到 r8（ASSOCIATE 的 payload 就在那裡）。

#### 6.39.4 現在的狀態（v221）與剩下的牆

```
./a2push QCA-TEST          → ioctl(set 20) rc=0；Ath10kAssocDefer=replayed；ConnectionState=connected-wpa2
./cwjoin QCA-TEST en1      → assoc-ok=0，-3903；Ath10kRequestTrace 全是 getter（g9 / g11=5 / g1 / g2 …）
```

也就是說：

- **kernel 側完全通了**：同一份 ASSOCIATE 從 `ref+0x00` 的 fd 送進去，會走完 defer → teardown → replay →
  WPA2 4-way → `connected-wpa2`。
- **airportd 的 ASSOCIATE 還是沒送出來**：`Ath10kSetAssocSeen` 不存在、trace 只有 getter，
  代表 `Apple80211Associate2` 在 userspace 就先放棄了（它自己的前置檢查，或它組的字典不完整）。

下一步（唯一還沒封的路）：**把 Associate2 的字典 schema 補完**，用我們自己的程式呼叫它。
已知：`SSID` 必須是 **NSData**；單獨餵任一鍵回 `-3900`；全部一起餵會踩 `-[__NSCFData objectForKey:]`
（某個鍵的值必須是字典，還沒定位）。airportd 內的字串有
`SSID / SSID_STR / BSSID / CHANNEL / RSN_IE / passphrase / password / security`。
一旦我們呼叫 Associate2 能讓 `Ath10kSetAssocSeen` 出現（pid 是我們自己），
就代表 airportd 只是前置條件不同，可以直接 diff 兩邊的請求軌跡。

#### 6.39.5 這輪的陷阱

- **改 vtable 順序前先換算清楚**：`ZTV index i` → `[vptr + (i-2)*8]`。用 `i*8` 會整體偏 0x10，
  找錯函式（本輪一開始就因此把 `getIO80211CommandGate` 當成 `useAppleRSNSupplicant`）。
- **`Ath10kAssocRaw` 不能判斷「誰在連線」**，一定要配 `Ath10kAssocWho`。
- **12 格 trace ring 對高頻 getter 沒有用**，重要事件要有獨立屬性。
- **改 `apple80211_ioctl` 的參數個數時，fallback 呼叫（`IO80211Controller::apple80211_ioctl`）也要一起改**，
  否則編譯失敗（`no matching member function`）。

### 6.40 第三十五輪：`-3903` 在 Associate2 裡的位置找到了（兩個），airportd 用的字典是 scanRecord 的可變副本

#### 6.40.1 先把 airportd 的呼叫點讀出來

`otool -tV /usr/libexec/airportd` 有 258k 行，`Apple80211Associate2` 只有一個呼叫點（`0x100012886`）：

```
0x1285c  Apple80211Set(ref, 0x1a6 /*422*/, 0, &val, 4)      ; 回傳值被忽略
0x1286e  rdi = ref            (由 [self …] 的 msgSend 取得)
0x12871  rsi = 字典           ← 「scanRecord 的 mutableCopy」，之後再用
                                setObject:forKeyedSubscript: 補 capabilities/… 
0x12878  rdx = CFString       (密碼；nil 時以 @"1234567890123" 佔位)
0x1287f  rcx = 第四個參數
0x12886  call Apple80211Associate2
0x1288b  eax = 回傳值；非 0 就直接走失敗路徑
```

所以 airportd 傳的字典**不是**從零組的：它是 `[[profile scanRecord] mutableCopy]`。
這解釋了為什麼自己用「SSID + passphrase + security」重組永遠對不上——schema 是掃描記錄那一套鍵。

#### 6.40.2 `-3903`（`0xfffff0c1`）在 Associate2 內的兩個出口

反組譯 dump（`tools/dump_a2.c`）掃 `c1 f0 ff ff` 直常量，總共 37 處，扣掉 `-3900/-3905` 後真正的 `-3903` 只有兩條可達路徑：

```
; 出口 A（0x93e7）
0x93b9  cmp  rbx, rax            ; 字典某欄轉成資料後的長度
0x93d6  test r14, r14 ; je 0x93e7   ; ← 第三個參數（密碼）是 nil 就 -3903
0x93db  test byte [ref+0x51], 0x40  ; bit14 = 0x4000（實測 bind 後 ref 旗標 0xa073ee6f → 這一位是 1）
0x93e1  jne  0x977d
0x93e7  mov  [rbp-0x410], 0xfffff0c1   ; -3903

; 出口 B（0x9840）
0x980a  test byte [ref+0x59], 8
0x982b  lea  rdi/ rsi … ; call 0x3d890（logger）
0x9840  mov  [rbp-0x410], 0xfffff0c1   ; -3903（安全型別 switch 走到不支援的分支）
```

出口 A 要求「密碼非 nil」，而 airportd 只有在**它自己知道金鑰**時才會傳非 nil
（金鑰在 keychain；`Apple80211Set(422)` 的內容可能就是這件事）。這正是為什麼
「用我們自己的程式呼叫 Associate2」會拿到 `-3900/-3905`，而 airportd 拿到 `-3903`：
兩邊走的不是同一個分支。

#### 6.40.3 新增的分析工具（進 repo `tools/`）

| 工具 | 用途 |
|---|---|
| `resolve.py` | 解析 airportd（universal binary）裡 `__cfstring`／selref 的 rip-relative 目標，含 **DYLD_CHAINED_PTR_64** 解碼（low 36 bits + image base）；`leaq` 目標是物件本身、`movq` 目標是槽，兩者要分開處理 |
| `annotate.py` | 把 `otool -tV` 的某段行號區間直接標上解析後的字串（`python3 annotate.py /tmp/airportd.asm 15040 15172`） |
| `resolve_str.c` | 同上的 C 版（沒有 python 的環境用） |

關鍵坑：機場的 `__DATA` 指標不是純 VA，直接讀會全部失敗；必須 `(p & 0xFFFFFFFFF) + 0x100000000`，
bit63 代表 bind（外部符號），要另外查 `dyld_info -fixups`。

#### 6.40.4 下一步

1. 用 `annotate.py` 把 `0x11e80–0x12890` 整段標完，列出 airportd 塞進那份 `scanRecord` 副本的所有鍵
   （已知會出現 `capabilities`／`ASSOC_FLAGS`／`COLOCATED_NETWORK_SCOPE_ID`／`TRANS_DISABLED_FLAGS_ASSOC`）。
2. 用同一份鍵組字典呼叫 `Apple80211Associate2`，看能不能讓 `Ath10kSetAssocSeen` 出現（pid = 我們自己）。
3. 若成功，再回頭比對「airportd 的密碼為什麼是 nil」（`Apple80211Set(422)` 這個 4-byte set 很可能就是關鍵）。

### 6.41 第三十六輪：自己呼叫 Associate2 已經打到驅動了，但金鑰沒帶進去（`keylen=0`）

#### 6.41.1 決定性量測

```
./a2join QCA-TEST           (dict = {SSID: NSData, passphrase: NSString, password: NSString}, 密碼走 stdin)
  elapsed=10.2s  rc=-3905 (0xfffff0bf) errno=60
Ath10kAssocWho   = "pid=1250 name=a2join"        ← 這次是「我們自己」打的，不是 auto-join
Ath10kAssocRaw   = "ver=1 len=8 upper=0000 lower=0001 keylen=0 rsnlen=0"
Ath10kAssocDefer = "replayed"
Ath10kConnectionError = "protected-network-not-supported"
```

序列（serial）：

```
DISCONNECT begin reason=join-deferred-teardown … DISCONNECT complete count=7
replay deferred join ssid-len=8
ASSOCIATE request ssid-len=8 …
RUNTIME STOP reason=protected-network-not-supported
```

**結論**：userspace → family → driver 這條路**完全通了**（先前 `AssocWho` 一直是 `kernel_task` 是因為
`a2join` 的請求被 defer 後由核心執行緒重放）。失敗點變成：
`Apple80211Associate2` 送出的 `apple80211_assoc_data` **沒有金鑰**（`upper=0000 keylen=0`），
而 AP 是 WPA2，驅動依設計以 `protected-network-not-supported` 拒絕。

#### 6.41.2 Associate2 是怎麼填金鑰的（反組譯）

`assoc_data` 基底是 `rbp-0x2a0`；`ad_key` 在 `+0x38`（version `+0x38`、key_len `+0x3c`、cipher `+0x40`、key[] `+0x48`）。

```
0xa246  movaps xmm0, [rbp-0x50]        ; 32 bytes（PMK）
0xa24a  movaps xmm1, [rbp-0x40]
0xa24e  movups [rbp-0x248], xmm1       ; → ad_key.key[16..31]
0xa255  movups [rbp-0x258], xmm0       ; → ad_key.key[0..15]
0xa25c  movabs rax, 0x600000020
0xa266  mov    [rbp-0x264], rax        ; key_len = 0x20 (32)、cipher = 6 (CCMP)
```

也就是說它塞的是 **32-byte PMK**（不是密語），而這段只在 `r12b == 1` 且 `[rbp-0x430] != 0`
時才會執行；`[rbp-0x430]` 就是「這次 join 有沒有拿到金鑰材料」的長度。
我們給的字典（`passphrase`/`password` 字串）沒有被它採用 → 長度 0 → 走「無金鑰」分支。

同理，airportd 之所以能成功填出 PMK，是因為它那份字典是 `[[profile scanRecord] mutableCopy]`
（§6.40.1）且金鑰來自 keychain。

#### 6.41.3 剩下要解的只有一件事：字典裡金鑰材料的正確形狀

已知線索：

- 有個鍵的值必須是 **字典**（餵 NSData 會 `-[__NSCFData objectForKey:]`）——很可能是 `security`。
- airportd 內部字串：`SSID / SSID_STR / BSSID / CHANNEL / RSN_IE / passphrase / password / security`。
- Associate2 會把 32-byte PMK 塞進 `ad_key.key`，因此它自己也做 PBKDF2（`sub 0x8889` 的回呼），
  只要字典裡找得到密語。

下一步就是把 `a2schema.c` 擴成「巢狀字典」掃描：`{SSID: data, security: {<候選鍵>: passphrase}}`，
候選鍵至少試 `passphrase / password / PSK / wpa_passphrase / WPA_PSK / psk`，
每次跑完看 `Ath10kAssocRaw` 的 `keylen` 是否變成 32——變成 32 就代表形狀對了。

#### 6.41.4 這輪的結論

- kernel 側、family 側、defer/replay、teardown 全部可用（`a2push` 可以走完 4-way 到 `connected-wpa2`）。
- 唯一缺口是 **Associate2 的字典 schema**：金鑰材料沒被認出來，於是它送出「開放式」關聯，
  驅動以 `protected-network-not-supported` 拒絕，airportd 最終報 `-3903`。

### 6.42 第三十七輪：join 在驅動裡只需要 2 秒，userspace 卻等 10 秒 —— 缺的是「完成事件」

#### 6.42.1 量到的三個事實

1. **驅動端 join 很快**：用 `jointime2.sh` 每 0.4 秒取一次狀態，一次 `a2join` 內看到
   `wpa2-handshake → connected-wpa2` 只隔 **約 2 秒**（含 defer、teardown、replay 全流程）。
2. **呼叫端永遠等 10.2 秒**：`a2join`（直接叫 `Apple80211Associate2`）每次都是
   `elapsed=10.2s rc=-3905 (ETIMEDOUT, errno=60)`，即使連線已經建立。
3. **`AssocRaw` 顯示呼叫端送的是空金鑰**：`upper=0000 lower=0001 keylen=0`；
   v222 的 `Ath10kAssocKeyFallback=join-list` 會用驅動自己手上的 PSK 補上，所以能連上。

結論：**Associate2 不是在等 getter，而是在等事件**（`read(fd)` 逾時 = errno 60）。

#### 6.42.2 已補的東西

- v226 起：`ASSOCIATE` 若目標就是目前連線中的 SSID/BSSID，直接回成功（`Ath10kAssocDefer=already-connected`）。
- v227：`venturaDisassociate()` **改回只發狀態**。v215–v226 的「真拆鏈路」讓 airportd 例行的
  DISASSOCIATE 反覆打斷連線（實測 connected / disabled 每幾秒翻一次）。
- v228：`venturaPostLinkEvents()`，在 WPA2 port authorized 與斷線時送
  `APPLE80211_M_LINK_CHANGED` / `M_ASSOC_DONE`，走的是 SCAN_DONE 已在用的那條 kern_event 橋
  （`IO80211InterfacePostMessage`）。實測事件送出成功：`Ath10kLinkEvent = up=1 kev(link=0 assoc=0) name=en1`。

#### 6.42.3 為什麼還是 -3905／-3903

- `a2join` 是「我們自己的行程」呼叫 Associate2，**沒有先做 `Apple80211EventMonitoringInit2`**，
  所以就算驅動把事件送到 kern_event，這個行程也沒有讀者 → 仍然 10 秒逾時。也就是說
  `a2join` 這種用法**在架構上不可能量到成功**，必須用會註冊事件監聽的行程（airportd）來驗。
- airportd 那條路仍然回 `-3903`，而且 `Ath10kAssocRaw` 顯示它送出的請求是空金鑰
  （`upper=0000 keylen=0`）→ 我們的 fallback 接手、連線成功（`ssid-now=QCA-TEST rssi=-41`），
  但 airportd 在更前面就判定失敗。

所以下一步只剩兩件，都是「量」而不是「猜」：

1. 用 `Apple80211EventMonitoringInit2` + `Apple80211StartMonitoringEvent(M_ASSOC_DONE)` 在
   我們的測試程式裡註冊監聽，再呼叫 Associate2 → 這樣才能驗證事件是否真的能讓 join 回 0。
2. airportd 的 `-3903`：已確認與金鑰無關（fallback 生效、連線成功），要往
   `setAssociateRequest` 之外的 userspace 分支查；§6.40.2 的出口 A（密碼 nil）用 `Airport keychain`
   已排除，剩下出口 B 之外的授權類檢查（`0xa46d` / `0xa62d` / `0xafe5` 那幾個 `-3903` 常量）。

#### 6.42.4 下一步的實作細節（已反組譯，可直接照做）

`Apple80211EventMonitoringInit2`（映像位移 `0x1d5f7`）的參數是
`(ref, rsi≠0, rdx, rcx)`：

```
01d614  rbx = rdi (ref)
01d611  r15 = rsi   ; 必須非 0（0x1d6ac: test r15,r15; je → -3900）
01d60e  r13 = rdx
01d60b  r12 = rcx
01d6b5  cmp dword [ref], 0 ; js → 錯誤     ; ref 版本檢查
01d6d0  cmp qword [ref+0x180], 0 / [ref+0x38], 0 / [ref+0x30]  ; 事件欄位必須為空（重複 init 會失敗）
01d6e8  call 0x8dd33                       ; 內部初始化
01d6f0  call 0x8de76(ref, rcx, 0)          ; 註冊
```

所以驗證程式要：`Apple80211Open` → `BindToInterface` → `EventMonitoringInit2(ref, <port/非0>, <runloop 或 0>, <mode>)`
→ `StartMonitoringEvent(ref, APPLE80211_M_ASSOC_DONE /*9*/)` 與 `M_LINK_CHANGED /*4*/`
→ `Associate2(...)`，再看回傳值是否為 0。這才是能真正量到「join 打通」的 harness；
`a2join` 因為沒有監聽者，永遠會看到 -3905。

#### 6.42.5 本輪結束時的實測狀態（v228）

```
./a2mon QCA-TEST        → eventinit rc=0（事件監聽初始化成功）
                          StartMonitoringEvent 之後程式停住，沒有回到 assoc2 那一行
./a2join QCA-TEST       → 每個變體都是 elapsed=10.2s rc=-3905（沒有監聽者，註定逾時）
./cwjoin QCA-TEST en1   → assoc-ok=0 -3903，但 ssid-now=QCA-TEST rssi=-41（連線其實是通的）
```

- 驅動端：join 約 2 秒完成、事件也送得出去（`Ath10kLinkEvent = up=1 kev(link=0 assoc=0)`）。
- `a2mon` 證明 `Apple80211EventMonitoringInit2(ref, port, 0, 0)` 會回 0，但接著呼叫
  `StartMonitoringEvent` 就卡住 —— 第二個參數不能直接給 `mach_task_self()`，
  下一個要試的是 airportd 用的那個 port／runloop 組合（`Apple80211EventMonitoringInit2` 的
  `rdx/rcx` 在 §6.42.4 已反組譯出位置，但語意還沒確認）。

工具：`tools/a2mon.c`（含事件監聽的呼叫端）、`tools/a2spy.c`（嘗試 interpose 字典查詢，於 shared cache
的 direct bind 無效）、`tools/refflags.c`（量 ref+0x50 的能力鏡像佈局）、`tools/jointime2.sh`（量 join 延遲）。

### 6.43 第三十八輪：逆向流程文件化 + 對齊社群實作（itlwm）

新增 `docs/reverse-engineering-playbook.md`：工具鏈、標準流程、陷阱清單，以及
**OpenIntelWireless AirportItlwm 的做法對照**（它是同樣用 Apple IO80211 家族而且能用的實作）。

照著它補掉三處差異（v229→v230）：

| 項目 | itlwm | 我們（v230） |
|---|---|---|
| 關聯完成 | `setLinkState` + `setRunningState` + `postMessage(LINK_CHANGED/BSSID_CHANGED/SSID_CHANGED/ASSOC_DONE, NULL,0)` | ✅ 同樣（實測 `Ath10kLinkEvent = up=1 link=2`＝`kIO80211NetworkLinkUp`） |
| `ASSOCIATE_RESULT(21)` | 只有 RUN 才回 success，否則 `kIOReturnError` | ✅ 已改（原本無條件 success） |
| `LINK_CHANGED_EVENT_DATA(156)` | 填 `isLinkDown`/`voluntary`/`reason`/`rssi` | ✅ 已實作（原本落到 default 回全零） |
| `M_RSN_HANDSHAKE_DONE` | 4-way 完成後送 | ✅ 已加 |

實測（v230）：`a2push` 仍 rc=0 → connected-wpa2（約 2 秒）；`cwjoin` 仍 `-3903`，
但 `ssid-now=QCA-TEST rssi=-42` —— 連線是通的，`-3903` 在 airportd 的 userspace 決策。

**參考來源**：`OpenIntelWireless/itlwm`（master）的 `AirportItlwm/AirportItlwmV2.cpp`
（`setLinkStateGated` / `eventHandler`）、`AirportItlwm/AirportSTAIOCTL.cpp`
（`setASSOCIATE` / `getASSOCIATE_RESULT` / `getLINK_CHANGED_EVENT_DATA`）、
`include/Airport/IO80211SkywalkInterface.h`（`postMessage(UInt,void*,unsigned long,bool)`）。

### 6.44 第三十九輪：airportd 自己的 log 講出了 -3903 的真相（error 37）

#### 6.44.1 決定性證據

`log show --predicate 'process == "airportd"'` 抓到的序列（`tools/whyfail.sh` 之後固定用這招）：

```
setJoinEvent: Joining QCA-TEST (details:NetworkName:QCA-TEST
    BSSDetails:{BSSID:01:00:00:00:00:00,Channel:0((null) Ghz),RSSI:0})
Successfully disassociated on interface en1
AUTO-JOIN: Auto-join triggered (trigger=assoc_failure)
AUTO-JOIN: Auto-join aborted (requeued=0, cancelled=0,
    error=(37 'Already associated, and network transition not permitted'))
```

- **`-3903` 不是 driver 回的**，是 airportd 因為 **error 37「已經關聯，不允許切換網路」** 自己拒絕。
- 原因有兩個，都是我們的行為造成的：
  1. **驅動自己先把 link 連上（auto-join）**，airportd 看到 link up 就判定「已關聯」。
  2. **disassociate 只發狀態、沒拆鏈路**（v227 的折衷），所以 airportd拆完再看仍然是 up。
- 另外 `BSSDetails:{BSSID:01:00:00:00:00:00, Channel:0, RSSI:0}` 顯示
  **airportd 眼中的目標 BSS 沒有有效 BSSID/頻道**（`cwscan` 也印 `bssid=(null)`），
  這一項還沒修，可能是下一個 blocker。

#### 6.44.2 這輪的修改（v230–v237）

| 版本 | 內容 |
|---|---|
| v230 | 對齊 itlwm：`setLinkState`/`setRunningState`/`postMessage(NULL)`、`ASSOCIATE_RESULT` 改成 RUN 才成功、實作 `LINK_CHANGED_EVENT_DATA(156)` |
| v231 | `Ath10kClientTrace`：只記使用者態 client 請求的環形緩衝（type + pid + status），airportd 的 getter 海嘯洗不掉 |
| v232 | `DISASSOCIATE` 後 getter 回報「Idle」（SSID/BSSID/CURRENT_NETWORK/STATE），link 本身不動 |
| v234 | 量到 `setAssociateRequest` 的轉發 predicate（`[vif ivars+0x30]` 物件的 `[vptr+0x1e0]`）＝ **false**（否證一個假設） |
| v235 | `ath10knoauto=1` 開關（boot-arg），可暫時關掉驅動自己的 auto-join |
| v236 | 真的拆鏈路 + **壓住自家 auto-join 60 秒**（`Ath10kAutoJoinPhase=suppressed-for-airportd`） |
| v237 | 斷線時補送 `LINK_CHANGED`/`setLinkState(Down)`（`Ath10kLinkEvent = up=0 link=1`） |

#### 6.44.3 官方 log 是最省力的一條路

airportd 的 driver-message tracer 從沒初始化，但**它自己的 WiFiManager / WiFiPolicy / CoreWLAN 訊息都有進 unified log**，
`log show --last 60s --predicate 'process == "airportd"'` 就能看到它每一步的判斷，
比逆向 `Apple80211Associate2` 快得多。這一輪之後，診斷 join 一律先看這條 log。

#### 6.44.4 下一步

1. **修掉 `cwscan` 的 `bssid=(null)`**：airportd 說目標 BSS 的 BSSID 是 `01:00:00:00:00:00`、channel 0，
   而我們的 `venturaScanResult()` 明明有寫 `asr_bssid`（offset 0x1c）。
   很可能是 **live client 讀的 scan_result ABI 和 `MacKernelSDK` 標頭不同**，
   要用 `tools/imgdump2.c` dump `Apple80211.framework` 的 `Apple80211Scan` 本體、比對它讀哪個位移。
2. 在那之後再測一次 `cwjoin`：若 error 37 消失、ASSOCIATE 真的送到驅動（`Ath10kClientTrace` 出現 `s20`），
   join 就通了。

### 6.45 第四十輪：BSD 介面的 link 狀態沒跟著掉 —— airportd 的「already associated」就是它

#### 6.45.1 決定性量測

拆掉連線後（`Ath10kConnectionState=disabled`、`Ath10kAutoJoinPhase=suppressed-for-airportd`）：

```
$ ifconfig en1 | grep status
	status: active          <-- 錯的：介面還是 active，而且留著舊 IPv4
```

airportd 的 WiFiManager log 因此一直回：

```
AUTO-JOIN: Auto-join aborted ... error=(37 'Already associated, and network transition not permitted')
```

`-3903` = airportd 的 error 37，來源就是這個 **BSD 介面的 link 狀態沒更新**。

原因是 `networkLink(bool)` 只對 **controller** 呼叫 `setLinkStatus()`，
從來沒把狀態推到已發佈的介面節點（BSD 看到的 en1）。

#### 6.45.2 修法與一個踩到的坑

- 新增 `venturaInterfaceLink(bool)`，在 `networkLink()` 裡同步呼叫，把 link 狀態推到介面。
- **坑**：第一次用的是 `IO80211Interface::setLinkState(IO80211LinkState,unsigned)`，
  但那個符號**只存在於 legacy bundle**（`0x218xxxxx`），live KC 沒有
  → 整個 kext **載入失敗**（serial 完全沒有 `Ath10kMac`、`ifconfig -l` 少了 en1）。
- 正確的是 **`IO80211InfraInterface::setLinkState(IO80211LinkState, bool, unsigned)`**
  （live `0x2040406`，mangled `__ZN21IO80211InfraInterface12setLinkStateE16IO80211LinkStatejbj`），
  物件取自 controller ivars `+0x198`（就是 attachInterface 停在那裡的那個）。

修好後（v240）：

```
$ ifconfig en1 | grep status
	status: inactive        <-- 正確
Ath10kIfaceLink = iface-link=0 ok=1
```

#### 6.45.3 仍然存在的東西

`cwjoin` 目前還是 `-3903`，但 airportd 的 log 已經變成
`setJoinEvent: Joining QCA-TEST (details:(null))` 之後才 aborted，
而且 `wifivelocityd` 的 `GET CURR SCAN RESULT` 一直 `err=2`（= 沒有掃描結果可拿）。
下一輪要查的是 **scan result 在 airportd/scan manager 的快取裡為什麼是空的**
（我們的 driver 有回 15~21 筆，BSD ioctl 路徑也拿得到正確紀錄）。

#### 6.45.4 教訓

- **live KC 與 legacy bundle 的符號要分清**：`an.py list` 的位址在 `0x218xxxxx` 就是 legacy，
  引用它会讓 kext 載入失敗，而且失敗得很安靜（只有 serial 少了 log、介面消失）。
- 每次 deploy 之後**先確認 kext 真的載入**（`ioreg -c Ath10kPCIDevice` 或 serial 第一行），
  再開始看行為，否則會把「沒載入」誤讀成「行為改變」。

### 6.46 第四十一輪：蘋果預期 vs 我們實際（逐項對照表）+ 兩個契約修正

新增 `docs/apple-vs-ours.md`：把 join 路徑上「Apple 的預期（反組譯／log 證據）」與
「本驅動實際行為（breadcrumb 證據）」逐項對照，分成
①userspace→family 的入口契約 ②assoc 本身 ③link 狀態回報 ④掃描結果 ⑤目前唯一剩下的差異。

這輪照著表補掉兩個真差異：

| 版本 | 差異 | 修法 | 證據 |
|---|---|---|---|
| **v242** | `IO80211ScanManager::queryScanResult`（`0x2013f80`）用 `(controller, cmd, type=11, vif, r8=out)` 呼叫 slot 406；我們把第三個參數（`11`）當使用者位址 copyIn → 失敗 → 落到 Apple 實作回 `0x66` | slot 406 增加「第三個是 type（<0x10000）」的分支，type 11 直接寫進 r8 指向的緩衝區；type 20 也走這條（含 copyIn） | `Ath10kScanServeTrace` 開始出現多筆成功；airportd 的 `GET CURR SCAN RESULT` 從每次都 `err=2` 變成部分 `err=0` |
| **v243** | `ASSOCIATE_RESULT(21)` 在未連線時回 `kIOReturnError`（v230 的過度修正）→ framework 轉成 `0x66`，client 報 **102** | ioctl 一律成功，改用 `result` 欄位表達狀態（`UNAVAILABLE(0)` / `SUCCESS(1)`，Apple enum 本來就這樣定義） | `Ath10kIoctlFall="type=21 st=66"` 消失，錯誤碼回到 `-3903` 且 airportd 已走到 `setJoinEvent: Joining QCA-TEST` |

剩下的差異（對照表 §4.3）：scan manager 需要 **`apple80211scan_req_node`** 才能把掃描結果
交給後續查詢（`getCachedScanResults`），我們只服務 type 11 的讀取、沒有維護 request node，
所以 `GET CURR SCAN RESULT` 仍會出現 `err=2`。

### 6.47 第四十二輪：Apple 預期的 request-node 契約，以及「Set(SSID) 直接開 join」的嘗試

#### 6.47.1 逆向到的事實

`IO80211ScanManager::getCachedScanResults`（`0x201150a`）一進來就在**驗證 `apple80211_scan_data`**：

```
[scan_data+0x04] - 1  > 2  → 回 0x16 (22)      ; ap mode 必須 1..3
[scan_data+0x10]      > 0x20 → 回 0x16          ; ssid_len ≤ 32
[scan_data+0x34]      > 9 或 !bt(0x38e, …) → 0x16 ; scan type 必須在白名單
[scan_data+0x44]      > 0x190 → 0x16            ; dwell ≤ 400
```

也就是說 scan manager 對「快取查詢」這條路**先驗請求參數**，不符就直接失敗（airportd 記成 `GET CURR SCAN RESULT err=2`）。
我們目前的 `venturaScan()` 送出的 scan_data 沒有照這個白名單填（`ad_mode` 常是 0、`scan type` 也沒設），
所以 manager 端的快取查詢永遠失敗。

#### 6.47.2 這輪的修改（v244）

在 `Set(SSID)` 的處理裡加了一條「拿到 SSID 就自己開 join」的路徑（`Ath10kJoinOnSSID` breadcrumb）：
airportd 在 join 時一定會送 `Set(SSID)`，即使它的 `Associate2` 在 userspace 就放棄，
驅動也能用自家憑證把連線建起來。

結果：`networksetup -setairportnetwork` 這條路**機場根本沒送 `s1`(Set SSID)**，
`Ath10kClientTrace` 只有 `g2 g1 g1 g1 g1 g2 g1 g2 g1 g1 g1 g14 …`，
所以這條 fallback 沒有機會觸發 —— 失敗點比我們以為的更前面（airportd 在送 Set(SSID) 之前就拒絕）。

#### 6.47.3 現在的狀態

- VM 保持在可用狀態：`connected-wpa2` / `status: active` / 舊 IP 正常。
- 點選連線（`cwjoin`、`networksetup`）仍是 `-3903`，且**沒有 `s1`／`s20`**。
- 對照表（`docs/apple-vs-ours.md`）裡 §4.3 的 request-node 差異已定位到具體參數白名單，是下一輪最明確的著力點。
