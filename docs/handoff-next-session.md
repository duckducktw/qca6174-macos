# 交接：QCA6174 Ventura 原生 Wi-Fi（2026-11-27 收工 → 下一個 session 從這裡開始）

> 這份是**跨 session 交接正本**。另一份必讀：`docs/HANDOFF.md` §6.50–§6.56
> （今天的完整量測與反組譯），以及 `docs/handoff-apple-join-conditions.md`（四層條件）。

---

## 0. 一句話現況

驅動**能上網**（開機自連 `connected-wpa2`），但**「點選連線」不通**：
`Apple80211Associate2` 在 userspace 就回 `-3903`。
今天把原因收斂到**一個結構性錯誤**（見 §2），還沒開始改。

---

## 1. 環境（照這個跑，不要重新摸索）

| 項目 | 值 |
|---|---|
| repo | `~/Data/Dev/qca6174-native`，分支 `ventura-io80211`，遠端 `duckducktw/qca6174-macos` |
| HEAD | `35d8c7c`（已 push） |
| VM 主機 | Acer `100.64.0.152`（Tailscale） |
| guest 存取 | `ssh 100.64.0.152 "ssh -p 2223 user@127.0.0.1 'bash -s'" < script.sh` |
| 目前部署版本 | **v254**（`/tmp/vk254` 留著可回滾） |
| 目前 boot-args | `… ath10kjoin=<ssid>:<psk> ath10krole=7`（role 旋鈕本輪新加，預設 7 = 原行為） |

### 建置／部署（**Acer 與 guest 的 `/tmp` 每次重開都會清空**）

```bash
bash ~/.hermes/cache/scratch/build_v.sh          # 本機 tar → Acer → guest 內 make VENTURA=1 -j2 kext
ssh 100.64.0.152 'bash ~/qca-tools/pull_v.sh 255'
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/deploy.sh /tmp/vk255'
```

- 工具正本在 **`~/qca-tools/`**（`pull_v.sh`、`deploy.sh`、`with_esp.sh`、`boot_vm.sh`、
  `dm_map.sh`、`vfio_bind.sh`、`press_enter.sh`）；`build_v.sh` 只在本機 scratch。
- 主機重開後要先 `dm_map.sh`（DM OK）→ `vfio_bind.sh`（`/dev/vfio/8`）→ `boot_vm.sh`。
- 開機後 60–90 秒回到 `connected-wpa2`；**每次部署先確認 kext 真的載入**再解讀行為。
- guest 內要用的工具（`cwjoin`、`a2*`、`iocs`、`refflags*`）來源在 `~/qca/tools/`，
  重開後要重新 `clang`。

---

## 2. 今天最重要的結論（先看這段，不要從舊結論重跑）

### 2.1 `-3903` 的最終定位

`Apple80211Associate2` 的第一個擋門是 **`0x93e7`**：

```
93d6  test r14, r14            ; 第 4 個參數（輸出字典）非空
93d9  je   0x93e7              ; 空 → -3903
93db  test byte [r12+0x51],0x40
93e1  jne  0x977d              ; bit6 有設 → 放行
93e7  store -3903
```

- `r12`/`rbx` 的 `+0x51` bit 6 在實機上**永遠是 0** → 所以任何帶 RSN 的 join 都 0.0 秒被踢。
- 把該常數就地歸零 → 同一份字典直接 `rc=0`，證明後面全通。
- 強制通過 `0x93e1` 後，下一個 `-3900` 落在 `0xa586`，它前面 `0xa567` 又是同一個
  `[ref+0x51] & 0x40`。**RSN 與 WPA 兩條路被同一顆 bit 擋住。**

### 2.2 但這顆 bit 的根不在 userspace

逐 IOC 探測（`tools/iocs.c`，fork 隔離）：

```
rc=0     : BSSID(9)、DRIVER_VERSION(43)、HW_VERSION(44)、VIF_PARENT(97)
rc=-3900 : STATE(13)、OP_MODE(15)、ASSOC_RESULT(21)、ASSOC_STATUS(50)、VIF_ROLE(96)
SIGSEGV  : SSID(1)、AUTH_TYPE(2)、CHANNEL(4)、CARD_CAPS(12)、PHY_MODE(14)、
           POWER(19)、SUP_CHANNELS(27)、CURRENT_NETWORK(103)
```

`STATE`/`OP_MODE` 在驅動裡**早就實作且回 success**（`Ath10kVentura.cpp` 的
`APPLE80211_IOC_STATE` / `APPLE80211_IOC_OP_MODE` case），所以 `-3900` 是**框架在到驅動之前**回的。

registry 對照：

```
Ath10kInfraAttach      = ok          ← 我們以為成功
Ath10kInfraPhase       = attached
Ath10kVifPhase         = vif-registered
Ath10kVifStart         = ok
Ath10kVifPublishRetry  = failed      ← ★ 自製 BSD publish 失敗
Ath10kVifBsdPhase      = skipped-guarded
Ath10kVifBsdName       = (空)
Ath10kCtrlInfraIvar    = No          ← controller ivar +0x200 是空的
class 實例：IO80211InfraInterface = 0、IO80211Interface = 0、
            IO80211VirtualInterface = 1、Ath10kVenturaInterface = 1
```

`Apple80211GetVirtualIfListCopy` 從本 session 一開始就是 `ifCount[0]`。

**同理：role 不是那把鎖** —— 加了 `ath10krole=1|6|7` 旋鈕實測，role=1 與 role=7 的
STATE/OP_MODE 都一樣 `-3900`（v254，HANDOFF §6.55.12）。

### 2.3 權威對照（AirportItlwm v2.2.0 原始碼，Ventura 可用）→ 我們走錯路

```cpp
// 權威：只有一個介面物件，BSD unit 由 IONetworkStack 配
IONetworkInterface *AirportItlwm::createInterface() {
    AirportItlwmInterface *netif = new AirportItlwmInterface;  // IOEthernetInterface 子類
    if (!netif->init(this, fHalService)) { netif->release(); return NULL; }
    return netif;
}
// start() 內：
if (!attachInterface((IONetworkInterface **)&fNetIf, true)) { … return false; }
…
registerService();          // controller
fNetIf->registerService();  // 介面 → IONetworkStack 配 BSD unit
```

我們多做了：`createVirtualInterface()` 另生一顆 vif、`ath10kAttachSkywalkInterface()`、
自製 `publishVenturaVifBsdUnit()`（回 false，再往前會 panic 在 Skywalk 的
`prepareBSDInterface`）、延遲 retry 手塞 controller ivar `+0x200`、手改 vif role。
**結果就是 §2.2 那一串症狀。**

---

## 3. 下一個 session 的第一件事（結構性改動，先備份）

目標：讓介面走權威路徑，使 `BSD Name` / `IO80211InfraInterface` 由框架自己產生。

1. 備份現況：`git tag pre-interface-publish-rewrite`（v254 bundle 已在 `/tmp/vk254`）。
2. 停用自建路徑：`attachPlatformVif()` 內的 `createVirtualInterface` /
   `publishVenturaVifBsdUnit` / 手塞 ivar `+0x200` 全部改成可關（暫以 boot-arg 或
   `#if` 切換，先不要刪，方便 A/B）。
3. 讓 `createInterface()` 的物件當**唯一**介面，`start()` 內照權威順序：
   `attachInterface((IONetworkInterface **)&fNetIf, true)` → `registerService()` →
   `fNetIf->registerService()`。
4. 驗收（依序）：
   - `ioreg -c IO80211InfraInterface` ≥ 1
   - `ioreg` 上該介面有 `BSD Name`
   - `Apple80211GetVirtualIfListCopy` 不再回空
   - `tools/iocs.c`：STATE(13)/OP_MODE(15) 回 0
   - `cwjoin`：`-3903` → `-3905`（只剩完成事件）
   - `ifconfig en1` 仍 `active` 且能上網（**這條最容易被改壞，先驗它**）
5. 若 en1 掉了：`deploy.sh /tmp/vk254` 立即回滾。

風險提醒：這會動到目前**唯一能上網**的路徑。改之前先確認回滾步驟跑得動。

---

## 4. 今天新增的工具（都在 `tools/`，可直接重用）

| 工具 | 用途 |
|---|---|
| `iocs.c` | 逐 IOC 探測（fork 隔離，一支崩掉不影響其他），印 rc 與前 4 bytes |
| `a2cipher.c` | 用完整 RSN 字典打 `Apple80211Associate2`；記錄 `RSN_IE` **必須是 CFDictionary** |
| `a2auth.c` | `AUTH_SEL_OVERRIDE` 1..12 掃描（全 `-3905`，證明那條 gate 過） |
| `a2-3903-sites.py` | 列出 Associate2 內 14 個 `-3903` 出口與前面幾句 |
| `a2gate.c` | 就地改分支（`0x93e1` / `0xa56b`）觀測 gate，mode 1/3/4/5 |
| `a2bit.c` | ref 位元實驗（手改 `ref+0x51` 會讓 framework 丟 NSException） |
| `refdump3.c` / `refflags2.c` | dump `ref+0x48..0x68`、各 IOC 後的 ref 旗標 |
| `refflags.c` | ref 0x40..0x70 before/after bind |

RE 基本功（記下來，省重做）：
- lldb **不可用**（非互動 session 拿不到 debug 權限、Developer mode 關、sudo 密碼不猜）。
- 替代法：把 `-3903` 常數就地改寫成編號 → 回傳值即「哪個出口」；
  或把 `jne` 改成 `jmp` 觀測下一道 gate。patch 前先 `vm_protect(…, RWX)`（成功）。
- userspace framework 的 dump：guest 內 `dlopen` + `dlsym` + 寫檔 + `base64 -i`（macOS 要 `-i`）。
- kernel cache 反組譯：`/tmp/kc2` + `/tmp/an.py list|dis|ptr`（live `0x1f…`/`0x20d…`，legacy `0x218…`）。
- `airportd` 本體在 guest `/usr/libexec/airportd`；`__associateToWiFiNetwork` IMP = `0x100011c8f`，
  Associate2 呼叫點 `0x100012886`，第 4 參數是 `[NSMutableDictionary dictionaryWithCapacity:0]`。

---

## 5. 已經排除的方向（別重跑）

- 補 capability 位元（`bits[5] |= 0x10`，v253）→ 無效，但仍保留（與 Apple 真值一致）。
- 改 `AUTH_SEL_OVERRIDE` 1..12 → 全 `-3905`，不是它。
- 換 cipher（u/m 1..5 共 25 組）→ 全 `-3903`，不是它。
- 調 replay counters（PTKSA 必須 short 4、GTKSA short 1，已修正）→ 仍 `-3903`。
- 加 `WPA_IE` 字典 / 兄弟鍵（BSSID/CHANNEL/AP_MODE）→ 只有 `AP_MODE=1` 變 `-3900`
  （那是「開 AP/IBSS」分支，不是 STA join）。
- 掃 vif role 1/6/7 → 對 `-3900` 無影響。
- 第二次跑 `log show` 找 airportd 說明 → airportd 的 logger 從未初始化，沒有更多輸出。

---

## 6. 安全與衛生

- 測試 AP 的 PSK 在 `src/kext/ventura/Ath10kAutoJoin.inc`（`ATH10K_AUTOJOIN_PSK`），
  也是 boot-args `ath10kjoin=` 的一部分 → **任何回覆／文件都不要貼出來**；
  `with_esp.sh` 會把 boot-args 原樣印出，用時注意不要複製。
- 腳本一律用 `grep -o … | sed` 在 guest 內取值，不要 echo 到 stdout。
