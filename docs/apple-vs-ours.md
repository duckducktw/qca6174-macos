# 蘋果預期 vs 我們實際行為（join 路徑逐項對照）

每一列都是「從 live KC／airportd 反組譯或 log 得到的預期」對上「本驅動實測行為」。
沒有證據的項目不會出現在這張表裡。

---

## 1. userspace → family：請求的入口契約

| # | Apple 的預期（證據） | 我們的實際（證據） | 差異 |
|---|---|---|---|
| 1.1 | `IO80211Controller::apple80211RequestIoctl`（`0x1fe3e5c`）先問 `[[this]+0x688]` 再問該物件的 `[+0x138]`，真→走 `[vptr+0xcb0]`（Skywalk ioctl），假→fallback `[vptr+0x388]` | 兩條都實作，實測請求都進得來（`Ath10kIoctlArgs`） | 一致 |
| 1.2 | `setAssociateRequest`（`0x1fe2533`）轉發前要求：`req_data != null`、`req_len != 0`、`[[ctrl ivars]+0x1a0]` 是 **role 6** 的 vif、該 vif `ivars[0x30]` 的 `[vptr+0x1e0]` 回 **false** | ivar `+0x1a0` 原本是 **null → 已補**（`Ath10kCtrlPrimaryBefore=null` / `After=set`）；predicate 實測 **0**（`Ath10kAssocGate=gate=… result=0`） | 已對齊 |
| 1.3 | 轉發呼叫形狀是 **5 個參數** `(controller, cmd, type, iface, payload)` | v220 起 slot 406 宣告 5 參數；v242 起還區分「第三個參數是 type（<0x10000）」與「第三個是 request block」 | v242 才對齊 |
| 1.4 | `IO80211ScanManager::queryScanResult`（`0x2013f80`）直接 tail-call 到 `[vptr+0xcb0]`，`cmd=0xc03069c9`、`type=11`，**結果指標在 r8**；`[[ivars]+0xa0]` 為 null 時回 `0x66` | v242 前：把第三個參數（type=11）當使用者位址 copyIn → 失敗 → 落到 Apple 實作回 `0x66`；v242 後走 `venturaScanResult(payload)` | **v242 修正**；`GET CURR SCAN RESULT err=2` 已從每次變成部分 `err=0`（`Ath10kScanServeTrace=167:12 167:15 …`） |

---

## 2. 關聯（assoc）本身

| # | Apple 的預期 | 我們的實際 | 差異 |
|---|---|---|---|
| 2.1 | `apple80211_assoc_data` 的 `ad_key` 裝 **32-byte PMK**（`0xa25c movabs 0x600000020` → `key_len=32, cipher=6`），金鑰材料來自呼叫端字典 | 我們接受「密語」或「PMK」；airportd 送來的是 `keylen=0`（空金鑰），由 `Ath10kAssocKeyFallback=join-list` 用驅動自己的 PSK 補上 | 行為不同但**結果等價**（都能連上） |
| 2.2 | 關聯完成後送 `M_ASSOC_DONE`（社群實作 `AirportItlwmV2.cpp eventHandler`） | v230 起送 `M_ASSOC_DONE` + `M_RSN_HANDSHAKE_DONE` + `M_LINK/BSSID/SSID_CHANGED`（NULL payload） | 已對齊 |
| 2.3 | 一次 join 應該在上游 10.2 秒的等待窗內完成（`Apple80211Associate2` 逾期回 `-3905`） | 我們的 defer→teardown→replay 實測 **9–11 秒**，常常剛好超窗 | **未解**：需要縮短 teardown，或同網路時走「已滿足」快路徑（v226 已加） |
| 2.4 | `ASSOCIATE_RESULT(21)`：只有 link 真的 RUN 才回 success，否則回 error（`AirportItlwm getASSOCIATE_RESULT`） | v230 起同樣語意 | 已對齊 |
| 2.5 | `useAppleRSNSupplicant` 回 **false** 時由驅動自己跑 4-way（否則 family 回 6、把 4-way 交給只存在 legacy 的 supplicant） | v213 起強制 false | 已對齊 |

---

## 3. link 狀態回報（airportd 判斷「已關聯」的依據）

| # | Apple 的預期 | 我們的實際 | 差異 |
|---|---|---|---|
| 3.1 | 介面 link 狀態要走 `IO80211InfraInterface::setLinkState(IO80211LinkState, bool, unsigned)`（live `0x2040406`） | v240 前只呼叫 controller 的 `setLinkStatus()`，BSD 介面永遠 `status: active`；v240 起推導到 infra interface | 已對齊（`Ath10kIfaceLink=iface-link=0 ok=1` → `status: inactive`） |
| 3.2 | `setRunningState(bool)` 與 link 狀態同步；`LINK_CHANGED`/`BSSID_CHANGED`/`SSID_CHANGED` 三連發 | v230 起照做（`Ath10kLinkEvent=up=1 link=2 kev=0/0`） | 已對齊 |
| 3.3 | `LINK_CHANGED_EVENT_DATA(156)` 要填 `isLinkDown`/`voluntary`/`reason`/`rssi` | v230 起實作（原本落到 default 回全零） | 已對齊 |
| 3.4 | `DISASSOCIATE(22)` 之後介面必須是 idle：SSID/BSSID/CURRENT_NETWORK/STATE 都空 | v232 起 getter 回報 idle（`_venturaDisassociated`） | 已對齊 |

---

## 4. 掃描結果（airportd 的 `err=2` 來源）

| # | Apple 的預期 | 我們的實際 | 差異 |
|---|---|---|---|
| 4.1 | 一次呼叫回 **一筆** `apple80211_scan_result`（1164 bytes），**非 0 回傳**代表結束（client 以此結束讀取迴圈） | `venturaScanResult()` 就是這樣做（BSD ioctl 路徑實測 15–21 筆、`QCA-TEST bssid=f0:68:e3:2d:40:22 ch=6 ie=22`） | 一致 |
| 4.2 | 走 scan manager 時結果指標在 **r8**，且 type 在第三個參數 | v242 前完全沒處理 → `0x66`；v242 後 `Ath10kScanServeTrace` 出現多筆成功服務 | **v242 修正** |
| 4.3 | scan manager 需要有 **scan request node**（`apple80211scan_req_node`）才能把結果給後來的查詢（`getCachedScanResults`） | 我們只服務 type 11 的讀取，**沒有建立/維護 request node** | **未解**：`GET CURR SCAN RESULT err=2` 仍會出現（部分 err=0） |
| 4.4 | `asr_bssid` 在 0x1c、`asr_ssid_len` 0x60、`asr_ssid` 0x61（Apple header 與 BSD 路徑實測一致） | 同 | 一致。CoreWLAN 的 `bssid=(nil)` 與 airportd 的 `BSSID:01:00:00:00:00:00` 是 **client 端佔位符**（把 version 改成 `0xABCD1234` 後仍是 01:00:…，證明不是讀 offset 0） |

---

## 5. 目前唯一還擋住「點選連線」的差異

`cwjoin` 的錯誤碼演進本身就是對照表：

| 階段 | 錯誤 | 對應差異 |
|---|---|---|
| v221 以前 | `-3903` | `setAssociateRequest` 的 ivar `+0x1a0` 是 null，ASSOCIATE 從沒轉發 |
| v229–v236 | `-3903` | airportd 的 **error 37 `Already associated`**：BSD 介面 link 狀態沒掉（3.1） |
| v240 | `-3903`（log 已變成 `Joining …` 後才失敗） | scan cache 空（4.2／4.3） |
| v242 | `102`（EOPNOTSUPP） | 量到 `Ath10kIoctlFall="type=21 st=66"`：**`ASSOCIATE_RESULT(21)`** 回 error → 這是我們 v230 的過度修正 |
| **v243** | `-3903`，但 airportd 已走到 `setJoinEvent: Joining QCA-TEST` | 21 改成「ioctl 成功 + `result=UNAVAILABLE/SUCCESS`」；剩下的 -3903 對應 4.3（scan cache / request node） |

→ 下一個要量的就是 **`102` 是哪一個 type**：`Ath10kClientTrace` 目前是
`g15 g1 g103 g1 g9 g4 g16 …`，配合 `Ath10kIoctlFall`（未服務而落到 Apple 實作的 type）
就能直接指名道姓，不必再猜。
