# 任務：QCA6174 macOS 驅動最小可測版本

## 背景與實機

- Acer Swift SF314-51
- Qualcomm Atheros QCA6174 `168c:003e` rev 32
- Lite-On subsystem `11ad:0807`
- Linux 驅動 `ath10k_pci`
- firmware `WLAN.RM.4.4.1-00309-`
- PCI path in OpenCore: `PciRoot(0x0)/Pci(0x1C,0x3)/Pci(0x0,0x0)`
- macOS SMBIOS `MacBookPro14,1`
- 目前 EFI 已有一份舊的 `AirportItlwm.kext`，但硬體不是 Intel；不要沿用或假裝它能控制 QCA6174。
- 純 PCI ID spoof 成 AR9380 已研究證明最多 L0/L1，不能掃描/連線；不要把 AirPortAtheros40 plist patch 當成驅動完成。

## 目標

建立一個可開源、可審查、分階段推進的 QCA6174 macOS DriverKit/IOKit kext 專案。本輪交付必須至少完成 **M0：安全匹配與 PCI probe 骨架**，盡可能推進 **M1：PCI BAR/Copy Engine 基礎、韌體檔解析與載入狀態機骨架**。

### M0 驗收

1. Xcode/kext 專案結構完整，有明確 build instructions。
2. IOKit personality 只匹配 `pci168c,3e`；bundle id 不冒充 Apple。
3. `start()` 只做安全 probe：取得 IOPCIDevice、enable memory/bus-master、映射 BAR、讀取只讀 identification/version registers；所有失敗必須 cleanly return false。
4. 不註冊假的 Wi-Fi interface，不宣稱 L2/L3。
5. 有 boot-arg（例如 `-qca6174debug`）控制 log；預設安靜。
6. 有 panic-minimizing safeguards：範圍檢查、resource release、timeout、不得 busy infinite loop。

### M1 驗收（可分段）

1. 依 Linux ath10k 的 QCA6174 hw params/PCI/Copy Engine 實作最小抽象；每一個借用邏輯標來源檔與 commit/license。
2. firmware/board data parser 可在 Linux host 以 unit tests 測，fixture 可用系統 `/lib/firmware/ath10k/QCA6174/hw3.0/`（若存在）。
3. BMI/HTC/WMI/HTT 尚未完成的地方必須明確 `ENOTSUP`/TODO，不得 stub 成成功。
4. 產生 `docs/STATUS.md`，用 L0/L1/L2/L3 說明真正完成到哪。

## 參考上游

- Linux `drivers/net/wireless/ath/ath10k/`（ISC）
- OpenIntelWireless/itlwm（理解 macOS IOKit/Wi-Fi glue，不得盲目複製不相容授權/架構）
- Apple IO80211Family 私有 API 不是本輪 M0 必要條件。

## 硬規則

- 不修改 `/mnt/opencore-efi`、不部署、不重啟任何機器。
- 不 commit、不 push。
- 不杜撰 build/test 成功；Linux 沒有 Xcode/macOS SDK，不能 build kext 就明說。
- 必須至少三輪：實作 → 自審找錯 → 修正 → 重跑 host tests/static checks。
- 所有 host-side parser/register tests 必須實際執行並保存輸出到 `artifacts/verification/`。
- 最後回報：逐檔用途、實際執行的命令與輸出、尚未完成、實機測試前的風險與安裝步驟。
