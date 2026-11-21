# HAT10K Ventura 原生 Wi-Fi 交接文檔（2026-11-21 更新）

## 這是什麼

目標：讓 QCA6174（`168c:003e`，子系統 `11ad:0807`）在 macOS 13.x Ventura 上以 **真正原生** 形式運作——出現在「系統設定 → Wi-Fi」、可開關、可掃描、可 WPA2 連線，且**不用相容層、不用獨立 app、不要求使用者開終端機**。驗證由 agent 自己在 VM 內完成（使用者留在 Ubuntu，只接實體網路線）。

**誠實狀態（2026-11-21 深夜）**：介面已能在 VM 內**建立、註冊並發布到 IORegistry**（驗收條件 A 通過）。**BSD 介面單元仍未取得**：`ifconfig -l` 只有 `lo0 gif0 stf0 en0`，`networksetup -listallhardwareports` 沒有 Wi-Fi 埠，所以系統設定看不到 Wi-Fi、B～E 全部未達成。硬體驗證（實體 ESP stage0）尚未重新部署。

分支：`ventura-io80211`，最新 commit `3bb0e72`（遠端同步）。VM 目前跑的是 **v20 bundle**（`/tmp/vk20`，已發布介面、穩定可登入）。

## 目前唯一權威狀態（v20）

VM：macOS **13.7.8 / 22H730**，`com.apple.iokit.IO80211Family 1200.13.0`（載入基址 `0xffffff8001f35000`，長度 `0x19bff9`），`IONetworkingFamily 3.4`。kext `br.com.jonasw8.driver.ath10kmac 0.0.105`，OpenCore 1.0.5 DEBUG（`Target=67`、`ShowPicker=false`、`Timeout=0`）。

| 斷點屬性 | 值 | 意義 |
|---|---|---|
| `Ath10kProbeMatched` | Yes | PCI personality 匹配成功 |
| `Ath10kVenturaLogPhase` | `ready` | 3 pipe + LogStream + CCFaultReporter 都建立成功 |
| `Ath10kIfacePhase` | `inited` | **介面 init 成功**（卡了多輪的 `super-init-failed` 已解） |
| `Ath10kStartPhase` | `interface-published` | 介面已 registerService + attachToDataLinkLayer |
| `Ath10kAlignPhase` | `aligned` | vtable slot 補丁確實寫進物件 vptr 指向的那張表 |
| `Ath10kReporterReady` | Yes | `_venturaCoreFault` 非 null |
| `Ath10kStackPhase` | `stack-insert-failed` | 見下方「死了的路」第 5 條（其實是誤判） |
| `Ath10kVifPhase` | `vif-create-failed` | `createVirtualInterface(addr,7)` 回 null |
| `ioreg -r -c IO80211Interface` | 23 行 | 節點 `Ath10kVenturaInterface`，registered/matched/active，**無 BSD Name、`IOInterfaceState=0`** |
| `ifconfig -l` | `lo0 gif0 stf0 en0` | 沒有 en1 |
| `kmutil showloaded` | 有 ath10kmac | 無 kernel panic |

## 不可破壞的規則（每條附一句理由）

1. **Ventura 建置不得引用 `IO80211FaultReporter`、`IO80211WorkLoop::workLoop()`** —— 這兩個在 13.7 的注入環境解析不到（或會讓 OpenCore 直接**靜默跳過整個 kext**：binary 有編出來、`kmutil showloaded` 一條都沒有）。`IO80211WorkLoop` 已實測會踩到（見「死了的路」第 2 條）。
2. **`Ath10kPCIDevice` 必須覆寫 `enable/disable(IO80211SkywalkInterface*)` 與 16 個 `_RESERVEDIO80211ControllerN()`** —— 缺任一個，OpenCore 的 vtable 重建失敗 → `Prelinked injection … Invalid Parameter`。
3. **先 `createWorkLoop()` 再跑 corecapture** —— `CCPipe` 建立需要 work loop（曾以 `Ath10kVenturaLogPhase = "no-workloop"` 呈現）。
4. **Ventura 分支不要呼叫 `IO80211Controller::start()`** —— 實測必 panic，見「死了的路」第 1 條。
5. **除錯一律用 IORegistry 斷點，不要靠 IOLog** —— 此 VM 的 kext `IOLog`/`kprintf` 不出現在 `log show`、`dmesg`（dmesg 1303 行、零 ath10k；serial 也沒有）。斷點寫在 `provider`（PCI nub）上才看得到；寫在 driver 物件 `this` 上，start 失敗時節點不存在 → 看不到。
6. **部署前必須 `oc-enable.py`** —— `deploy_v*.sh` 樣板只跑 `oc-auto.py`（只設選單/日誌），不會啟用 kext。漏了就是「完全沒有斷點」的假象。
7. **vtable 位移一律用「vptr 相對」計算** —— 物件 vptr 指向 `&vtable[2]`，所以 kernel 端 `[vptr+0xNNN]` 對應的是 vtable 的 `0xNNN+0x10`。搞錯會寫到隔壁的 virtual（我因此在 `+0xd20`/`+0xd58` 白忙兩輪，正確值是 `+0xd10`/`+0xd48`）。

## 這輪真正解掉的根因（勿重查）

`IO80211Interface::init()` 回傳 false，根因在它尾段呼叫的 `IOEthernetInterface::init(IONetworkController*)`，以及我們繞過 Apple `start()` 後沒人做的事：

1. **controller 必須有 `IOPacketFilters` 這個 OSDictionary 屬性**，內容至少 `IONetworkFilterGroup`（值 819）、`IOEthernetWakeOnLANFilterGroup`、`IOEthernetDisabledWakeOnLANFilterGroup`（最後一個必須是 OSNumber）。不存在或型別錯 → 直接 `return false`。現在由 `start()` 自己 `setProperty` 補上。
2. **`configureInterface()` 需要 controller 有 output queue**（`getOutputQueue()` 非 null）→ `start()` 內補 `createOutputQueue()`。
3. **`attachInterface(&iface,true)` 會自己再呼叫一次 `createInterface()`**、並把結果寫回指標；實務上直接手動走 `configureInterface()` → `iface->attach(this)` → `registerService()` → `attachToDataLinkLayer()` → 把介面寫進 `IO80211Controller` 的 ivar `0x200` 反而更可控（且能逐段加斷點）。
4. **呼叫 Apple 的實作要用 mangled 名稱直接呼叫**：`extern "C" bool f(...) __asm("__ZN19IOEthernetInterface4initEP19IONetworkController");`。寫 `IOEthernetInterface::init(controller)` 會被編成 **virtual dispatch**（`call *0x868(%rax)`），又繞回 `IO80211Interface::init`。檢查方法：`nm -u` 你的 `.o` 要看到那串 mangled 名稱的直接引用。
5. **`attachToDataLinkLayer()` 回 true 不代表有 BSD 單元** —— 這是最容易誤判的一點，實測回 true 但 `ifconfig -l` 完全沒變。

## 死了的路（全部實測過，不要重試）

1. **`super::start(provider)`（= `IO80211Controller::start`）→ 必定 panic**
   `"No ivars->_faultReporter" @IO80211Controller.cpp:3604`，堆疊固定是
   `IO80211Controller::logDebug ← IO80211Controller::start+0x1bd ← Ath10kPCIDevice::start`。
   即使 `Ath10kReporterReady=Yes`、`Ath10kAlignPhase=aligned` 也一樣；panic 發生在 start 之後的第一次 logDebug（`createIOReporters` 失敗訊息之後）。`getLogPipes` 改成轉呼叫 base、或直接拿掉 override，都無法避免。
2. **`createWorkLoop()` 改回 `IO80211WorkLoop::workLoop()`（AirportItlwm 的做法）→ 整支 kext 不被注入**
   binary 正常 link（符號在 KC 裡是 `N_SECT|N_EXT`、`nm` 也看得到），但 `kmutil showloaded` 完全沒有它，`ioreg` 也沒有 `Ath10kPCIDevice`。症狀與「kext 被 OpenCore 靜默跳過」一樣，沒有任何日誌。
3. **直接改 vtable slot 硬塞 fault reporter/getLogPipes** → 兩輪失敗：
   第一次偏移算錯（`+0xd20`/`+0xd58`，正確是 vptr 相對的 `+0xd10`/`+0xd48`）；
   修正偏移後仍然 panic（同第 1 條），所以那條 chain 不是唯一缺口。`alignVenturaVtable()` 現行版本已用正確偏移、可用、但不足以讓 Apple start 過關。
4. **`attachVirtualInterface(&vif, mac, 7, true)` / `createVirtualInterface(mac, 7)` → 都失敗**
   `type` 必須是 7；MAC 也給了（`_wmiInit.ready` 為 false 時 `getHardwareAddress()` 回 NotReady，已改讀 `_wmiInit.mac`、再退化合成本機位址），仍然回 null。把 `apple80211VirtualRequest`/`apple80211SkywalkRequest` 兩個 slot 接到我們的 `apple80211Request` 也沒救；失敗點在 `IO80211VirtualInterface::init` 內部（第一關是 `IO80211SkywalkInterface::init(controller)`，之後還要 controller 回答 `apple80211VirtualRequestIoctl(0xc03069c9, 0xc, vif, …)`，0xc = `APPLE80211_IOC_CARD_CAPABILITIES`，我們有實作但顯然不是卡點）。
5. **`IONetworkStack` 的 `vptr+0x340`** → 那格其實是 `IORegistryEntry::isParent`，不是插入函式（`stack-insert-failed` 只是「不是我的子節點」）。
6. **用固定位移硬呼叫 `IONetworkStack::insertNetworkInterface`（vptr−0xac94）** → **kernel 卡死**（TCP 2223 埠開著、sshd banner 不回應、serial 沒有 panic）。`insertNetworkInterface` 與 `gIONetworkStack` 都是 **local 符號**（`ext=False`），正常方式連結不到，硬跳也不可行。

## 社群解法（AirportItlwm / OpenIntelWireless，已讀原始碼）

`itlwm` 的 `AirportItlwm` 就是「Ventura 原生 Wi-Fi 可用」的實證（v2.2.0 有 Ventura 版），做法是：

1. 介面類別跟我們一樣：`AirportItlwmInterface : IO80211Interface`，`init()` 只有 `super::init(controller)`。
2. `AirportItlwm::start()` **直接 `super::start(provider)` 會成功**，之後只呼叫一次 `attachInterface((IONetworkInterface **)&fNetIf, true)`。
3. `createWorkLoop()` 回傳 `IO80211WorkLoop::workLoop()`（這一條我們照抄會讓 kext 不被注入，見「死了的路」第 2 條）。
4. **BSD 介面是它自己接的**：`configureInterface(netif)` 內 `super::configureInterface(netif)` 之後呼叫 `ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif))`，`ifp` 是它自帶 net80211 的 `struct ifnet`。→ 這解釋了我們為何 `attachToDataLinkLayer` 回 true 卻什麼都沒有：BSD 單元不是 IO80211 的 vif 長出來的。
5. v2.3.0 之後另外加了 `IO80211InfraInterface` 與新的 virtual interface 邏輯（給更新的 macOS）。

## 下一步（給下一個接手的人）

前提認知：**這台 13.7 的 IO80211 有兩份 `IO80211Controller` 實作**（KC 內 `__ZTV17IO80211Controller` 兩個：`0xffffff80020d86f8` 在 IO80211Family 範圍內、屬 Skywalk 世代，`getLogPipes` 在 vtable `0xd20`、`getFaultReporterFromDriver` 在 `0xd58`；`0xffffff8002240b60` 是另一份、順序不同、沒有 `getFaultReporterFromDriver`）。`/tmp/vt_both.txt` 有兩份的完整 slot 對照表。panic 訊息裡的字串是 `IO80211_kexts-1132.58`，跟 `kmutil` 顯示的 1200.13 不一致，不要被它誤導。

照成本效益排序：

1. **照 AirportItlwm 的 net80211 路線自己接 ifnet**（最有可能成功、不必碰 Apple 的 vif）：
   - kernel 沒有 export `ether_ifattach`（已確認），所以要走 `ifnet_allocate` + `if_attach`（兩者都有 export：`_ifnet_allocate` / `_ifnet_attach`），自己填 `ifnet_init_params`（output/ioctl/demux 等 callback），再讓 `IONetworkInterface` 的資料路徑接到它。
   - 缺點：要實作一整套 ifnet callback（含 output 走 `outputPacket`、ioctl 轉 apple80211），工作量最大但路徑最乾淨。
2. **把介面層改成繼承 `IO80211InfraInterface`**（Sequoia 分支就是這樣做的：`src/kext/native/Ath10kNative.cpp:241` 有 `IO80211InfraInterface::init()` 的用法）。`IO80211InfraInterface::init()` 本文只有 `IO80211SkywalkInterface::init()` + 一個 ivar 配置，成本低，但要重做介面層與 vtable，且會動到已通過的 A。
3. **重編譯對齊 `IO80211ControllerV2.h`**：vendored v1 header 的 virtual 順序跟實際載入的世代不一致（`IO80211ControllerV2.h` 的 `createWorkQueue`/`getWorkQueue`/`getFaultReporterFromDriver`/`bool getLogPipes` 才對得上 0x20d86f8 那份）。整支 controller glue 要改簽章（`getHardwareAddressForInterface(IOEthernetAddress*)`、`monitorModeSetEnabled(bool,UInt)`、`hardwareOutputQueueDepth()`、`dataLinkLayerAttachComplete()`、`apple80211VirtualRequest`/`apple80211SkywalkRequest`…），是「根治」但改動大。
4. **不再碰 IO80211 vif，改走 itlwm（非 Airport）模式** + 自寫使用者空間 app —— 這違反本次驗收條件（不得要求獨立 app），只列為最後手段。

驗收條件（依序，未變）：

- A：`Ath10kIfacePhase = "inited"` 且 `Ath10kStartPhase = "interface-published"`。**已達成**
- B：`ioreg -r -c IO80211Interface` ≥ 1 筆（已達成）且 `networksetup -listallhardwareports` 出現 Wi-Fi 介面。**未達成**
- C：「系統設定 → Wi-Fi」可見且開關可操作（截圖為證）。**未達成**
- D：可掃到 AP 並以 WPA2 連線、取得 IP（`ifconfig` + ping 為證）。**未達成**
- E：以上全程不修改系統設定以外的東西、不需終端機、無獨立 app；最後才把 kext 部署回實體 ESP 並在真機重驗。**未開始**

## 檔案地圖

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | `probe()`/`start()`：Ventura 分支建 work loop、備 corecapture、publish `IOPacketFilters`、建 output queue、手動 create/configure/attach/register/attachToDataLinkLayer、寫 `[this+0x200]`、stack 插入嘗試、vif 嘗試；所有 `Ath10k*Phase` 斷點 |
| `src/kext/ventura/Ath10kVentura.cpp` | `prepareVenturaLogs()`、`getLogPipes()`、`alignVenturaVtable()` + 三個 slot thunk（`venturaLogPipesSlot`/`venturaFaultReporterSlot`/`venturaVirtualRequestSlot`/`venturaSkywalkRequestSlot`）、`createInterface()`、`apple80211Request` 分派、`venturaPower/Scan/Associate…` |
| `src/kext/ventura/Ath10kVenturaInterface.cpp/.hpp` | `Ath10kVenturaInterface : IO80211Interface`；`init()` 直接用 mangled `ath10kNetworkInterfaceInit` + `ath10kEthernetInterfaceInit`（跳過 `IO80211Interface::init`）；`inputPacket()` 把 EAPOL 交 IO80211 supplicant |
| `src/kext/ventura/Ath10kVenturaDecl.inc` | Ventura 專屬宣告：Skywalk enable/disable、16 reserved slot、`getLogPipes` override、vtable slot thunk、corecapture 成員 |
| `src/kext/ventura/Ath10kVenturaABI.hpp` | `__IO80211_TARGET __MAC_13_0` + `IO80211Controller.h`/`IO80211Interface.h`/`CCPipe.h`/…/`FaultReporter.h` |
| `src/kext/Ath10kNetwork.cpp` | `configureInterface()`（目前只 `IOEthernetController::configureInterface` + 屬性）、`createWorkLoop()`、`createOutputQueue()`、`getHardwareAddress()`（`_wmiInit.ready` 才給） |
| `src/kext/native/*` | Sequoia（v2）對照實作：`getFaultReporterFromDriver()`、`prepareNativeLogs()`、`IO80211InfraInterface::init()` 用法 |
| `docs/hardware-verification.md` | 實機 stage0/stage3 陷阱與 hash 比對 |

## 重跑 / 救援（Acer `100.64.0.152`）

```bash
# 1) 同步原始碼並在 guest 內編譯
cd ~/Data/Dev/qca6174-native && tar czf /tmp/qca-src.tgz --exclude=build --exclude=.git --exclude=artifacts-ci* .
scp -q /tmp/qca-src.tgz 100.64.0.152:/tmp/
ssh 100.64.0.152 'scp -q -P 2223 /tmp/qca-src.tgz user@127.0.0.1:/tmp/ && ssh -p 2223 user@127.0.0.1 \
  "tar xzf /tmp/qca-src.tgz -C ~/qca && cd ~/qca && make VENTURA=1 -j2 kext"'

# 2) 部署 + 開機（編號自增，內含 oc-enable.py + oc-auto.py）
sed 's/vk14/vk35/g; s/v14.tgz/v35.tgz/g' /tmp/deploy_v14.sh > /tmp/deploy_v35.sh
scp -q /tmp/deploy_v35.sh 100.64.0.152:/tmp/
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/deploy_v35.sh'

# 3) 讀斷點
ssh 100.64.0.152 'ssh -p 2223 user@127.0.0.1 \
  "ioreg -l -w0 | grep -E \"Ath10kIfacePhase|Ath10kStartPhase|Ath10kVifPhase|Ath10kStackPhase|Ath10kAlignPhase\"; ifconfig -l"'
```

**救援（VM panic 循環或卡死時，不需要 guest）**：

```bash
cat ~/sudo.pwd | ssh 100.64.0.152 'bash /tmp/install_bundle.sh /tmp/vk20'   # 裝回已知可用的 v20 並開機
```

`install_bundle.sh <dir>` 是這輪新寫的：直接從 Acer 上的 `/tmp/vkNN` 取 bundle 寫進 ESP 再開機，完全不依賴 guest SSH。各版 bundle 都還留在 Acer 的 `/tmp/vk18…/tmp/vk34`（v20 = 已發布介面的穩定版，v21/v22/v23/v24/v29/v31/v32/v33/v34 = 各種失敗實驗）。

`/tmp/disable_boot.sh` 仍可用（`oc-disable.py` + `oc-auto.py` + 開機）取得可登入但不載入 kext 的 guest。

## 分析工具（這輪建立，可直接重用）

- `/tmp/kc2`：本機（Ubuntu）上的 `BootKernelExtensions.kc` 副本（69MB）。**不需要**再從 guest 抓。
- `/tmp/an.py` + venv `/tmp/cs`（`capstone 5.0.7`）：解析 fileset、全域 symtab、vtable、反組譯。用法：
  - `/tmp/cs/bin/python /tmp/an.py list <子字串>`：找符號
  - `... vtable <vtable位址> <entry數>`：解 vtable（已內建 chained fixup 解碼：`0xffffff8000000000 + (p & 0xFFFFFFFFF) + 0x100000`）
  - `... dis <符號名或位址> <bytes>`：反組譯並自動標註 call/jmp 目標與 rip 相對字串
  - `... str <位址> …`：讀 C 字串
  - `... ptr <位址>`：讀指標並解析到符號
- `/tmp/vt_both.txt`：兩份 `IO80211Controller` vtable 的完整對照（vtable offset / obj offset / 符號）。
- **解析 KC 的兩個地雷**：抽單一 kext 的 Mach-O 後不能解析它的 `LC_SYMTAB`（offset 是整個 KC 的絕對位置），要對完整 `.kc` 解析；`nm` 對 kext 內的 local 符號會標 `ext=False`，這種符號**不能**直接引用（會讓 OpenCore 跳過 kext）。

## 其他已知陷阱（沿用）

- **不要**在 VM panic 循環時 `scp` 進 guest（會 `Connection closed`）→ 用 `install_bundle.sh`。
- **不要**用 `pgrep -q`（此 Ubuntu 不支援）、**不要** `pkill -f qemu-nbd`（被安全層擋）→ 用 `systemctl --user stop qca-vm`。
- **不要**用 `ioreg | grep 0x168c` 判斷網卡（byte-swapped）→ 用 `system_profiler SPPCIDataType`。
- **qemu-nbd**：VM 開著會 `Failed to get write lock`；掛載後等 `/dev/nbd0p1` 最多 15 秒。
- **git push**：`~/.gitconfig` 曾有 `http.https://github.com.proxy=socks5h://127.0.0.1:40001`，會把 TLS handshake 卡死（`GnuTLS recv error (-110)`）。已 `--unset`，直連正常。若又出現推送失敗，先檢查這個 proxy 設定。
- **otel/capstone**：以「符號名子字串」比對會撞到 `__ZZN15Ath10kPCIDevice5startEP9IOServiceE6states` 這類 local static；`md.skipdata=True` 才不會提早停。
- **`gem install`/pip 不要動系統 python**：capstone 裝在 `/tmp/cs` venv。

## 建議技能

- `osx-kvm-headless-install`：VM 生命週期、qemu-nbd、SSH 進 guest。
- `macos-kext-hardware-bringup`：kext 建置/注入、OpenCore 設定、硬體驗證，以及本輪新增的 IO80211 v1/v2 ABI 陷阱。
- `handoff`：下次再交接時沿用本文件結構。
