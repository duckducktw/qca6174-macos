# Join 流程逆向總圖（airportd → 韌體）

這份文件只講「按下 join 之後，請求怎麼一路走到驅動、在哪裡斷」。所有位址、偏移、
呼叫慣例都是在本機（macOS 13.7.8 / Ventura，KC `kernelcache` dump 在 Acer `/tmp/kc2`）
實測或反組譯得到，不是推測。狀態時間點：**kext v188**（commit `c6f2335`）。

---

## 0. 一句話現況

驅動本身已能把 en1 帶到「掃描 → 關聯 → 4-WPA2 握手 → DHCP → 上網」全通（用本專案的
`a11assoc` 直接下 ioctl 時），但**由系統（選單列／System Settings／`networksetup`／CoreWLAN）
發起的 join 會在 airportd 內部以 `-3903` 結束，`ASSOCIATE(20)` 從未送到驅動**。

---

## 1. 完整鏈路（由外到內）

```
networksetup / System Settings / cwjoin
   │  (CoreWLAN.framework：程式碼在 dyld shared cache，磁碟上只剩 Resources)
   ▼  XPC
airportd  (/usr/libexec/airportd, 3.45 MB; x86_64 slice fileoff 0x4000 size 1749680)
   │  本機 log：/var/log/wifi.log（unified log 幾乎為空，只有它可靠）
   ▼  opens IO80211APIUserClient（family 的 user client，不是我們的）
family user client  ── extRunDriverCommand
   ▼  IO80211Controller::apple80211RequestIoctl  (0xffffff8002183e5c)
        call [rax+0x388]   esi=cmd  edx=type  rcx=iface  r8=data
   ▼
driver 入口（三個 virtual + 一個 non-virtual，見 §4）
   ▼
WMI / HTT → 韌體
```

### 1.1 各層的證據

| 層 | 證據 |
|---|---|
| CoreWLAN 有拿到掃描 | `tools/cwjoin.m`：`scan-saw-target=1` |
| airportd 是請求者 | 驅動內抓到 `Ath10kReqWho = "pid=170 name=airportd ret=0xffffff80027118be type=256 len=0"` |
| airportd 有拿到掃描結果 | `Ath10kScanServeTrace = "168:9 168:10 …"`，而 `wifi.log` 的 `<airport[168]>` 證明 **pid 168 就是 airportd** |
| set 曾被拒 | `/var/log/wifi.log`（v184 之前）：`Error Apple80211Set: Operation not supported seqNum 1 Total 2 chg 1 en1` |
| set 現在不被拒 | 同一行在 v184 之後**沒有 Error** 前綴 |
| airportd 開的是 family 的 client | `Ath10kUserClientBefore/After = "IO80211APIUserClient"`、`Ath10kUserClientAttempt = 2`；`ioreg -n IO80211APIUserClient` 顯示 `IOUserClientCreator = "pid 86, configd"`；我們的 `Ath10kVenturaUserClient::externalMethod` 從未被呼叫（`Ath10kUC*` 全部不存在） |
| join 失敗點 | `cwjoin`：`assoc-ok=0`、`domain=com.apple.wifi.apple80211API.error code=-3903`、`userInfo={}`（空，沒有更細的理由） |

---

## 2. family（live `com.apple.iokit.IO80211Family`）

live `__TEXT`：VA `0xffffff8001f35000`，KC 檔內偏移 `31674368`，大小 `1687552`。

### 2.1 請求轉發

`IO80211Controller::apple80211RequestIoctl` @ `0xffffff8002183e5c`：

```
0x0043 mov esi, r13d      ; cmd
0x0046 mov edx, r12d      ; type
0x0049 mov rcx, r15       ; interface
0x004c mov r8,  r14       ; data
0x0067 call qword ptr [rax + 0x388]
```

→ **驅動側的第四個 virtual 槽（+0x388）收到的就是 `(cmd, type, iface, data)`。**
這也是為什麼把「第一個參數當成 1/0 動詞」是錯的（見 §5.1 的 bug）。

### 2.2 ASSOCIATE 的 family 端處理

`setAssociateRequest`（live 版本）@ `0xffffff8001fe2533`（另有 `0xffffff8002034437`、legacy `0xffffff8002179430`）：

```
0x0052 call IOLog
0x00a6 call IO80211Controller::apple80211VirtualRequestIoctl
```

也就是 family 只做記錄＋轉發，**它不建 supplicant**。真正的 4-way supplicant
在 legacy bundle（見 §7）。

### 2.3 `getCurrentNetwork`（type 103）@ `0xffffff8001fde40a`

```
0x0009 cmp dword ptr [rcx + 0x18], 0x48c     ; req_len 必須是 0x48c
0x0028 mov esi, 0xc03069c9                   ; get
0x002d mov edx, 0x67                         ; type 103
0x0032 mov rcx, r10                          ; interface
0x0038 ... call [rax + 0xcb0]                ; → 驅動
0x0058 mov eax, 0x66                         ; 失敗一律回 EOPNOTSUPP
```

→ 回覆佈局 = `version(4) + num_bss(4) + 一個 apple80211_scan_result`（0x48c = 8 + 0x484）。

### 2.4 `setRSN_Conf` / `OFFLOAD_RSN`（type 177）@ `0xffffff8002038e2c`

```
0x0028 cmp dword ptr [rcx + 0x18], 8         ; req_len == 8
0x0031 mov rsi, qword ptr [rcx + 0x20]       ; req_data
0x0049 call IO80211Controller::copyIn
0x005f mov rcx, qword ptr [r14 + 0x118]      ; vif → Skywalk ivars
0x0066 mov dword ptr [rcx + 0x118], eax      ; 存 payload 的上半 dword
0x007c call apple80211VirtualRequestIoctl(0x803069c8, 0xb1, vif, …)
```

### 2.5 `useAppleRSNSupplicant`（live）@ `0xffffff8001fed798`

```
0x0000 test rsi, rsi
0x0003 je   0xffffff8001fed7b4      ; null → false
0x0009 mov  rax, qword ptr [rsi + 0x118]   ; vif → Skywalk ivars
0x0010 mov  al, byte ptr [rax + 0x118]     ; RSN-conf byte
0x0016 and  al, 2
0x0018 shr  al, 1
0x001a ret
```

**實測：我們這顆 vif 的值是 `0x22`（bit1 已設），`Ath10kAppleRSNGate = "already-on"`。**
所以「Apple RSN 沒開」不是問題；問題是 live 這代**根本沒有 supplicant 可開**（§7）。

---

## 3. Apple 驅動的請求型別分派（用來判斷哪些 type 是真型別）

`AirPort_BrcmNIC::apple80211Request` @ `0xffffff80022b06a6`：

```
0x002e cmp esi, 0xc03069c9 ; get cmd
0x003c cmp ebx, 0x803069c8 ; set cmd
0x00a6 lea eax, [r14 - 1]  ; r14 = req_type
0x00aa cmp eax, 0x108
0x00af ja  reject          ; 只接受 1..0x109 (265)
0x00b1 lea rcx, [rip + 0x1556]   ; → 0xffffff80022b1cb4，0x109 項跳表
0x00b8 movsxd rax, dword [rcx + rax*4]
0x00bc add rax, rcx
0x00bf jmp rax
```

- 跳表位址 `0xffffff80022b1cb4`（KC 檔內偏移 `35331252`）；default 分支 `0xffffff80022b1c3a`。
- **115 個型別有實作**。其餘落 default。
- **`0xff / 0x100 / 0x101 / 0x102 / 0x67(103) / 0xb1(177)` 全部指向 default** ——
  所以 255/256 這種數字不是協定型別，Apple 自己也不認。
- 實作的 1..199：`1-20, 22-46, 48-51, 53-57, 59-62, 65, 66, 69, 73, 74, 76-81, 84-87,
  90, 92-95, 107-109, 111-113, 156, 169-175, 179-181, 183, 185, 187-189, 191-196`；
  另有 `216, 221, 222, 230, 235, 237, 238, 254, 265`。

本專案驅動有專門 case 的約 45 個；其餘在 v184 之後一律「get 回 version+0、set no-op」，
所以型別不再是阻塞點（`Ath10kUnknownReq` 只剩資訊用途）。

---

## 4. 驅動側的四個入口（我們能接的與接不到的）

| 入口 | 簽章（標頭宣告） | 可覆寫 | 追蹤 id |
|---|---|---|---|
| `apple80211VirtualRequest` | `virtual SInt32 (UInt cmd, int type, IO80211VirtualInterface *, void *)` | ✅ | 2 |
| `apple80211_ioctl` | `virtual SInt32 (IO80211SkywalkInterface *, unsigned long cmd, void *, void *)` | ✅ | 1 |
| `apple80211SkywalkRequest` | `virtual SInt32 (UInt cmd, int type, IO80211SkywalkInterface *, void *)` | ✅ | 2 |
| `apple80211VirtualRequestIoctl` | `SInt32 (unsigned int cmd, int type, IO80211VirtualInterface *, void *)`（**非 virtual**） | ❌ | — |

標頭出處：`MacKernelSDK/Headers/IOKit/80211/IO80211Controller.h:282`、
`vendor/itlwm-v1/Airport/IO80211Controller.h:297`（兩份都沒有 `virtual`）。

因為 family 的 `apple80211RequestIoctl` 是用 **vtable +0x388** 呼叫，而
`apple80211VirtualRequestIoctl` 不是 virtual，所以 +0x388 實際上對應的是
`apple80211VirtualRequest`（我們有覆寫、也修好了 get/set 判斷）。
**但 airportd 的 associate 並沒有從這裡進來**——`Ath10kVirtualReqType` 停在 12
（CARD_CAPABILITIES），之後就沒有請求。

---

## 5. 這一輪修掉的（依版本）

| 版本 | 內容 |
|---|---|
| v165 | `initIfnetParams()` 的 `params->type` 由 71 改成 6 → IPConfiguration 的 ARP client 接受，DHCP 通（§6.25） |
| v168 | SSID/BSSID/RSSI getter 回真實值；`venturaPower(false)` 真的斷線 |
| v169 | `CURRENT_NETWORK(103)` 依 live `getCurrentNetwork` 的佈局實作 |
| v171 | `AUTH_TYPE`/`OP_MODE` 回真值（原本「回 success 但不寫」） |
| v172 | `VIRTUAL_IF_CREATE/DELETE(94/95)` 承認既有 station vif |
| v173–v179 | 入口軌跡 `Ath10kEntryTrace`、`Ath10kIoctlFall`、使用者指標 copyIn/copyOut 路徑 |
| v180–v182 | `Ath10kReqDump`/`ReqPayload`/`ReqWho`；補 `RSN_IE(46)`、`RSN_CONF(77)`、`STA_AUTHORIZE(74)`、`CIPHER_KEY(3)` |
| v184 | 未知型別一律服務（get 補 version+0、set no-op）→ `wifi.log` 的 `Error Apple80211Set` 消失 |
| v187 | **`apple80211VirtualRequest` 把 cmd 當 1/0 的 bug 修正**（所有 set 曾被當 get） |
| v188 | CARD_CAPABILITIES 照 Apple 的佈局重寫（`01 ee 73 a0 04 41 01 02 00 00 00`） |

### 5.1 v187 的那個 bug 值得單獨記

```cpp
// before
return venturaRecordRequest(requestType, requestNumber, data);   // requestType 是 cmd 字！
// after
const bool isSet = requestType == 0x803069c8UL || requestType == 1;
return venturaRecordRequest(isSet ? 1 : 0, requestNumber, data);
```

`0xc03069c9` 不等於 1，所以 **set 全被當成 get**：airportd 的 power/SSID/associate
寫入都回 success 卻什麼都沒做。這是「明明看起來都在回 success，系統卻說不能 join」的來源之一。

---

## 6. 已量到、但還沒解釋的現象

1. `Ath10kVirtualReqType = 12`（CARD_CAPABILITIES）是這條 slot 上的最後一個請求。
2. `Ath10kRequestTrace` 的序列長這樣：`g1 g2 g15 g4 g19 g1 g1 g2 g12 g12 g19 g1 g16`
   —— 全是 getter，最後一個是 `RSSI(16)`，**沒有 `s20`（ASSOCIATE）**。
3. `Ath10kReqWho = "pid=170 name=airportd … type=256 len=0"`：airportd 確實送過一個
   `type` 讀起來是 256 的 set（`Ath10kReqDump` 顯示 `if_name="en1"`、`type=0x100`、`len=0`）。
   但 256 不在 Apple 的 0x109 表內 → 極可能是**區塊已被重用／釋放**，或該批不是走
   48-byte `apple80211req`。這一條還沒收斂。
4. `cwjoin` 之後 `[iface ssid]` 回 `"\u0001"`、`rssi=0`：`SSID` getter 在未關聯時回空，
   但 client 端把它顯示成 1 個 byte 的值，值得再對照。

---

## 7. 為什麼「Apple RSN」不是解：符號歸屬

KC 符號表（`n_type & N_EXT`，逐 fileset 條目）實查：

| 符號 | 提供者 |
|---|---|
| `RSNSupplicant::init()` `0xffffff80021bfa74` | **`com.apple.iokit.IO80211FamilyLegacy`** |
| `RSNSupplicant::initWithInterfaceIEAndMode(...)` `0xffffff80021c030e` | 同上 |
| `RSNSupplicant::process1of4/process3of4` | 同上 |
| `RSNSupplicant::installPTK/installGTK/cachePMKSA` | 同上 |
| `_ccec_cp_256` `0xffffff800370ea68`、`_ccec_mult_blinded`、`_cczp_mul/inv/add/sub`、`_ccrng` | `com.apple.kec.corecrypto` |
| `_ccsha1_di` `0xffffff80005fbd30`、`_ccsha256_di` `0xffffff8000585400` | `com.apple.kernel` |
| `_cczp_prime`、`_cczp_n` | **不存在**（corecrypto 標頭的 inline／macro） |

結論：注入 kext 只能綁 live（OpenCore 只查 `OSBundleLibraries`，kernel 排第一），
**核心內 supplicant 拿不到**。社群「開 Apple RSN 就 panic」= 把 legacy 才有的東西
從 live 形狀的介面叫下去，虛擬位落到隔壁槽（同 §6.15.5 的死法）。

企業驗證（802.1X）則走完全不同的路：`EAPOLController`（212 KB，
`/System/Library/SystemConfiguration/EAPOLController.bundle/Contents/MacOS/EAPOLController`）
→ `eapolclient` → 在介面上開 **EAPOL socket** 收送 `0x888e`，
由 `EAP8021X.framework` 完成 EAP。這條不需要 legacy bundle。

---

## 8. 未解的核心問題與兩個候選方案

**問題**：airportd 拿得到掃描結果、set 也不再被拒、caps 也照 Apple 佈局給了，
但它**在送出 ASSOCIATE 之前就結束**（`-3903`，userInfo 空）。
唯一還沒被解釋的是「family 的 user client 到底往哪裡走」：

- `IO80211APIUserClient` 是 family 建的（`IOUserClientCreator = pid 86, configd`），
  我們的 `Ath10kVenturaUserClient` 沒被用到（`Ath10kUC*` 全空）。
- family 的 `extRunDriverCommand` 最後呼叫 `apple80211VirtualRequestIoctl`，
  而它不是 virtual → 我們覆寫不到。

**方案 A（推薦）**：攔 `newUserClient`，讓 airportd 拿到**我們的** user client，
在 `externalMethod` selector 0 解析 `apple80211req`（≥0x3c30 bytes）並轉給
`venturaRecordRequest`，把回覆寫回 structureOutput。
現有 `Ath10kVenturaUserClient::externalMethod` 已經有 selector 0/1 的骨架（目前只回
零緩衝＝「命令成功但結果全 0」），改成真正轉發即可。

**方案 B**：反 family 的 `extRunDriverCommand`（family `__TEXT` 內，需從
`IO80211APIUserClient::externalMethod` 追進去），找出它在什麼條件下才往下呼叫
driver 的 assoc；再補那個條件。

---

## 9. 量測工具與麵包屑一覽

| 工具／屬性 | 用途 |
|---|---|
| `tools/ifnet-type.c` | 印 `if_data.ifi_type`（必須是 6） |
| `tools/apple80211-assoc.c`（`a11assoc`） | 直接下 ASSOCIATE（驅動側驗證用；密碼走 stdin） |
| `tools/apple80211-scan.c`、`tools/cwscan.m` | 掃描（後者走 CoreWLAN） |
| `tools/cwjoin.m` | **走 CoreWLAN 發 join 並印完整 NSError**（密碼走 stdin） |
| `tools/sc-interfaces.c`、`tools/io-match.c` | SC 介面清單、IOPropertyMatch 驗證 |
| `Ath10kRequestTrace` | 最近 16 個 `(get/set, type, status)` |
| `Ath10kEntryTrace` | 最近 12 個 `(entry, cmd, type, len)`，含沒進 dispatcher 的 |
| `Ath10kArgDump` | 每個到達 ioctl slot 的請求前 6 個 dword（`316e65` = "en1"，type 在 +0x10） |
| `Ath10kIoctlFall` | 掉回 Apple 實作的型別、回傳碼、entry、cmd |
| `Ath10kReqDump` / `Ath10kReqPayload` / `Ath10kReqWho` | 異常型別的原始 block／payload／呼叫者 pid+name |
| `Ath10kCaps` / `Ath10kVirtualReqType` / `Ath10kUnknownReq` | 能力表實值、virtual slot 最後型別、未專門處理的型別 |
| `Ath10kAppleRSNGate` / `Ath10kRSNConfBefore` | Apple RSN 開關（實測 already-on） |

常用指令：

```sh
# guest 內
ioreg -l -w0 | grep -a Ath10k | cut -c1-200
grep -a Apple80211Set /var/log/wifi.log | tail -3
networksetup -setairportnetwork en1 <SSID> <pw>     # 只印一行錯誤
printf '%s\n' "$PW" | ./cwjoin <SSID> en1           # 印完整 NSError
```

---

## 10. 陷阱（會浪費整輪的那種）

1. **stale object**：只改 `.hpp`/註解時 `make` 會用舊 obj，產物 sha256 一模一樣。
   改完一定 `rm -rf build/ventura` 再編（v175–v177 就踩過）。
2. **`Ath10kEntryTrace` 的指標被截成 32-bit**（`%u`），看起來像使用者指標其實是核心指標；
   要判斷指標種類請看 `Ath10kArgDump` 或 `Ath10kReqDump`。
3. **`networksetup` 只印一行**，沒有 userInfo；要細節用 `cwjoin`。
4. **unified log 幾乎是空的**，airportd 只寫 `/var/log/wifi.log`。
5. **密碼永不進 argv／輸出**：`a11assoc`、`cwjoin` 都從 stdin 讀。
6. **`apple80211VirtualRequest` 的第一個參數是 cmd 字**，不是 1/0（v187 的教訓）。
7. **`apple80211VirtualRequestIoctl` 不是 virtual**，`override` 會編譯失敗；別再試。

---

## 11. 第三十三輪追加：`Apple80211Associate2` 才是 airportd 用的入口，而且它自己送 type 20

`nm -u /usr/libexec/airportd` 匯入的 Apple80211 符號共 33 個，其中包含 **`_Apple80211Associate2`**。
所以 §8 的「ASSOCIATE 從來沒送出」要修正成：**airportd 走 Associate2，不走 `Apple80211Set(20)`**。

### 11.1 Associate2 的實際行為（反組譯＋實測）

- 映像位移 `0x8b3a`（base 由 `dlsym("Apple80211Set")` 反推可得）。
- 在堆疊建 **0x250-byte `apple80211_assoc_data`**，欄位由傳入的 CFDictionary 填。
- 失敗善後呼叫 **`Apple80211Set(ref, 0x16 /*22 = DISASSOCIATE*/, 0, 0, 0)`** —— 這就是驅動 trace 裡的 `s22`。
- 送出關聯用的是自己的 ioctl：

```
apple80211req { char if_name[16]; int type=20; int val; u32 len=0x250; void *data; }
ioctl(fd, 0x802869c8, &req)
```

`apple80211_ref` 佈局（實測）：`+0x00 = fd`、`+0x04 = "en1"`、`+0x50 = 旗標字`（bind 前 1、bind 後 `0xa073ee6f`，bit 22 = `0x400000` 已設）。

### 11.2 這條通道會到驅動（決定性）

`tools/a2push.c`（open → bind en1 → 用 `ref+0x00` 的 fd 下 `type=20`、WPA2-PSK）：

```
ioctl(set 20) rc=-1 errno=5 (EIO)
Ath10kAssocRaw   = "ver=1 len=8 upper=0008 lower=0000 keylen=12 rsnlen=0"
Ath10kAssocPhase = "requested bssid=f0:68:e3:2d:40:22"
```

驅動收到且開始關聯，但 userspace 拿到 **EIO(5)**。→ **kernel 入口不是牆**，牆在「assoc 完成／結果」的回報，
即 §8 的問題要改寫成「family 送出 assoc 之後等不到完成」，不是「assoc 送不出去」。

### 11.3 Associate2 的字典 schema（實測）

| 鍵 | 型別 | 備註 |
|---|---|---|
| `SSID` | **NSData（raw SSID bytes）** | 給 NSString 會 `-[__NSCFString getBytes:range:]` 爆掉 |
| `SSID_STR`,`BSSID`,`CHANNEL`,`RSN_IE`,`passphrase`,`password`,`security` | — | 單獨給都回 `-3900`（沒有 SSID 就不動作） |
| 全部一起給 | — | `-[__NSCFData objectForKey:]`：有一個鍵的值必須是**字典**（未定位） |

只給 `SSID`：10.2 秒後 `-3905 (0xfffff0bf)`、errno=60(ETIMEDOUT)，並把既有連線打成 `failed`。

### 11.4 重放工具（已進 repo `tools/`）

| 工具 | 用途 |
|---|---|
| `a2fd.c` | 印 `apple80211_ref` 佈局、找出 fd、用 GET 驗證通道 |
| `a2push.c` | 用 ref 的 fd 直接推 `ASSOCIATE(20)`（PSK 走 stdin） |
| `a2join.c` | 直接呼叫 `Apple80211Associate2`（字典變體，fork 隔離） |
| `a2schema.c` | 逐鍵探 Associate2 的字典型別 |
| `dump_a2.c` / `imgdump.c` | 把 framework 的函式區段／`__TEXT` dump 出來離線反組譯 |

### 11.5 下一輪的目標（改寫）

量「family 在 assoc 之後等什麼」：`setAssociateRequest` → 之後的
`ASSOCIATE_RESULT(21)`／`ASSOCIATION_STATUS(22)`／link-changed 事件路徑，
找出它在哪一步判定失敗（EIO／-3905），而不是再碰 userspace 白名單。
