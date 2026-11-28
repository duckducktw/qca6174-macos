# QCA6174 在 macOS 13.7 (Ventura) 的原生 Wi-Fi — 交接入口

**這是什麼**：讓 QCA6174（Acer Swift SF314-51 內建網卡）在 macOS 13.7 用原生 kext 上網，
不靠 HeliPort 之類的 userspace app。驅動是自己寫的 IOKit kext（`Ath10kMac.kext`），
透過 `IO80211Interface` + `apple80211` IOC 與 airportd 溝通。
VM 跑在 Acer `100.64.0.152`（QEMU/KVM + VFIO passthrough），guest 是 macOS 13.7。

**誠實狀態**：**可用但未完全打通。** 上網、掃描、點選連線、斷電重連都能用，但連線是
**驅動內部的 watchdog 完成關聯**（`Ath10kAssocWho=kernel_task`），不是 airportd 自己跑 4-way。
因此 System Settings 的 `Transmit Rate` / `MCS Index` 顯示 0，AWDL/AirDrop/漫遊/PMF 都沒有。

- Repo：`~/Data/Dev/qca6174-native`（GitHub `duckducktw/qca6174-macos`），分支 `ventura-io80211`
- HEAD：`1bf98f1` — **已推上 origin**（local == `origin/ventura-io80211`）
- 目前部署：**v293**，`Ath10kMac` sha256 `c5308efd02ac58f2d15f3c11e03bf2260dee3cdca402067012a2226377ce96ca`
- 逐輪實驗紀錄：`docs/HANDOFF.md` §6.x（本檔只寫現況與規則）

---

## 1. 目前實測結果（每個數字都註明來源）

| 項目 | 值 | 來源／條件 |
|---|---|---|
| 連線狀態 | `Ath10kConnectionState="connected-wpa2"`，SSID `QCA-TEST` | `ioreg -l -w0 -c Ath10kPCIDevice` |
| IP / 連通 | ping 1.1.1.1 4/4，avg 7.0 ms，0 % loss | guest 內 `ping` |
| 吞吐 | **84–88 Mbps**（10.53–10.96 MB/s） | guest 內 `curl` 抓 Cloudflare 20–25 MB，多次取高值；同一條線也會掉到 45–51 Mbps，波動大 |
| 發射功率 | **20 dBm**（`Ath10kTxPowerApplied="halfDbm=40 (20 dBm)"`） | v293 起真的送進韌體 |
| WMI runtime | `Ath10kWMIRuntimeStatus="active-msi-worker"` | 同上 |
| 協商标籤 | `Ath10kConnectionPHY="HT20-1SS"` | **低估**：88 Mbps 已超 72 Mbps 上限，標籤與 `APPLE80211_IOC_RATE` 都不準 |
| UI 速率 | `Transmit Rate: 0`、`MCS Index: 0` | `system_profiler SPAirPortDataType`；原因見 §4.1 |

舊的「效能約 1/12（72 Mbps vs VHT80 867 Mbps）」估計**已作廢**：72 Mbps 是我們自己的標籤，不是真實鏈路。

---

## 2. 不能弄壞的規則（每條附一句為什麼）

1. **`RegulatoryPolicy::ceilingHalfDbm` 必須 ≤20（半 dBm）。** 它同時是 `buildWMIRadio()` 的韌體 radio
   功率參數，`Ath10kWMI.hpp:319` 驗證 `mask<=20`；改大會讓 `buildWMIRadio` 回 false、runtime 停在
   `radio-build`、WMI 永不 active（v289–v292 就是這樣死掉的）。要調高我們自己的 assoc 功率，改
   `powerCeilingHalfDbm`。
2. **thread call 不可以從自己的 callback 裡重排自己。** xnu 會拒絕 `thread_call_enter_delayed` 自我重入，
   watchdog 只會跑一次（`Ath10kAutoJoinRuns=1`）；`THREAD_CALL_OPTIONS_ONCE` 改成 0 也救不了。
   retry 要走每秒在跑的 WMI runtime timer（`serviceRuntime()` 內每 10 s 踢一次）。
3. **`Ath10kWmiLive` / `Ath10kRecoveryPhase` 是舊字串，會騙人。** `endWMIRuntime()` 只改 `_wmiLive.active`，
   不更新屬性。判斷 runtime 死活只看 `Ath10kWMIRuntimeStatus`（`active-msi-worker` / `active-polling` = 活）。
4. **13-byte `APPLE80211_IOC_CARD_CAPABILITIES` 要回短格式（不帶 `version`）。** 帶了 capability byte 會位移，
   airportd 的 gate（`Apple80211Associate2` 讀 `ref+0x51` bit `0x40`）就讀不到，ASSOCIATE 直接回 `-3903`。
5. **事件投遞前必須先 `createEventPipe`**（在 `Ath10kVenturaInterface::newUserClient`），否則
   `postMessageIOUC` 回 `0xe00002c7`。
6. **不要呼叫 `Ath10kInfraInterface::createAndAttach`。** 它會留下未定義符號，kext 整個載不起來
   （v265–v270 全部失敗，`kmutil showloaded | grep -c Ath10k` = 0）。
7. **預設保留 hand-built publish；authoritative publish 只能 opt-in（`ath10kpub=1`）。** 實測框架不會替
   hand-built 介面發 BSD unit，硬走會讓 `en1` 消失。
8. **不要用 ad-hoc 簽章去拿 `com.apple.wlan.authentication`。** 執行即 SIGKILL rc=137，
   `amfi_get_out_of_my_way=1` 在 SIP 沒全關時無效。

---

## 3. 陷阱（照做，不要重新踩）

- **guest `/tmp` 每次重開清空**：`cwjoin`、`a2*`、`iocs` 等工具要重推；`/tmp/psk` 要重新餵。
- **PSK 只用 stdin 管線傳**，不要寫進命令列或貼進聊天（本檔不記任何憑證值）。
- **部署後至少等 150 秒**再讀屬性，否則會讀到開機中途的暫態（曾誤判成 `disabled` / `legacy`）。
- **`s.phyReason` 會被後面的 `ht-common-capabilities` 覆蓋**，想診斷 40 MHz 閘門要另開屬性。
- **改 build 後先 `kmutil showloaded | grep Ath10k` 確認載入**，再解讀行為。
- **吞吐量單次不可信**（同一條線 45–88 Mbps 都出現過），要取多次。

---

## 4. 還沒解決的（下一個 session 的主線）

### 4.1 airportd 自己那條 4-way 關聯（最大的洞）
- 現況：join 由 watchdog 完成，`Ath10kAssocWho=kernel_task`；airportd 從未自己完成關聯。
- 已查：ad-hoc entitlement 路封死；`Apple80211Associate2` 的 gate 在 `ref+0x51` bit `0x40`；
  airportd 的 user client 綁在 Apple `start()` 早期建的介面上，不是我們 role 6 station vif。
- **要回答的問題**：能不能把 airportd 的 user client 導到我們的 vif 上（kernel 側改綁定）？
- **驗收**：`Ath10kAssocWho` 不再是 `kernel_task`，且 `Transmit Rate` / `MCS Index` 非 0。

### 4.2 PHY / 速率回報低估
- 現況：標籤 `HT20-1SS`、`Ath10kRateIoc=65`，但實測 84–88 Mbps 已超過 72 Mbps 上限。
- **要回答的問題**：真實協商是 40 MHz 還是 2SS？哪個閘門（`apHT&2` / `a.htOp[1]&4` / `channelDefinitionAllowed`）擋住？
- **驗收**：`Ath10kConnectionPHY` 與 `APPLE80211_IOC_RATE` 的值和實測吞吐一致（誤差 <20 %）。

### 4.3 進階功能（依價值排序）
- 5 GHz / VHT80（需 hostapd 開 5 GHz AP；Acer 主機沒有 Wi-Fi，要外接 AP 或改用支援 5 GHz 的測試 AP）
- 漫遊（`ROAM_PROFILE` 已接受請求）、PMF、AWDL/AirDrop

---

## 5. 檔案地圖與執行指令

| 路徑 | 用途 |
|---|---|
| `src/kext/Ath10kPCIDevice.cpp` | 裝置生命週期、`serviceRuntime()`（每秒 tick，含 join retry 踢點）、`radio-build` 迴圈 |
| `src/kext/ventura/Ath10kVentura.cpp` | IOC dispatch、caps、掃描游標、事件、watchdog（`runVenturaAutoJoin`）、TX 功率夾鉗 |
| `src/kext/ventura/Ath10kVenturaInterface.cpp` | vif 介面、`newUserClient` 內 `createEventPipe` |
| `src/kext/Ath10kRegulatory.hpp` | `ceilingHalfDbm`（韌體參數，≤20）與 `powerCeilingHalfDbm`（我們的夾鉗） |
| `src/kext/Ath10kWMI.hpp` | `buildWMIRadio` / `buildWMIRegdomain` TLV 與驗證 |
| `src/kext/Ath10kNetwork.cpp` | `networkLink()`、速率計算、`Ath10kChannelPolicy` |
| `docs/HANDOFF.md` | 逐輪實驗紀錄 §6.x（沿革） |
| `~/qca-tools/{pull_v.sh,deploy.sh,boot_vm.sh,dm_map.sh,vfio_bind.sh}` | 主機側工具（`pull_v.sh NN` 需帶版本號） |

```bash
# 建置（本機 → Acer → guest 內編譯）
bash ~/.hermes/cache/scratch/build_v.sh

# 拉版本 + 部署（一定要分開兩次 ssh，deploy 需要 sudo 密碼走 stdin）
ssh 100.64.0.152 'bash ~/qca-tools/pull_v.sh 293'
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/deploy.sh /tmp/vk293'

# 主機重開後
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/dm_map.sh'
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/vfio_bind.sh'
cat ~/sudo.pwd | ssh 100.64.0.152 'bash ~/qca-tools/boot_vm.sh'

# 讀狀態（guest 內）
ioreg -l -w0 -c Ath10kPCIDevice | grep -E 'Ath10k(WMIRuntimeStatus|ConnectionState|ConnectionPHY|TxPowerApplied|AutoJoinRuns)'
```

---

## 6. 建議技能（下一個 session 先載入）

- `qca6174-native-wifi` — 本專案的完整背景、反組譯筆記、既有陷阱
- `handoff` — 交接待寫時
- `systematic-debugging` — 遇到「改了 A 壞了 B」時先確認欄位是否一物多用
