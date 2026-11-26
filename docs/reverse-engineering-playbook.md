# 逆向流程與社群做法（QCA6174 Ventura 原生 Wi-Fi）

這份文件記錄**怎麼逆向**這條路（工具、步驟、驗證準則、踩過的坑），以及**別人是怎麼做成的**
（OpenIntelWireless AirportItlwm），最後列出兩者的差距與下一步。

---

## 1. 為什麼一定要逆向：三個「用猜的就錯」的例子

| 例子 | 猜的結果 | 實際（量到的） |
|---|---|---|
| CARD_CAPABILITIES 佈局 | 以為是 `bits[0]=0x0f` + 每個 cipher 一個 byte | Apple 是 `[+0x04]=0xee6f`（u16）、`[+0x06]=0x61`、`[+0x09]=0x40`、`[+0x0c]=0x201`；放錯 byte 時 airportd 讀完 caps 就放棄（`-3903`） |
| `setAssociateRequest` 裡的 `[vptr+0xc78]` | 以為是 `getIO80211CommandGate`（照 `ZTV index × 8` 算） | 是 `useAppleRSNSupplicant`。正確換算是 **`[vptr + (ZTV index - 2) * 8]`**；算錯就改錯函式、量到完全不相關的結果 |
| `Ath10kAssocRaw` 是誰送的 | auto-join 與 airportd 的 `apple80211_assoc_data` **位元組完全相同**，直接看會把自動連線誤認成 airportd | 要配 `proc_selfpid()` 的 `Ath10kAssocWho` 才知道是誰；這一條讓「點選連線」多繞了三輪 |

結論：**每一個結論都要有一個可觀察的輸出**（ioreg 屬性 / serial log / 回傳碼 + errno），
沒有輸出就不算量到。

---

## 2. 工具鏈

### 2.1 從 dyld shared cache 挖核心／framework 的程式碼

| 工具 | 用途 |
|---|---|
| `tools/dump_a2.c` | 在 guest 用 `dlopen` + `dlsym` 取得符號**實際位址**，算出映像位移（`dlsym - 映像base`），把指定長度的機器碼寫成檔案。繞過「shared cache 裡的檔案在磁碟上不存在」的問題 |
| `tools/imgdump.c` / `tools/imgdump2.c` | dump 整個 `__TEXT` segment（給 `imgdump2` 一個 framework 路徑即可），回家離線用 capstone 掃 |
| `tools/apple80211-assoc.c`、`tools/a2push.c` | 直接對 driver 下 `ASSOCIATE(20)`（兩條不同通道：BSD ifnet ioctl vs `apple80211_ref` 的 fd） |
| `tools/a2fd.c` | 印 `apple80211_ref` 佈局（`+0x00=fd`、`+0x04="en1"`、`+0x50=能力鏡像`） |
| `tools/refflags.c` | 量「能力表被鏡射到 ref 的哪個位移」——用實測取代猜 |
| `tools/a2join.c` / `tools/a2schema.c` | 直接呼叫 `Apple80211Associate2`，探字典 schema |
| `tools/a2mon.c` | 先 `Apple80211EventMonitoringInit2` 再呼叫 Associate2（**驗 join 是否完成的正確 harness**） |

### 2.2 反組譯與符號還原（在 Linux 主機上，Guest 只負責 dump）

| 工具 | 用途 |
|---|---|
| `/tmp/an.py dis <位址> <bytes>` | capstone 反組譯，自動把 rip-relative 目標標成 `; @符號名` |
| `/tmp/an.py list <關鍵字>` | 從 kernel cache 的符號表查函式位址／反查名字（`an.py list postMessage`） |
| `/tmp/an.py vtable <位址> <n>` | dump vtable 並標上函式名（**注意 index → 位移換算規則**） |
| `tools/resolve.py` / `tools/annotate.py` | 解析 airportd（universal binary）裡 `__cfstring` / selref 的 rip-relative 目標，含 **DYLD_CHAINED_PTR_64** 解碼；`annotate.py` 可直接把 `otool -tV` 的一段行號標上字串 |
| `otool -tV /usr/libexec/airportd` | 使用者態反組譯（258k 行）；`nm -u` 看它 import 了哪些 Apple80211 符號 |

### 2.3 量測（車上）

| 工具 | 用途 |
|---|---|
| ioreg breadcrumb | 每個 gate 一顆屬性，命名固定 `Ath10k<主題>`（`AssocRaw`／`AssocWho`／`AssocDefer`／`KeyFallback`／`LinkEvent`…） |
| `tr -d '\000' < /tmp/ventura-vm.serial` | `IOLog` 全部走序列埠，比 unified log 可靠（airportd 的 logger 從未初始化） |
| `tools/jointime2.sh` | 每 0.4 秒取一次狀態，量「join 請求 → 連上」的真實延遲 |
| `tools/cwjoin.m` / `cwscan.m` | 走 CoreWLAN → airportd → family → driver 的**真實使用者路徑** |

---

## 3. 標準流程（照這個順序做，不要跳）

1. **先量現況**：`ioreg` breadcrumb + serial，確認「請求有沒有到達」、「到達後走到哪一階段」。
2. **定位兇手**：用 `an.py list` 找符號 → `dis` 反組譯 → 找出「回這個錯誤碼」的那一個 `mov`，
   再把它的條件往回追（誰寫的 flag、誰設的長度）。
3. **確認換算規則**：vtable／struct offset／chained fixup 這三種「位移」一定要先驗一次
   （用一個已知答案的案例對照）。
4. **一次只改一個 gate**，同時保留前一次的 breadcrumb，避免「改了三個、不知道哪個有效」。
5. **用真實 client 驗**：自己寫的 harness 只能證明「kernel 收得到」，
   要證明「使用者點得動」必須走 `cwjoin`（airportd）。兩者的失敗碼不同（-3905 vs -3903）就是證據。
6. **失敗也要留紀錄**：把負面結果寫進 HANDOFF（哪個猜測被否證、怎麼否證的），避免下一輪重試。
7. **收尾**：回到乾淨基線（`system_reset` 一次），確認 `connected-wpa2` + IPv4 + ping 通。

---

## 4. 陷阱清單（全部是實測踩到的）

- **`Ath10kEntryTrace` 只有 12 格**，airportd 每秒幾十個 getter 會瞬間洗掉；重要事件要有獨立屬性。
- **`Ath10kAssocRaw` 無法區分呼叫者**，一定要配 `Ath10kAssocWho`（`proc_selfpid` + `proc_selfname`）。
- **guest 的 `/tmp` 每次重開清空**；工具每次都要重傳（或放進 repo `tools/` 再傳）。
- **macOS 的 `base64` 要 `-i`**，`base64 file` 只會印 usage。
- **shared cache 的 `__DATA` 指標是 chained fixup**：`target = 0x100000000 + (p & 0xFFFFFFFFF)`，
   bit63 = bind（外部符號，要用 `dyld_info -fixups` 查名字）。
- **`leaq` 的目標是物件本身、`movq` 的目標是槽**：解析字串時兩者要分開處理（`resolve.py` 的 `kind` 參數）。
- **`__interpose` 對 shared cache 的 direct bind 無效**（試過 `CFDictionaryGetValue`，完全沒攔到）。
- **SIP 的 Debugging Restrictions 開啟時 lldb 不能 attach**（`attach failed: cannot get permission`），
  而 `nvram csr-active-config=%7f` 會被擋（`(iokit/common) not permitted`）→ 別再試這兩條。
- **`setAssociateRequest` 的轉發條件**：`[controller ivars+0x1a0]` 必須是 role 6 的 vif、
   `[assoc_data+0x1cf] & 1` 決定走 BSSID-get 分支；ivar 是 null 時 ASSOCIATE 永遠不會轉發給驅動。

---

## 5. 社群做法：OpenIntelWireless AirportItlwm（Ventura 分支）

同樣是「用 Apple 自己的 IO80211 家族 + 自製驅動」，而且是**能用的**（Ventura 支援見 itlwm issue #784 / #841，
v2.2.0 起）。它的原始碼就是最好的參考實作：

### 5.1 關聯完成：`setLinkState` + `setRunningState` + `postMessage(NULL)`

`AirportItlwm/AirportItlwmV2.cpp`（`setLinkStateGated`）：

```cpp
that->fNetIf->setLinkState((IO80211LinkState)arg0, (unsigned int)arg1);
that->fNetIf->setRunningState(arg0 == kIO80211NetworkLinkUp);
that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, false);
that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, false);
if (up) that->fNetIf->reportLinkStatus(3, 0x80);
else    that->fNetIf->reportLinkStatus(1, 0);
```

`eventHandler`：

```cpp
case IEEE80211_EVT_STA_ASSOC_DONE: interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0); break;
case IEEE80211_EVT_STA_DEAUTH:     interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0); break;
```

**我們已經照做（v229）**：`venturaPostLinkEvents()` 現在會 `setLinkState`（實測
`Ath10kLinkEvent = up=1 link=2`＝`kIO80211NetworkLinkUp`）、`setRunningState`、
`postMessage(LINK_CHANGED/BSSID_CHANGED/SSID_CHANGED/ASSOC_DONE/RSN_HANDSHAKE_DONE, NULL, 0)`。

### 5.2 三個 ioctl 的實作（我們可以逐項對照）

`AirportItlwm/AirportSTAIOCTL.cpp`：

| ioctl | itlwm 做法 | 我們的現況 |
|---|---|---|
| `ASSOCIATE (20)` | 直接 `associateSSID(ad_ssid, len, bssid, auth_lower, auth_upper, key, key_len, index)`，**不做版本/gate 檢查** | 有做，但多了 defer / 金鑰 fallback；`assoc_data` 是 **32-byte PMK**（`0xa25c`）而不是密語 |
| `ASSOCIATE_RESULT (21)` | **只有 state == `IEEE80211_S_RUN` 才回 `kIOReturnSuccess` + `result=SUCCESS`，否則回 `kIOReturnError`** | ✅ v230 已改成同樣語意（未連線回 `kIOReturnError`） |
| `LINK_CHANGED_EVENT_DATA (156)` | 填 `isLinkDown` / `voluntary` / `reason` / `rssi` | ✅ v230 已實作（未連線 → `isLinkDown=1` + `DEAUTH`；已授權 → `rssi=-50`） |
| `RSN_HANDSHAKE_DONE` | 4-way 完成後 `postMessage(APPLE80211_M_RSN_HANDSHAKE_DONE)` | 已加（v229） |

### 5.3 它跟我們最大的架構差異

- itlwm 的 join **完全由驅動自己跑**（`associateSSID()` 直接進 802.11 狀態機），
  airportd 只是下指令 + 等事件；它不做「defer → teardown → replay」。
- itlwm 用 `reportLinkStatus()` 把 link 品質往上報；我們只發 ioreg 屬性。
- itlwm 沒有我們這種「airportd 送空金鑰」的問題，因為它的 `setASSOCIATE` 直接吃
  `ad_key.key`（airportd 給的密語），不需要 fallback。

---

## 6. 兩條路線的現況（v230 實測）

| 路徑 | 結果 |
|---|---|
| `tools/a2push.c`（直接用 ref 的 fd 送 type 20） | `rc=0`，defer→replay→`connected-wpa2`，全程約 2 秒 |
| `tools/a2join.c`（我們自己叫 `Apple80211Associate2`） | 每次都 `elapsed=10.2s rc=-3905`（**沒有事件讀者，註定逾時**，不是 join 失敗） |
| `tools/cwjoin.m`（airportd 真實路徑） | `assoc-ok=0 -3903`，但 `ssid-now=QCA-TEST rssi=-41` → 連線其實是通的 |

也就是說：**驅動、family、事件、link state 都已經對齊可用的社群實作**；
剩下的 `-3903` 在 airportd 自己的 userspace 決策，不是驅動擋的。

---

## 7. 下一步（按優先序）

1. **把 `ASSOCIATE_RESULT` 改成 itlwm 的語意**（非 RUN 狀態回 error），並實作
   `LINK_CHANGED_EVENT_DATA(156)` 的真實內容 → 這是社群實作與我們差最多的兩項。
2. 把 `a2mon` 的事件監聽參數調對（`Apple80211EventMonitoringInit2` 第二參數不能是
   `mach_task_self()`），用它在**我們自己的行程**裡驗證 join 回 0。
3. airportd 的 `-3903`：只剩 userspace 分支（§6.40.2 出口 A 已排除、出口 B 的能力位已證實為 1），
   下一步是攔 `Apple80211Associate2` 前後的請求軌跡（`Ath10kSetSeen` 已有專用記錄，
   可再加一顆「最近 N 個 set 的 type + pid」環形緩衝）。
