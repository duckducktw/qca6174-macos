# 交接：QCA6174 原生 macOS Ventura Wi-Fi（v84 之後）

> 寫於 2026-11-23。上一份 `docs/HANDOFF.md`（600+ 行）仍然是完整歷史，這份是**只給下一個接手的人看現況與判斷**的短版，並且刻意把「目前方向可能是錯的」寫在中間，不要照單全收。

---

## 0. 一句話現況

macOS 13.7.8（QEMU/KVM + VFIO）上，QCA6174 已經能被 SystemConfiguration 認成 Wi-Fi（`en1`、`type=IEEE80211`），airportd 的**內層**列舉也認得它（`_getIfListCopy: exit 0, ifCount[1]`），但**外層** `Apple80211GetIfListCopy` 仍回 `-3903 / ifCount[0]`，所以 `networksetup -getairportpower en1` 還是「Wireless is not available」。掃描／連線／上層 UI 都還沒開始。

---

## 1. 驗收條件（沒變）

1. `Ath10kIfacePhase=inited`、`Ath10kStartPhase=controller-started` → ✅ 已達成
2. `ioreg -r -c IO80211Interface` 有節點、有 BSD name、`networksetup -listallhardwareports` 顯示 Wi-Fi → ✅ 已達成
3. 系統設定 → Wi-Fi 可開關 → ❌ **卡在這裡**
4. 掃描 → WPA2 連線 → 取得 IP → 未開始
5. 真機部署 → 未開始

---

## 2. 目前跑的是什麼

- 分支 `ventura-io80211`，HEAD `46851e6`（已 push 到 `duckducktw/qca6174-macos`）。
- VM：Acer（`100.64.0.152`）上的 OSX-KVM，Ventura 13.7.8，guest SSH `127.0.0.1:2223`。
- 部署版本 **v84**（`vk84`），ESP 上的 kext 已換、開機正常、`en1` 存在、無 panic。
- 本輪新增的關鍵程式碼：
  - `Ath10kVenturaInterface::initIfnetParams()` 尾端把 `params->type` 改成 `0x47`（ifnet 是 802.11，但 `IOInterfaceType` property 仍是 6）。
  - `Ath10kPCIDevice::configureInterface()` 在 `IOEthernetController::configureInterface()` **前後各發一次** IEEE80211 medium。
  - `Ath10kPCIDevice::runVifPublish()` 每次重試都把介面節點的 `IOUserClientClass` 蓋成 `IO80211APIUserClient`。

---

## 3. 已證實的事實（有量測，不是推論）

| 事實 | 證據 |
|---|---|
| airportd 收介面的條件是「`SIOCGIFMEDIA` 的答案含 `IFM_IEEE80211`(0x80)」 | 正常機器 log 會印 `Added 'IFM_IEEE80211' supported interface`；我們以前 `supported[0]=0x20`(IFM_ETHER) 時 `_getIfListCopy` 一律 `count[0]` |
| 發佈 0x80 medium 就能讓內層列舉通過 | v80 起 `_getIfListCopy: exit 0/0x00000000, ifCount[1]` |
| `publishMediumDictionary()` 必須在 `super::start()` 之後 | v77 panic：`createVenturaMediumTables+0x7d → _IOLockLock+0x3`，RDI=0；鎖在 `IONetworkController::start` 的 `+0x143` 才 `IOLockAlloc` 到 `[this+0xc8]` |
| 只在 start 後發不夠，介面 ifmedia 在 attach 期間就建好 | v78：字典是 0x80 但 `ifconfig` 仍是 autoselect；v80 改成 configureInterface 前後各發才生效 |
| 外層 wrapper 會對「內層找到的介面」再做一次過濾 | 同一秒的 log：內層 `exit 0, ifCount[1]`、外層 `exit -3903, ifCount[0]` |
| 目前為止**從來沒有** `IO80211APIUserClient` 實例 | `IOKitDiagnostics` 的 Classes 計數 `"IO80211APIUserClient"=0`；`ioreg` 也查無 |
| 把介面節點 `IOUserClientClass` 改成 `IO80211APIUserClient` 也沒用 | v84：`Ath10kUserClientBefore/After` 都錄到、`ioreg` 節點確實是該值，外層仍 -3903、實例仍 0 |
| live `IO80211APIUserClient::commonStart` 要求 provider 是 `IO80211SkywalkInterface` | 反組譯 `commonStart`：`safeMetaCast(rsi, __ZN23IO80211SkywalkInterface10gMetaClassE)`，失敗就 return |
| live `IO80211Controller::attachInterface(OSObject*, IOService*)` 也只收 Skywalk 介面 | 反組譯：cast 失敗印 `invalid skywalk interface`，成功才寫 `[[this+0x120]+0x198]`（`getInfraInterface()` 的來源） |
| live 的 controller **沒有** v1 `apple80211Request`、也**沒有** `getNetworkInterface` | 符號只存在於 legacy 那份：`apple80211Request` @ `0x2188f8c`、`getNetworkInterface` @ `0x2186b4a`（都在 `0x218…` legacy 範圍） |
| `IO80211InfraInterface` **不是** `IONetworkInterface` | `__ZTV21IO80211InfraInterface`（`0xffffff80020e4700`）裡 0 個 `IONetworkInterface` slot，全是 `IOSkywalkEthernetInterface/NetworkInterface/Interface` |
| `publishVenturaVifBsdUnit()` 會 panic | v83 backtrace：`publishVenturaVifBsdUnit+0x685 → IO80211VirtualInterface::prepareBSDInterface → IO80211SkywalkInterface::prepareBSDInterface → IOSkywalkEthernetInterface::prepareBSDInterface → IOSkywalkNetworkInterface::prepareBSDInterface+0x1a` |

---

## 4. ⚠️ 目前方向可能是錯的（請先讀這段）

我上一則訊息說「下一步＝把 Sequoia 的 Skywalk 介面下放成 Ventura `IO80211InfraInterface` 子類」。**那只是推論**，理由是「airportd 開不起 user client → user client 要 Skywalk → 所以介面要變 Skywalk」。但中間那一步（airportd 是因為開 user client 才失敗）**沒有直接量測過**，只有「實例數 0」的間接證據。

更關鍵的是第 3 節最後兩列帶出的另一條路：

- live family **完全沒有** v1 路徑（沒有 `apple80211Request`、沒有 `getNetworkInterface`、`attachInterface` 只收 Skywalk）。
- 但 **legacy 那份 `IO80211Controller` 就在同一個 KC 裡**（`0x218…`），它才有 v1 入口、才認 v1 `IO80211Interface`、而且 legacy 也有一份 `IO80211APIUserClient`（`commonStart` 期望的是 v1 介面）。
- 我們的介面物件的 metaclass 其實是 `IO80211Interface`（live 與 legacy 各有一份：`gMetaClass` @ `0xffffff80020edd60` / `0xffffff8002251a18`）。**如果我們建立的介面實例屬於 legacy 那份 metaclass，而 controller 屬於 live**，兩邊互相 cast 都會失敗——這可以同時解釋「SC 看得到（只看 property）」與「airportd 開不起來（要看 class）」。
- AirportItlwm v2.2.0（Ventura 上公認可用）用的就是 v1 API：`apple80211Request`、`AirportItlwmInterface : IO80211Interface`。它等於走 legacy 那條線。

**所以真正該先問的問題不是「怎麼搬 Skywalk」，而是「我們這個 kext 現在到底綁到哪一份 `IO80211Controller` / `IO80211Interface`，以及能不能改綁 legacy」。** 綁 legacy 的工作量遠小於搬 Skywalk，而且和已知可用的 AirportItlwm 同構。

### 三個便宜的判別實驗（照成本排序，先做 E1）

- **E1（離線，10 分鐘，不開機）**：確認我們編出來的 kext 對 `IO80211Controller` / `IO80211Interface` 的參考，OpenCore 注入時解析到哪一份。做法：用 `kmutil showloaded -b com.apple.iokit.IO80211Family*` 看兩份的載入順序，再比對 `OSMetaClass::lookup` 的先後；以及看 `xnu` 對同名 class 的處理（先註冊者勝）。若結論是「解析到 live」，就評估能不能讓 kext 依賴 legacy 那份（例如把 `OSBundleLibraries` 指向 legacy 的 bundle id，或在 Info.plist 用 `IOProviderClass` 限定）。
- **E2（1 次開機）**：在 guest 寫一支小程式，直接量失敗點，不要再用 log 推論：
  ```c
  io_service_t svc = IOServiceGetMatchingService(0, IOBSDNameMatching(0, 0, "en1"));
  io_connect_t c = 0;
  kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);   // 印 kr
  ```
  同時對 controller 節點做一次。`kr` 會直接告訴你是「這節點沒有 user client」還是「有 class 但 start 失敗」。
- **E3（1 次開機，僅診斷）**：把 `[[controller+0x120]+0x198]` 硬寫成我們的介面指標，看 `getInfraInterface()` 是否非 null、airportd 行為是否改變。**這會跨過 Skywalk cast**，所以就算有效也不能當正式解，只當「airportd 到底走哪條路」的探針。

> 若 E1/E2 顯示失敗點是 class 身分（legacy vs live），那正確方向是**改綁 legacy**，不是搬 Skywalk。
> 只有在 E2 證明 `IOServiceOpen` 是因為 provider 不是 Skywalk 而失敗、且 legacy 路線不可行時，才值得投入 Skywalk 介面的工程。

---

## 5. 環境與指令（Acer `~/qca-tools/`，本機 `~/qca-tools/` 同名副本）

> `Acer` 的 `/tmp` 是 tmpfs，**會整個被清掉**（本輪就被清過一次，舊腳本與 bundle 全消失）。工具都放在 `~/qca-tools`，不要放 `/tmp`。

| 腳本 | 用途 |
|---|---|
| `dm_map.sh` | 重建 `/dev/mapper/ventura-ro`（gpt-head.img ＋ `/dev/nvme0n1p2` ＋ gpt-tail.img）。**主機重開後必須先跑這個**，否則 QEMU 起不來說 `Could not open '/dev/mapper/ventura-ro'` |
| `vfio_bind.sh` | 把 `0000:02:00.0` 從 `ath10k_pci` 改綁 `vfio-pci`，並把 `/dev/vfio/*` 設成群組 `kvm` |
| `build_v.sh` | 本機打包原始碼 → 丟到 acer → 丟進 guest → `make VENTURA=1 -j2 kext`（編譯在 guest 裡） |
| `pull_v.sh NN` | 把 guest 編好的 `Ath10kMac.kext` 拉回 `/tmp/vkNN` |
| `esp_fix.sh [bundle]` | 掛上 OC 的 ESP（`oc-ventura.qcow2`）→ 可選換 bundle → 印 config → 修正 config → 卸載 → 開機 |
| `boot_nokext.sh` | **kext 會 panic 時用**：先把 Ath10kMac 停用再開機，這樣才有 SSH 可以編譯下一版 |
| `boot_vm.sh` / `press_enter.sh` | 開機並送 `sendkey ret`（見下） |
| `oc_enable.py` | 確保 `Kernel>Add` 的 Ath10kMac 啟用、`22.0.0–23.99.99`、`ShowPicker=True`、`Timeout=3` |
| `oc_toggle.py <plist> enable\|disable` | 單獨切換 kext 啟用狀態 |
| `shot.sh` | 從 QEMU monitor 抓 screendump 轉 PNG（卡住時先看畫面） |
| `media-probe.c` | 印 `SIOCGIFMEDIA` 的 `ifm_active/current/count` 與 supported[]，判斷 media 關卡 |

標準循環：

```bash
bash ~/qca-tools/build_v.sh
ssh 100.64.0.152 'bash ~/qca-tools/pull_v.sh 85'
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/esp_fix.sh /tmp/vk85'
ssh 100.64.0.152 'bash ~/qca-tools/press_enter.sh 1'
```

### 五個會浪費你半小時的坑

1. **OpenCore 會卡在選單**：`ShowPicker=False` 在 NVRAM 沒有預設開機項目時**無效**，畫面停住、serial 停在 `OCC: Install console control`、TCP 2223 進得去但沒有 SSH banner（看起來像 kernel wedge）。解法：`press_enter.sh` 送 `sendkey ret`（箭頭鍵可驗證輸入有通）。
2. **`/dev/dm-0` 權限**：重建 dm 之後要 `chgrp kvm /dev/dm-0 && chmod 660`，不然 QEMU 說 `Permission denied`。
3. **kext panic 後不要直接 build**：guest 起不來就沒有編譯環境，先 `boot_nokext.sh`。
4. **不要為了「多等幾次」把 `runVifPublish` 的重試次數拉長**：`publishVenturaVifBsdUnit()` 會在多試幾次之後進到 Skywalk 的 `prepareBSDInterface` 然後 panic（v83 就是這樣炸的）。
5. **`/tmp` 不可靠**：bundle、log、腳本都別只放 `/tmp`；serial log 在 `/tmp/ventura-vm.serial`（開機後才會出現）。

---

## 6. 死路清單（別重走，附證據）

| 做過的事 | 結果 |
|---|---|
| 在 controller vtable 找 infra `apple80211_ioctl(IO80211Interface*, …)` 並實作 | **這個虛擬在 live 根本不存在**（slot 405/406 只有 Virtual/Skywalk 兩個）；5 參數版本只存在於 legacy `0x2186bce` |
| 只把 `ifi_type` 改成 71 | `tools/ifnet-type` 證明 ifnet 真的變 `0x47`，SC 也還是 `IEEE80211`，但 airportd 仍 `count[0]` → **不是這關** |
| 只把 `IOInterfaceType` property 改成 71 | SC 直接丟掉介面（`SCNetworkInterface.c` 走 `default:`）→ 只能留 6 |
| 手動蓋 `[controller+0x200]` 並指望 `getNetworkInterface()` | live **沒有** `getNetworkInterface`；而且 `Ath10kCtrlInfraIvar=No`，Apple 的 attach 不是存在這個 offset |
| 在 `start()` 之前發 medium | panic（`_IOLockLock`，鎖還沒配） |
| 只在 `start()` 之後發 medium | 介面 ifmedia 已經建好，`supported[0]` 仍是 0x20 |
| 把介面節點的 `IOUserClientClass` 改成 `IO80211APIUserClient` | 屬性改了、實例還是 0、外層還是 -3903 |
| 直接 `new IO80211VirtualInterface`（舊帳） | kext 靜默不被注入（HANDOFF §5.10） |

---

## 7. 座標速查（13.7.8 KC，`/tmp/kc2`，用 `/tmp/an.py`）

- live `__ZTV17IO80211Controller`：`0xffffff80020d86f8`；legacy：`0xffffff8002240b60`
  - slot 396 createWorkQueue(0xc60)、397 getWorkQueue(0xc68)、402 getPrimarySkywalkInterface(0xc80)
  - 405 `apple80211_ioctl(Virtual,ifnet,m,void)`(0xca8)、406 Skywalk ioctl(0xcb0)、407/408 Virtual/SkywalkRequest(0xcb8/0xcc0)
  - 420 getLogPipes(0xd20)、427 getFaultReporterFromDriver(0xd58)、437 createVirtualInterface(0xda8)、438 attachVirtualInterface(0xdb0)
  - vptr = `&vtable[2]`，所以 kernel `[vptr+0xNNN]` ⇒ 索引 `(0xNNN+0x10)/8`
- `IO80211Controller::attachInterface(OSObject*, IOService*)`：`0xffffff8001fed7b8`（收 Skywalk、寫 `[[this+0x120]+0x198]`）
  - 註：live bundle 的位址範圍比想像中寬（至少到 `0x20edd60`），所以「`0x20e…` 是 live、`0x218…`/`0x224…`/`0x225…` 是另一份」才是對的切法；不要用單一常數硬判。
- `IO80211Controller::getInfraInterface`：`0xffffff8001fef084`
- `IO80211Interface::gMetaClass`：live `0xffffff80020edd60`、legacy `0xffffff8002251a18`
- `IO80211InfraInterface::gMetaClass` `0xffffff80020ee2a0`、ctor `C1` `0xffffff800203f06c`、vtable `0xffffff80020e4700`
- `IO80211SkywalkInterface::init()` `0xffffff800200033a`、`init(IOService*)` `0xffffff800200038e`、`setInterfaceRole` `0xffffff8002000aaa`（就是寫 `[ivars+0x50]`）、`setSelfMacAddr` `0xffffff80020025d8`
- `IONetworkController::publishMediumDictionary` `0xffffff8002c5f924`（鎖 `[this+0xc8]`）、`IONetworkController::start` `0xffffff8002c5dcc8`（`+0x143` 配置該鎖）
- `IO80211APIUserClient::commonStart` `0xffffff800202445a`（要求 Skywalk provider）

---

## 8. 這條線相關的檔案

- `src/kext/Ath10kPCIDevice.cpp`：`start()`（fault reporter／work queue／packet filters／Apple start／medium）；`createInterface()` 在 `src/kext/ventura/Ath10kVentura.cpp`
- `src/kext/Ath10kNetwork.cpp`：`configureInterface()`（medium 前後各發一次，**改這裡要小心順序**）
- `src/kext/ventura/Ath10kVentura.cpp`：`createVenturaMediumTables()`、`venturaMediumDictionaryKeys()`（診斷用）、`createInterface()`、`runVifPublish()`、`publishVenturaVifBsdUnit()`
- `src/kext/ventura/Ath10kVenturaInterface.cpp`：`init()`（SC properties、`IOUserClientClass`）、`initIfnetParams()`（`params->type=0x71`）、`attachToDataLinkLayer()`、`inputPacket()`
- `src/kext/ventura/Ath10kVenturaDecl.inc`：上面那些方法的宣告都在這裡（class 內）
- `src/kext/native/Ath10kNativeInterface.hpp` / `Ath10kNative.cpp`：**Sequoia 分支**的 Skywalk 介面參考實作（`IO80211InfraProtocol` + `IO80211SkywalkInterface::init(owner, mac)`），若真要走 Skywalk 就從這裡改寫

---

## 9. 給下一個人的第一句話

**先做 E1/E2，不要先寫 Skywalk 類。** 這一輪所有「看起來該做 X」的判斷，事後都證明要先量一次（infra ioctl 是假的、`+0x200` 是 legacy 的、`IOUserClientClass` 不夠、medium 的位置比想像中關鍵）。現在的推論鏈還缺「airportd 到底在哪一行失敗」這一環，補上它再決定方向，比搬一整個介面類別便宜太多。
