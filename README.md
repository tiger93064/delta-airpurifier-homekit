# 台達管道風機 HomeKit 控制器

以 ESP32 + HomeSpan 透過 RS485 Modbus 控制台達管道風機（如 **VDB36AEXT-F**），並以 Apple 家庭 App 操作開關、風速與濾網維護。

韌體版本：`2.3.0-AirPurifier`（見 `src/main.cpp`）

---

## 功能一覽

### HomeKit（空氣清淨機）

| 功能 | 說明 |
|------|------|
| 電源開關 | `Active` → Modbus `0x05`（開／關） |
| 風速 | `RotationSpeed` 1–100% → 風機檔位 1–17（`0x07`） |
| 運轉模式 | 開機強制多段調速（`0x06 = 0x04`） |
| 狀態同步 | 約每 10 秒巡檢；HomeKit 為準，硬體偏離時強制校正 |
| 故障指示 | 溫度異常／馬達異常 → `StatusFault` |
| 濾網維護 | 壽命％、需更換提示、重置計時（`0x1C`–`0x1F`） |
| 配件資訊 | 名稱「台達 VDB36 空氣清淨機」、製造商 Delta、型號 VDB36AEXT-F RS485 |

> 無空氣品質感測器；家庭 App 的 AUTO 僅供顯示，實際仍依手動風速。

### WiFi／配對

- HomeSpan 自動設定熱點（首次或需重設 WiFi 時）
- 已存 WiFi 連不上：約 **20 秒**後自動開設定熱點
- 取得無效 IP `0.0.0.0` 後再連上：自動重啟以修復 HomeKit mDNS
- 板載 **BOOT（GPIO0）** 作為 HomeSpan Control 鍵
- 配對碼（解除配對後重新加入）：**466-37-726**
- Web Log：`http://<IP>/log`（最多 50 筆）

### HTTP JSON API（埠 **8080**）

HomeSpan HAP 佔用 80，狀態／管理 API 在 8080。

| 路徑 | 說明 |
|------|------|
| `GET /` 或 `/status` | 風機與 ESP32 狀態 JSON |
| `GET /api` | 端點說明 |
| `GET\|POST /reset/wifi?confirm=1` | 清除 WiFi 並重啟 |
| `GET\|POST /reset/homekit?confirm=1` | 解除 HomeKit 配對 |
| `GET\|POST /reset/homekit-full?confirm=1` | 清除 HomeKit 裝置 ID＋配對並重啟 |
| `GET\|POST /reset/factory?confirm=1` | 出廠重置（WiFi＋HomeKit＋特性值）並重啟 |
| `GET\|POST /reboot?confirm=1` | 僅重啟 ESP32 |

破壞性操作必須帶 `confirm=1`。

`/status` 大致欄位：`modbus_online`、`power`、`speed_*`、`rpm`、系統／錯誤狀態、濾網、ESP32 IP／RSSI／uptime。

### 通訊參數

| 項目 | 值 |
|------|-----|
| 介面 | RS485 Modbus RTU |
| 鮑率 | 9600 8N1 |
| 風機站號 | `0xC8`（200） |
| ESP32 UART | Serial2，`RX=GPIO16`、`TX=GPIO17` |

---

## 硬體組成

| 項目 | 說明 |
|------|------|
| 台達管道風機 | 強電（L/N）＋控制端子（A / B / Vcc 24V / Gnd） |
| 24V→5V 降壓板 | 由風機控制端 24V 取電，USB-A 供電給 ESP32 |
| RS485 模組 | **自動流向**（無需 DE/RE）；輸出端接風機，輸入端接 ESP32 |
| ESP32 DevKit | HomeSpan ＋ Modbus |

腳位定義見 `src/main.cpp`（`USE_RS485_DE_RE=0`，無 DE/RE）。

---

## 配線說明

### 總覽

```
市電 L/N ──► 風機電源（強 + N）


風機控制端子                24V→5V 降壓板              ESP32
────────────              ─────────────              ─────
Vcc (24V) ──────────────► Vn+
Gnd ────────────────────► Vn−
                              │
                              └── USB-A ────────────► USB（供電）


風機控制端子                RS485 模組                  ESP32
────────────              ───────────                ─────
A ──────────────────────► 輸出端 A+
B ──────────────────────► 輸出端 B−
                              輸入端 Vcc ◄─────────── 5V（必須 5V）
                              輸入端 Gnd ◄─────────── GND
                              輸入端 RX  ◄─────────── GPIO16
                              輸入端 TX  ◄─────────── GPIO17
```

### 風機電源（強電）

| 風機端 | 接至 |
|--------|------|
| 強（L） | 市電火線 |
| N | 市電中性線 |

> 強電請依電工規範施工；控制／通訊線與強電分開走線。

### 控制電源（24V → 降壓 → ESP32）

| 風機控制端 | 接至 |
|------------|------|
| Vcc（24V） | 降壓板 **Vn+** |
| Gnd | 降壓板 **Vn−** |

| 降壓板 | 接至 |
|--------|------|
| USB-A 輸出 | ESP32 USB（整板供電） |

> 兩端都誤接到 Vn+ 等於電源短路。Gnd 一定要接降壓板負端。

### RS485：風機 ↔ 模組輸出端

| 風機控制端 | RS485 輸出端 |
|------------|--------------|
| A | A+ |
| B | B− |

> 台達說明書對 A/B 有時標為 A(D−)、B(D+)。通訊不通可對調 A/B 再試。

### RS485：模組輸入端 ↔ ESP32

| RS485 輸入端 | ESP32 | 備註 |
|--------------|-------|------|
| Vcc | **5V** | 必須 5V（勿接 3.3V） |
| Gnd | GND | 與 ESP32 共地 |
| RX | GPIO **16** | `RX_PIN` |
| TX | GPIO **17** | `TX_PIN` |
| （無 DE/RE） | — | 自動流向；`USE_RS485_DE_RE=0` |

若改用需手動收發的模組（例如部分 JSM485ESA）：DE+RE → **GPIO4**，並將 `USE_RS485_DE_RE` 改為 `1`。

### 腳位速查

| 功能 | ESP32 GPIO |
|------|------------|
| RS485 RX | 16 |
| RS485 TX | 17 |
| RS485 DE/RE（選用） | 4（預設未用） |
| BOOT／HomeSpan Control | 0 |

### 注意事項

1. **485 模組邏輯電必須 5V**，接 3.3V 常導致通訊不穩或無回應。
2. 風機控制 Gnd、降壓板 Vn−、ESP32 GND、485 Gnd 應正確共地；勿接錯 24V 正端與 GND。
3. 先確認風機強電與 24V 控制電正常，再插 ESP32 USB。
4. 更完整的配線原稿：`docs/配線說明.md`；風機通訊協定：`docs/台達管道風機控制介面說明書_03.md`。

---

## 建置與燒錄（PlatformIO）

預設環境 `esp32dev` 編譯 HomeKit 主程式（排除 `demo`／`fan_*` 工具）。

```bash
pio run -e esp32dev -t upload
pio device monitor -e esp32dev
```

### 輔助環境（無 HomeKit）

| 環境 | 用途 |
|------|------|
| `fan_off` | 純 RS485 關機 |
| `fan_scan` | Modbus 位址掃描 |
| `fan_mode_test` | 全模式輪詢測試 |

```bash
pio run -e fan_scan -t upload
```

相依函式庫：`HomeSpan`、`ModbusMaster`（見 `platformio.ini`）。

---

## 目錄結構（摘要）

```
├── src/main.cpp          # HomeKit + Modbus 主韌體
├── src/fan_*.cpp         # RS485 除錯工具
├── platformio.ini
├── docs/配線說明.md
├── docs/台達管道風機控制介面說明書_03.md
└── README.md             # 本文件
```
