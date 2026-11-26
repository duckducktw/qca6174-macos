# 交接：蘋果「放行 join」的條件（逆向結果）＋ 社群做法對照

> 這份文件只談一件事：**要讓 Apple 的 stack 真的放行一次「點選連線」，每一層各自要求什麼**，
> 以及實測我們現在卡在哪一格。每一條都附上反組譯位址或 log／ioreg 證據。
>
> 專案：QCA6174 原生 Wi-Fi（Ventura / IO80211 家族）。
> 相關文件：`docs/HANDOFF.md`（逐輪紀錄）、`docs/apple-vs-ours.md`（逐 API 對照）、
> `docs/reverse-engineering-playbook.md`（工具與流程）。

---

## 0. 一句話總結

蘋果的放行條件是**四層串聯**的 AND：

```
airportd/CoreWLAN 願不願意發（userspace 判斷）
  AND Apple80211Associate2 願不願意送（framework 的 password / 安全型別 / 能力位）
    AND IO80211Family 願不願意轉發（setAssociateRequest 的 ivar、role、predicate）
      AND 驅動回報的狀態讓前面三層都滿意（link state、事件、ASSOCIATE_RESULT、scan 資料）
```

任何一層說「不」，使用者的結論都是同一句 **`-3903`**（或包裝後的 `error 37`／`102`／`-3905`）。
這就是為什麼「-3903」不能當作診斷線索，必須逐層量。

我們目前 **第 2、3、4 層的已知條件全部對齊**；卡在 **第 1 層**：
airportd 只做 getter 探測（`g12 g19 g96 g1 g1 g2 g15 g4 …`），
**連 `Set(SSID)` 都不送**，所以後面三層根本沒有機會被觸發。

---

## 1. 第 1 層：airportd / CoreWLAN 的放行條件（userspace）

### 1.1 它會先「只讀不寫」地問一輪

實測 `Ath10kClientTrace`（只記使用者態 client 的請求，含 pid 與狀態）：

```
g12:166:0  g19:166:0  g96:166:0  g15:166:0  g15:166:0  g19:166:0  g15:166:0  g15:166:0
s22:166:0  g254:166:0 g19:166:0  g15:166:0  g15:166:0  g1:166:0   g103:166:0 g1:166:0
```

| type | 名稱 | 放行要求 |
|---|---|---|
| 12 | `CARD_CAPABILITIES` | 佈局必須與 Apple 一致（`[+0x04]=0xee6f`、`[+0x06]=0x61`、`[+0x09]=0x40/0x41`、`[+0x0c]=0x201`）；放錯 byte 時它讀完 caps 就放棄 |
| 19 | `POWER` | 必須回「開」 |
| 96 | `VIRTUAL_IF_ROLE` | 主站台介面 role 0；拒絕會被當成「不支援」 |
| 15 | `OP_MODE` | 必須是 STA |
| 1 | `SSID` | 佈局是 **flat（32 bytes 名稱 + 長度）**，不是 versioned struct |
| 2 | `AUTH_TYPE` | 未連線時**不能回 NONE**，要回支援集合（OPEN/WPA/WPA2/WPA3） |
| 4 | `CHANNEL` | 回 pin 的頻道／現行頻道／目標 BSS 頻道（回 0 會被當成「沒有可用頻道」） |
| 27/254 | `SUPPORTED_CHANNELS` / `HW_…` | 回完整表 |
| 103 | `CURRENT_NETWORK` | 未連線時 `num_bss=0` |

### 1.2 它自己會擋掉的兩種狀態（airportd 的 WiFiManager log 原文）

```
AUTO-JOIN: Auto-join aborted … error=(37 'Already associated, and network transition not permitted')
AUTO-JOIN: Auto-join aborted … error=(82 'WiFi interface is OFF')
```

- **error 37** = airportd 認為介面「已關聯」。它的依據是 **BSD 介面的 link 狀態**
  （`ifconfig en1` 的 `status:`），不是驅動 ioreg 裡的狀態。
  → 驅動必須在拆線時把 link 狀態推到**已發佈的介面**（見 §3.4），否則永遠 37。
- **error 82** = 無線關閉。

### 1.3 scan 快取的放行條件

```
BEGIN REQ [GET CURR SCAN RESULT] … END REQ … err=2      ; 沒有可用的掃描結果
```

`err=2` 來自 **wifivelocityd / configd** 對「目前掃描結果」的查詢，不是 join 的必經步驟
（實測 join 失敗時這些查詢同時存在，兩者無因果）。但 scan manager 這條路徑本身有硬條件：

- `IO80211ScanManager::queryScanResult`（`0xffffff8002013f80`）直接 tail-call 到
  `[vptr+0xcb0]`，參數是 **`(controller, cmd=0xc03069c9, type=11, vif, out=r8)`**；
  `[[ivars]+0xa0]` 為 null 時回 `0x66`。
- `startScan()` 會先跑 `validateScanData`（條件見 §3.5），通過才建立
  `apple80211scan_req_node`；**沒有 node，之後所有快取查詢都失敗**。

### 1.4 它願意發 join 的順序（Apple 自己的實作）

`Apple80211Associate2`（airportd 的 import，呼叫點 `0x100012886`）：

```
Set(SSID) → Set(AUTH_TYPE) → [Set(BSSID) → Set(CHANNEL)] → Apple80211Associate2
```

實測我們的 trace 從來沒有出現 `s1`／`s9`／`s4`／`s20`，
代表它在 §1.1／§1.2 的判斷階段就結束了 —— **不是我們回錯值，而是它沒走到寫入階段**。

---

## 2. 第 2 層：`Apple80211Associate2` 的放行條件（framework，userspace）

`Apple80211Associate2` 映像位移 `0x8b3a`。它自己組 `apple80211_assoc_data` 並用
`ioctl(fd, 0x802869c8, req{type=20, len=0x250})` 送出（**繞過 `Apple80211Set` 的型別白名單**）。

### 2.1 三個必要條件

| # | 條件 | 證據 |
|---|---|---|
| A | **密碼參數非 nil**，且 `[ref+0x51] & 0x40` 成立 | `0x93d6 test r14,r14; je 0x93e7 → -3903`；`0x93db test byte [r12+0x51],0x40`。實測 bind 後 ref 旗標 `0xa073ee6f` → 這一位是 1 |
| B | 字典的**安全型別是 1..12**，且該型別的 case 需要的能力位成立 | `0x97cc` 取 CFNumber → `0x97d2 lea edx,[rax-1]; cmp edx,0xb` → 跳表 `0xb014[type-1]`；型別 8 → `0x980a`，要求 `[ref+0x59] & 8` |
| C | 送出的 `ad_key` 是 **32-byte PMK**（`key_len=0x20`、`cipher=6`） | `0xa25c movabs rax,0x600000020; mov [rbp-0x264],rax`，前兩行把 32 bytes 搬進 `ad_key.key` |

### 2.2 `apple80211_ref` 佈局（決定上面那些位在哪）

`tools/refflags.c` 實測：

| 位移 | 值 | 意義 |
|---|---|---|
| `+0x00` | fd | 送 ioctl 用的 fd |
| `+0x04` | `"en1"` | 綁定的介面名 |
| `+0x50` | `0xa073ee6f`（bind 後） | 旗標字；`+0x51` bit6 (`0x40`) 是 Associate2 條件 A 的一半 |
| `+0x54` | `6f ee 73 a0 04 41 …` | **能力表鏡像**：`ref+0x54 == capabilities[0]` |

→ 因此 **`ref+0x59 == capabilities[5]`**。實測 `capabilities[5]` 原本就是 `0x41`，
補上 bit3 變 `0x49`，但**對 join 沒有影響**（條件 B 不是我們的卡點）。

### 2.3 另一個 framework 白名單（注意：不是這條路）

`Apple80211Set`（公開 API）對小 type 有位圖 `0x08089000`（只放行 12/15/19/27），
type 20 會落到 `0x…3b11` 直接回 `-3903`。
**但 airportd 走的是 Associate2 的自建 ioctl，不受這個位圖限制。**
（這是前幾輪一度誤判成「只能關 SIP 改 framework」的原因。）

---

## 3. 第 3 層：IO80211Family 的放行條件（kernel）

### 3.1 請求入口：兩條路

`IO80211Controller::apple80211RequestIoctl`（`0x1fe3e5c`）：

```
rax = [this+0x688] → [rax+0x138]  ; 有 primary skywalk 介面且就緒？
true  → jmp [vptr+0xcb0]          ; 快路徑：我們的 apple80211_ioctl(Skywalk,…)
false → call [vptr+0x388]         ; fallback
```

### 3.2 `setAssociateRequest`（`0x1fe2533`）—— 轉發 ASSOCIATE 前的四個條件

```
req->req_len != 0 且 req->req_data != null                    ; 否則錯誤
[assoc_data+0x1cf] & 1                                        ; 決定走 BSSID-get 分支或直接轉發
[controller ivars+0x1a0] 必須存在，且是 role 6 的 vif          ; 我們原本是 null → v13 補
[該 vif ivars+0x30] 物件的 [vptr+0x1e0] 必須回 false           ; v234 實測 = 0（否證）
轉發：[vptr+0xcb0](controller, 0x803069c8, 0x14, iface, assoc_data)   ; 5 個參數
```

**注意呼叫形狀**：`(controller, cmd, type, iface, payload)` —— type 在**第三個**暫存器。
我們 v220 起改成 5 參數、v242 起才正確區分「第三個是 type」與「第三個是 request block」。

### 3.3 驅動回 success 之後的第四個條件

```
ax = [assoc_data+8]                       ; ad_auth_upper
if (ax > 8 || !bt(0x116, ax)) → return 0
call [vptr+0xc78](controller, vif)        ; = useAppleRSNSupplicant（ZTV slot 401）
if (true) { [vif ivars+0x118]+0x20 = now+15 ; return 6 }
return 0
```

`useAppleRSNSupplicant` 若回 true，family 就把 4-way 交給**只存在於 legacy bundle** 的
supplicant，並回 6 給呼叫端（airportd 就報 -3903）。**必須回 false**（v213 起已強制）。
（vtable 換算規則：`[vptr + (ZTV index − 2) × 8]`；算錯會改到 `getIO80211CommandGate`。）

### 3.4 link 狀態必須推到「已發佈的介面」

- `IO80211InfraInterface::setLinkState(IO80211LinkState, bool, unsigned)`（live `0x2040406`）
  才是介面（BSD 看到的 en1）狀態的來源。
- **`IO80211Interface::setLinkState` 只存在於 legacy bundle**（`0x218xxxxx`），
  引用它會讓注入的 kext **靜默載入失敗**（serial 沒有 `Ath10kMac`、`ifconfig -l` 少一個介面）。
- 沒推到介面 → `ifconfig en1` 永遠 `status: active` → airportd error 37（§1.2）。

### 3.5 scan manager 的請求驗證（`getCachedScanResults` `0x201150a`）

```
[data+0x04] - 1 > 2                          → 0x16      ; bss_type 必須 1..3
[data+0x10]     > 0x20                       → 0x16      ; ssid_len ≤ 32
[data+0x34] > 9 或 !bt(0x38e, [data+0x34])   → 0x16      ; scan_type ∈ {1,2,3,7,8,9}
[data+0x44]     > 0x190                      → 0x16
```

不通過 → 沒有 request node → 之後所有 `GET CURR SCAN RESULT` 都 `err=2`（v245 已做 sanitize）。

### 3.6 掃描結果的形狀

- 一次呼叫 **一筆** `apple80211_scan_result`（1164 bytes），**非 0 回傳**＝結束。
- `asr_bssid` @ `0x1c`、`asr_nssid_len` @ `0x60`、`asr_ssid` @ `0x61`、`asr_age` @ `0x84`
  （BSD ioctl 路徑實測：`QCA-TEST bssid=f0:68:e3:2d:40:22 ch=6 ie=22`）。
- `CoreWLAN` 的 `bssid=(nil)` 與 airportd log 的 `BSSID:01:00:00:00:00:00` 是
  **client 端佔位符**（把 `version` 改成 `0xABCD1234` 後仍印 `01:00:…`，證明不是讀 offset 0）。

---

## 4. 第 4 層：驅動要「回得讓前面三層滿意」的清單

| 項目 | Apple 的預期 | 我們（最新） | 狀態 |
|---|---|---|---|
| `ASSOCIATE_RESULT(21)` | ioctl 成功；`result` 用 enum 表達（`UNAVAILABLE=0` / `SUCCESS=1`），只有 RUN 才是 SUCCESS | v243：同 | ✅ |
| `LINK_CHANGED_EVENT_DATA(156)` | 填 `isLinkDown`/`voluntary`/`reason`/`rssi` | v230：同 | ✅ |
| 事件 | `postMessage(LINK/BSSID/SSID_CHANGED, NULL, 0)`、`M_ASSOC_DONE`、`M_RSN_HANDSHAKE_DONE` + kern_event 橋 | v229/230：同（`Ath10kLinkEvent = up=1 link=2 kev=0/0`） | ✅ |
| link 狀態 | `setLinkState`(Infra) + `setRunningState` + BSD 介面 | v240/241：`iface-link=0/1`、`ifconfig status: inactive/active` | ✅ |
| `DISASSOCIATE(22)` | 對外呈現 idle（SSID/BSSID/CURRENT_NETWORK/STATE） | v232：`_venturaDisassociated` | ✅ |
| `BSSID` getter | pin 過的值優先 | v246 | ✅ |
| `CHANNEL` getter | 不可回 0（pin／現行／目標 BSS） | v247 | ✅ |
| `CARD_CAPABILITIES` | Apple 佈局 + `capabilities[5]` bit3 | 已對齊（bit3 實測對 join 無影響） | ✅ |
| ASSOCIATE 的完成延遲 | 必須落在 `Apple80211Associate2` 的 **10.2 秒**窗內 | defer→teardown→replay 實測 **9–11 秒**，常在邊緣 | ⚠️ |
| scan manager 請求 | `validateScanData` 白名單 | v245 sanitize（`Ath10kScanMgrStart=ok`） | ✅ |
| 自動連線 | 不可與 airportd 爭 | v241：disassociate 後壓 15 秒（有界，不累加） | ✅ |

---

## 5. 社群做法：OpenIntelWireless／AirportItlwm（Ventura 可用）

同樣是「Apple IO80211 家族 + 自製驅動」，而且**點選連線可用**。它的原始碼就是本專案的參考實作。

### 5.1 它如何滿足第 4 層（逐項對照我們的實作）

`AirportItlwm/AirportItlwmV2.cpp`：

```cpp
// setLinkStateGated
that->fNetIf->setLinkState((IO80211LinkState)arg0, (unsigned int)arg1);
that->fNetIf->setRunningState(arg0 == kIO80211NetworkLinkUp);
that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED,  NULL, 0, false);
that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED,  NULL, 0, false);
if (up) that->fNetIf->reportLinkStatus(3, 0x80); else that->fNetIf->reportLinkStatus(1, 0);

// eventHandler
case IEEE80211_EVT_STA_ASSOC_DONE: interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0); break;
case IEEE80211_EVT_STA_DEAUTH:     interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0); break;
```

`AirportItlwm/AirportSTAIOCTL.cpp`：

| ioctl | itlwm 做法 | 我們的對應 |
|---|---|---|
| `ASSOCIATE(20)` | 直接 `associateSSID(ad_ssid, len, bssid, auth_lower, auth_upper, key, key_len, index)`，不做版本/gate 檢查 | 有 defer/fallback，語意等價 |
| `ASSOCIATE_RESULT(21)` | **只有 `ic_state == IEEE80211_S_RUN` 才回 success**，否則 `kIOReturnError` | v243 改成「ioctl 成功 + result enum」（更符合 Apple enum，且避免 client 把 error 當 102） |
| `LINK_CHANGED_EVENT_DATA(156)` | 填 `isLinkDown`/`voluntary`/`reason`/`rssi` | v230 |
| `RSN_HANDSHAKE_DONE` | 4-way 完成後 `postMessage(M_RSN_HANDSHAKE_DONE)` | v229 |

### 5.2 架構上的三個關鍵差異（為什麼它沒有我們這些坑）

1. **它不跟 airportd 搶**：沒有「驅動自己先連上」的 watchdog，因此不會觸發 airportd 的
   `error 37 Already associated`。我們用「disassociate 後壓 15 秒」規避（v241）。
2. **它不用 `Apple80211Associate2` 的字典**：`setASSOCIATE` 直接吃 `ad_key.key`（airportd 給的密語），
   所以沒遇過「字典裡金鑰材料形狀不對 → 送出 keylen=0」的問題。
3. **它的掃描結果天生帶 BSSID**（透過 802.11 堆疊的 `ni_bssid`），所以 airportd 的
   `BSSDetails` 不會是佔位符。

### 5.3 可直接照抄的部分

- `setLinkState` + `setRunningState` + 三個 `postMessage(NULL)` + `reportLinkStatus` 的**順序**。
- `ASSOCIATE_RESULT` 的「以狀態表達可用性」原則（不要用 ioctl 錯誤碼表達「還沒好」）。
- `LINK_CHANGED_EVENT_DATA` 的欄位填法。
- 「驅動不主動 join，讓 airportd 主導」的整體策略。

---

## 6. 我們現在的準確位置

```
第 1 層（airportd 願不願意發）      ← 卡在這裡
第 2 層（Associate2 願不願意送）    ✅ 條件 A/B/C 都已量過（B 的能力位不是卡點）
第 3 層（family 願不願意轉發）      ✅ ivar +0x1a0、role 6、predicate=0、5 參數、RSN gate=false
第 4 層（驅動回報是否合格）         ✅ 除「完成延遲 9–11s vs 10.2s 窗」外全部對齊
```

**第 1 層的證據**：`Ath10kClientTrace` 只有 getter
（`g12 g19 g96 g1 g1 g2 g15 g4 …`），**沒有任何 set**。
airportd 在這一步之後沒有任何寫入，所以 `Apple80211Associate2` 從未被呼叫。

---

## 7. 下一步（建議順序，每步都有可驗證輸出）

1. **量 airportd 眼中的介面清單**：它用 `Apple80211GetIfListCopy`
   （log：`getifaddrs nInterfaces[20], count[1]`）。
   把驅動對這條路徑回報的属性（`IO80211InterfaceRole`、`IOInterfaceName`、BSD name）
   與 Apple 驅動逐欄比對；`count` 與 role 是它決定「這是不是可 join 的 Wi-Fi 介面」的依據。
2. **把 `s1`(Set SSID) 餵進去**：既然 airportd 不送，就用 `networksetup -setairportnetwork`
   之外的路徑（例如 `Wireless Diagnostics` 或 `airportd` 的 XPC）確認它在什麼輸入下才會進入寫入階段。
3. **若第 1 層確認是 `IO80211InterfaceRole`／介面清單問題**，就在 `attachPlatformVif`
   補齊對應屬性（我們已會 stamp `IO80211InterfaceRole=Infrastructure`，需要確認是哪個節點被讀）。
4. 最後再回來處理「join 延遲 < 10.2 秒」：縮短 `serviceDisconnect` 的 Drain→Barrier 階段，
   或同 SSID 時走「已滿足」快路徑（v226 已有，需確認真的被命中）。

---

## 8. 量測工具（都用得到，全部在 `tools/`）

| 工具 | 用途 |
|---|---|
| `a2push.c` | 用 `apple80211_ref` 的 fd 直接送 `ASSOCIATE(20)`（證明 kernel 側通不通） |
| `a2join.c` / `a2mon.c` | 直接呼叫 `Apple80211Associate2`（含／不含事件監聽） |
| `a2dict.c` | 逐一加鍵，找哪個鍵改變 Associate2 的行為 |
| `a2fd.c` / `refflags.c` | `apple80211_ref` 佈局與能力鏡像位移 |
| `dump_a2.c` / `imgdump2.c` | 把 shared cache 裡的函式／`__TEXT` dump 出來離線反組譯 |
| `resolve.py` / `annotate.py` | 解析 airportd 的 `__cfstring`/selref（含 chained fixup） |
| `joincheck.sh` / `after.sh` / `whyfail.sh` | 一次 join 後同時取 cwjoin 結論、驅動 breadcrumb、airportd log |
| `cwscan.m` / `cwjoin.m` | CoreWLAN 路徑的掃描與連線 |

**診斷鐵則（這一輪學到的）**：
1. `ioreg` 的 breadcrumb 一定要 **pid 歸屬**（`Ath10kAssocWho`）與 **獨立環形緩衝**
   （`Ath10kClientTrace`）；12 格的 `EntryTrace` 會被 getter 海嘯洗掉。
2. `-3903` 不是診斷資訊，只是「有一層說不」。要往上看是哪一層。
3. 錯誤碼的**演進**本身就是進度表（`-3903` → `102` → `-3903` 但 log 已前進）。
4. deploy 後先確認 **kext 真的載入**（serial 有 `Ath10kMac`、`ifconfig -l` 有 en1），
   再解讀行為；引用 legacy-only 符號會造成靜默載入失敗。
