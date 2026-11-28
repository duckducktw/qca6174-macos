# 交接：蘋果「放行 join」的條件（入口文件）

> 只談一件事：**要讓 Apple 的 stack 真的放行一次「點選連線」，每一層各自要求什麼**，
> 以及我們現在準確卡在哪一格。每條都附反組譯位址或 ioreg／log 證據。
> 逐輪歷史在 `docs/HANDOFF.md`（§6.x）；工具與流程在 `docs/reverse-engineering-playbook.md`。
> 最後大改：2026-11-28（v265 之後）。

---

## 0. 先讀這段：現在的結論

**驅動本身是好的。** 開機後 `connected-wpa2`、`ifconfig en1` 有 IP、`ping 1.1.1.1` 通，
`System Settings` 也把 en1 顯示成 Wi-Fi。壞掉的只有「使用者按連線」這條路。

**卡點只剩一個條件，而且是 kernel 與 userspace 的交界：**

> airportd 的 `IO80211APIUserClient` 綁在哪一個 skywalk 介面上。
> 那一顆介面決定 `getCardCapabilities`（type 12 handler）會不會去問驅動，
> 也就決定 `ref+0x50` 那 13 bytes 有沒有「可關聯」的 bit（`ref+0x51 & 0x40`）。

它現在綁到的是 Apple `start()` 早期建立的那顆 v1 介面，所以 type 12 從不抵達驅動、
`ref+0x50` 永遠停在 `01 00`，`Apple80211Associate2` 在 `+2209` 就回 `-3903`，
**ASSOCIATE 從未送出**（驅動 `Ath10kReqLog` 只有 t1/t9/t11）。

**三次「看起來有希望」的路線都已被實測否證**（詳見 §2）：
能力位元表、Apple supplicant、`enableVirtualInterface`；外加服務排序與自行簽 entitlement。

---

## 1. 四層與量測現況

| 層 | 條件 | 現況 | 證據 |
|---|---|---|---|
| 1 | airportd／CoreWLAN 願不願意發 | ⚠️ **卡在這裡**（不是 getter 回錯，是它判定介面不具備 join 能力） | `Ath10kClientTrace` 有 `s10`（Set SSID）後即停；`Ath10kReqLog` 無 t20 |
| 2 | `Apple80211Associate2` 願不願意送 | ❌ 條件 A 不成立 | gate 在 `+2209 testb $0x40,0x51(%r12)` → `+2221 movl $0xfffff0c1`（-3903） |
| 3 | IO80211Family 願不願意轉發 | ✅ 已對齊（ivar `+0x1a0`=vif、role 6、5 參數形狀、RSN gate） | `Ath10kCtrlPrimaryAfter=set`、`Ath10kAssocWho=pid=0` |
| 4 | 驅動回報是否合格 | ✅ 除完成延遲（9–11s vs 10.2s 窗）外全部對齊 | `Ath10kLinkEvent`、`ASSOCIATE_RESULT` 實作 |

**已 superseded 的舊結論**（舊版本文寫過、現在不成立，別再照抄）：

- ~~「`ref+0x50 = 0xa073ee6f`，bit 0x40 是 1」~~ → 實測 bind 後是 `01 00 00 00`，bit 0x40 = **0**。
  （舊值應是別台／舊版 personality 的殘留，別再引用。）
- ~~「卡第 1 層是因為 airportd 只做 getter、連 Set(SSID) 都不送」~~ → 後來 trace 明確看到 `s10:187:0`，
  Set(SSID) 有送，停的是之後的 Associate2。
- ~~「`useAppleRSNSupplicant` 必須回 false」~~ → 依上游改成 true 後行為**不變**（v261 實測）；
  該值不是 gate。`Ath10kAppleRSNGate=already-on` 是 publish 階段舊程式碼寫的。
- ~~「能力位表放錯 byte 是主因」~~ → 換成 AirportItlwm 逐 byte 權威表後，框架確實鏡像到
  `ref+0x54..`，但 `ref+0x51` 不動、`cwjoin` 仍 -3903（v259）。

---

## 2. 已經封閉的路（不要再花回合）

| 假設 | 怎麼死的 |
|---|---|
| 補某個 getter 就會填 bit | `tools/a2probe2.c` 逐型別 fork 掃完：12/43/44/9/27/97 成功但不動它；13/15/21/50/96 回 -3900；1/2/4/14/103 在框架內崩 |
| bit 由 `or imm` 寫入 | 掃 IO80211 `__TEXT` 的 `80 4? 51 40` 等 pattern：0 命中 |
| 硬改 `ref[0x51] |= 0x40` | 直接 `NSException`；它是被管理的值 |
| 自行簽 `com.apple.wlan.authentication` 去觀測 | ad-hoc 簽章一執行就被 SIGKILL（rc=137）；`amfi_get_out_of_my_way=1` 在 SIP 未全關時無效 |
| `enableVirtualInterface` 覆寫 | 加斷點後整個開機＋cwjoin 期間**從未被呼叫** |
| 網路服務排序／主要介面 | Wi-Fi 移到第 1 位、重啟 airportd：`ref+0x51` 仍 0、仍 -3903 |
| 補 IOC 覆蓋率 | 我們 46 個 vs AirportItlwm 40 個，缺的是 AWDL/P2P/HT/VHT/IE 等與 join 無關 |
| 自建 live `IO80211InfraInterface` 一次全開 | kext **完全載不起來**（`kmutil` 0、無 en1、無 log）。要試只能分步開 |

**工具層的教訓（會浪費整輪的那種）**：
`Makefile` 的 `VENTURA_OBJECT` 曾漏掉 `Ath10kInfraInterface.o`，於是對該檔的任何編輯都不進 binary，
`make` 照樣回 0、腳本照樣印 `Built and symbol-checked`。**判準是 sha256 與 `nm -m | grep -c <Class>`**，
不是那行訊息。已修（v265 sha256 由 `0ef3f3dd…` 變 `d27bbdfd…`、符號 1 → 21）。

---

## 3. 三條還開著的路（含成本／風險／能證明什麼）

### A. 由使用者手動 attach airportd（成本最低、風險 0）
需要一個 GUI 終端機（互動 session）。指令由我給、使用者貼：
`sudo lldb -p $(pgrep airportd)`，在 `Apple80211BindToInterfaceWithService` 下斷點，
抓 `ref` 後 `watchpoint set -w write -- *(unsigned char*)($ref+0x51)`。
- 能證明：那一 bit 是誰寫的、airportd 的 client 綁到哪顆介面、13 bytes 實際內容。
- 風險：只讀不寫，attach 失敗也只是沒有結果。

### B. 全關 SIP／AMFI 後改 framework 或注入（成本中、風險中）
`nvram` 改不動（OpenCore 每次用 `config.plist` 重寫），要改 ESP 的
`NVRAM/Add/7C436110-…/boot-args` ＋ `csr-active-config`。
- 能做的事：patch gate、注入 interpose，直接看/繞過判定。
- 代價：之後每一輪都在非標準環境量測，結論要打折；且要確認不會卡在 SIP 造成的開機問題。

### C. 把「驅動自帶 join」當成品（成本 0）
現況已經：開機自動連、SC 顯示 Wi-Fi、可上網。UI 點選連線失敗。
- 若目標是「這台 hackintosh 能用 Wi-Fi」，這條已經達成；若目標是「行為與真機一致」，則不算。

---

## 4. Runbook（精確指令）

```bash
# 主機（Acer 100.64.0.152）與 VM 前置：先查現狀，別重跑已在的東西
ssh 100.64.0.152 'basename $(readlink /sys/bus/pci/devices/0000:02:00.0/driver); ls /dev/mapper/ | grep ventura; ls /dev/vfio/; pgrep -c qemu-system'
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/dm_map.sh'      # 期待 DM OK
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/vfio_bind.sh'   # 可能數分鐘
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/boot_vm.sh'

# 建置／部署（NN 依序遞增）
bash ~/.hermes/cache/scratch/build_v.sh
ssh 100.64.0.152 'bash ~/qca-tools/pull_v.sh NN'                    # 會印 sha256
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/deploy.sh /tmp/vkNN'
# 部署前先確認 sha256 與上一個版本不同，否則就是沒新產物

# 回滾（kext 載不起來、沒有 en1 時第一件事）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/deploy.sh /tmp/vk262'

# guest 內指令（寫成檔案再餵，避免巢狀引號）
ssh 100.64.0.152 "ssh -p 2223 user@127.0.0.1 'bash -s'" < script.sh

# 帶真密碼量測（PSK 不進 argv、不印內容）
PSK=$(grep -o 'ATH10K_AUTOJOIN_PSK[^"]*"[^"]*"' src/kext/ventura/Ath10kAutoJoin.inc | sed 's/.*"\([^"]*\)"$/\1/')
printf '%s\n' "$PSK" | ssh 100.64.0.152 "ssh -p 2223 user@127.0.0.1 'cat > /tmp/psk && chmod 600 /tmp/psk'"
# guest 內：cd ~/qca/tools && clang … -o /tmp/cwjoin cwjoin.m && /tmp/cwjoin QCA-TEST en1 < /tmp/psk

# boot-args 只能改 OpenCore plist（工具只印「加了哪些／共幾個 token」）
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/with_esp.sh set_bootarg.py <token>'
```

**驗收節奏**：部署後等 VM 開完、`en1` 出現再起算 60 秒，才讀 `Ath10kConnectionState`
（剛點完 join 失敗會是 `disabled`，看門狗約 60 秒內回到 `connected-wpa2`）。

---

## 5. 陷阱（照抄會壞）

1. **不要再用 `nvram boot-args=…`**：OpenCore 每次開機重寫，重開後 `grep -c` 一定是 0。
2. **不要引用 legacy-only 符號**（如 `IO80211Interface::setLinkState`）：kext 會靜默不載入。
3. **不要一次全開自建 skywalk 介面**：會讓整個 kext 載不起來，先 `new`→role/id→`attach`→`attachInterface` 分步。
4. **不要用 `ssh -tt` 餵 sudo 密碼**：TTY 會回顯，密碼會進工具輸出。sudo 時間戳是 per-tty，
   要在 pty 內用 `sudo -S`，不能先 `sudo -v` 再 `sudo -n`。
5. **不要貼 `nvram boot-args` 全文**：裡面有 `ath10kjoin=<SSID>:<PSK>`。
6. **不要相信 `Built and symbol-checked`**：改 `Makefile` 物件清單或新增 `.cpp` 後，比對 sha256 與 `nm`。
7. **`ref+0x50` 不是能力位**，是 driver flags；能力鏡像是 `ref+0x54+i`。兩者不要混。

---

## 6. 建議技能

- `qca6174-native-wifi`（本專案所有既有結論與工具）
- `handoff`（下一次交接的格式）
- `credential-hygiene`（處理 `~/sudo.pwd`、PSK 時）
- `systematic-debugging`（若要重開一條假設鏈，四階段流程避免又變成猜）
