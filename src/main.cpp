#include "HomeSpan.h"
#include <ModbusMaster.h>
#include <WebServer.h>

// ================= 硬體腳位與參數定義 =================
// 自動流向 485：模組 TX→RX_PIN、模組 RX→TX_PIN（無 DE/RE）
// 若用 JSM485ESA 等需手動 DE/RE，改 USE_RS485_DE_RE=1 並接 DE+RE→DE_RE_PIN
#define RX_PIN 16
#define TX_PIN 17
#define DE_RE_PIN 4
#define USE_RS485_DE_RE 0


const uint8_t DELTA_FAN_ID = 0xC8; // 台達風機預設地址 200 (0xC8)[cite: 8]
const uint32_t POLL_INTERVAL = 10000; // 定時讀取狀態間隔 (10秒)

// VDB36AEXT-F 支援濾網功能
const bool SUPPORT_FILTER_SERVICE = true; 

ModbusMaster node;
// HomeSpan HAP 已佔用 80，JSON API 改用 8080
WebServer httpServer(8080);
bool httpStarted = false;
bool sawInvalidIp = false; // 曾拿到 0.0.0.0：HomeSpan 會在錯誤 IP 上啟動 mDNS，需重啟修復
bool wifiEverConnected = false;
bool apFallbackStarted = false;
const uint32_t WIFI_AP_FALLBACK_MS = 20000; // 有存 WiFi 但連不上：20 秒後開設定熱點

// ================= 全域風機狀態暫存結構 (供 HTTP JSON 讀取) =================
struct FanStateCache {
  bool modbusOnline = false;
  uint16_t power = 0;           // 0x05: 0x00關, 0x01開[cite: 8]
  uint16_t speedMode = 0;       // 0x06: 0x04多段調速, 0x05低風, 0x06高風, 0x07待機[cite: 8]
  uint16_t speedLevel = 1;      // 0x07: 1~17[cite: 8]
  int speedPercent = 0;         // HomeKit 對應百分比
  uint16_t rpm = 0;             // 0x0D: 0~9999 rpm[cite: 8]
  
  uint16_t errorCode = 0;       // 0x10: 異常狀態原始值[cite: 8, 9]
  bool tempError = false;       // Bit5: 溫度異常 (0x20)[cite: 9]
  bool motorError = false;      // Bit7: 馬達異常 (0x80)[cite: 9]

  uint16_t sysStatus = 0;       // 0x13: 系統狀態原始值[cite: 8, 9]
  bool isRunning = false;       // Bit0: 運行狀態 (0x01)[cite: 9]
  bool lowTempProtect = false;  // Bit6: 低溫保護 (0x40)[cite: 9]
  bool halfSpeedSwitch = false; // Bit7: 半速開關 (0x80)[cite: 9]

  uint16_t filterMaxHours = 2000; // 0x1C: 濾網設置時間[cite: 8]
  uint16_t filterNeedChange = 0;  // 0x1D: 濾網更換提示[cite: 8]
  uint16_t filterUsedHours = 0;   // 0x1E: 濾網使用時間[cite: 8]
  float filterLifePercent = 100.0f;
} fanState;

// ================= RS485 收發控制與通訊輔助函式 =================
void preTransmission() {
#if USE_RS485_DE_RE
  digitalWrite(DE_RE_PIN, HIGH);
  delayMicroseconds(500);
#endif
}

void postTransmission() {
  delayMicroseconds(500);  // 自動流向：等 TX 結束後模組切回接收
#if USE_RS485_DE_RE
  digitalWrite(DE_RE_PIN, LOW);
#endif
}

bool writeRegisterWithRetry(uint16_t reg, uint16_t val, int maxRetries = 3) {
  for (int i = 0; i < maxRetries; i++) {
    delay(30);
    uint8_t result = node.writeSingleRegister(reg, val);
    if (result == node.ku8MBSuccess) {
      fanState.modbusOnline = true;
      return true;
    }
    Serial.printf("[Modbus] 寫入 0x%02X 失敗 (錯誤碼: 0x%02X), 重試 %d/%d...\n", reg, result, i + 1, maxRetries);
    delay(100);
  }
  fanState.modbusOnline = false;
  return false;
}

// ================= HTTP JSON Endpoint 處理函式 =================
void handleStatusJson() {
  char jsonBuffer[768];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{\n"
    "  \"modbus_online\": %s,\n"
    "  \"power\": %s,\n"
    "  \"speed_mode\": %u,\n"
    "  \"speed_level\": %u,\n"
    "  \"speed_percent\": %d,\n"
    "  \"rpm\": %u,\n"
    "  \"system_status\": {\n"
    "    \"raw\": %u,\n"
    "    \"running\": %s,\n"
    "    \"low_temp_protection\": %s,\n"
    "    \"half_speed_switch\": %s\n"
    "  },\n"
    "  \"error_status\": {\n"
    "    \"raw\": %u,\n"
    "    \"temp_error\": %s,\n"
    "    \"motor_error\": %s\n"
    "  },\n"
    "  \"filter\": {\n"
    "    \"supported\": %s,\n"
    "    \"max_hours\": %u,\n"
    "    \"used_hours\": %u,\n"
    "    \"life_percent\": %.1f,\n"
    "    \"need_change\": %s\n"
    "  },\n"
    "  \"esp32\": {\n"
    "    \"ip\": \"%s\",\n"
    "    \"wifi_rssi\": %d,\n"
    "    \"uptime_sec\": %lu\n"
    "  }\n"
    "}",
    fanState.modbusOnline ? "true" : "false",
    (fanState.power == 0x01) ? "true" : "false",
    fanState.speedMode,
    fanState.speedLevel,
    fanState.speedPercent,
    fanState.rpm,
    fanState.sysStatus,
    fanState.isRunning ? "true" : "false",
    fanState.lowTempProtect ? "true" : "false",
    fanState.halfSpeedSwitch ? "true" : "false",
    fanState.errorCode,
    fanState.tempError ? "true" : "false",
    fanState.motorError ? "true" : "false",
    SUPPORT_FILTER_SERVICE ? "true" : "false",
    fanState.filterMaxHours,
    fanState.filterUsedHours,
    fanState.filterLifePercent,
    (fanState.filterNeedChange == 0x01) ? "true" : "false",
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    millis() / 1000
  );

  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json; charset=utf-8", jsonBuffer);
}

bool httpConfirmOk() {
  return httpServer.hasArg("confirm") && httpServer.arg("confirm") == "1";
}

void httpSendJson(int code, const char *body) {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(code, "application/json; charset=utf-8", body);
}

void handleApiHelp() {
  httpSendJson(200,
    "{\n"
    "  \"endpoints\": {\n"
    "    \"GET /status\": \"風機與 ESP32 狀態\",\n"
    "    \"GET /api\": \"此說明\",\n"
    "    \"GET|POST /reset/wifi?confirm=1\": \"清除 WiFi 並重啟（之後開 HomeSpan 熱點）\",\n"
    "    \"GET|POST /reset/homekit?confirm=1\": \"解除 HomeKit 配對（可重新加入家庭）\",\n"
    "    \"GET|POST /reset/homekit-full?confirm=1\": \"清除 HomeKit 裝置 ID+配對並重啟\",\n"
    "    \"GET|POST /reset/factory?confirm=1\": \"出廠重置（WiFi+HomeKit+特性值）並重啟\",\n"
    "    \"GET|POST /reboot?confirm=1\": \"僅重啟 ESP32\"\n"
    "  },\n"
    "  \"note\": \"破壞性操作必須帶 confirm=1\"\n"
    "}");
}

void handleResetWifi() {
  if (!httpConfirmOk()) {
    httpSendJson(400, "{\"ok\":false,\"error\":\"請加上 ?confirm=1\"}");
    return;
  }
  WEBLOG("[HTTP] 重置 WiFi，即將重啟");
  httpSendJson(200, "{\"ok\":true,\"action\":\"reset_wifi\",\"msg\":\"已清除 WiFi，正在重啟…\"}");
  delay(300);
  homeSpan.processSerialCommand("X"); // 清 WiFi NVS 並 reboot
}

void handleResetHomeKit() {
  if (!httpConfirmOk()) {
    httpSendJson(400, "{\"ok\":false,\"error\":\"請加上 ?confirm=1\"}");
    return;
  }
  WEBLOG("[HTTP] 解除 HomeKit 配對");
  homeSpan.processSerialCommand("U"); // 清配對、sf=1，不重啟
  httpSendJson(200,
    "{\"ok\":true,\"action\":\"reset_homekit\","
    "\"msg\":\"已解除配對，請用家庭 App 重新加入（碼 466-37-726）\"}");
}

void handleResetHomeKitFull() {
  if (!httpConfirmOk()) {
    httpSendJson(400, "{\"ok\":false,\"error\":\"請加上 ?confirm=1\"}");
    return;
  }
  WEBLOG("[HTTP] 完整清除 HomeKit，即將重啟");
  httpSendJson(200, "{\"ok\":true,\"action\":\"reset_homekit_full\",\"msg\":\"已清除 HomeKit 資料，正在重啟…\"}");
  delay(300);
  homeSpan.processSerialCommand("H"); // 清 hapNVS 並 reboot
}

void handleResetFactory() {
  if (!httpConfirmOk()) {
    httpSendJson(400, "{\"ok\":false,\"error\":\"請加上 ?confirm=1\"}");
    return;
  }
  WEBLOG("[HTTP] 出廠重置，即將重啟");
  httpSendJson(200, "{\"ok\":true,\"action\":\"reset_factory\",\"msg\":\"出廠重置中，正在重啟…\"}");
  delay(300);
  homeSpan.processSerialCommand("F"); // WiFi+HAP+char 並 reboot
}

void handleReboot() {
  if (!httpConfirmOk()) {
    httpSendJson(400, "{\"ok\":false,\"error\":\"請加上 ?confirm=1\"}");
    return;
  }
  WEBLOG("[HTTP] 重啟");
  httpSendJson(200, "{\"ok\":true,\"action\":\"reboot\",\"msg\":\"正在重啟…\"}");
  delay(300);
  homeSpan.processSerialCommand("R");
}

void registerHttpRoutes() {
  httpServer.on("/", HTTP_GET, handleStatusJson);
  httpServer.on("/status", HTTP_GET, handleStatusJson);
  httpServer.on("/api", HTTP_GET, handleApiHelp);

  httpServer.on("/reset/wifi", HTTP_GET, handleResetWifi);
  httpServer.on("/reset/wifi", HTTP_POST, handleResetWifi);
  httpServer.on("/reset/homekit", HTTP_GET, handleResetHomeKit);
  httpServer.on("/reset/homekit", HTTP_POST, handleResetHomeKit);
  httpServer.on("/reset/homekit-full", HTTP_GET, handleResetHomeKitFull);
  httpServer.on("/reset/homekit-full", HTTP_POST, handleResetHomeKitFull);
  httpServer.on("/reset/factory", HTTP_GET, handleResetFactory);
  httpServer.on("/reset/factory", HTTP_POST, handleResetFactory);
  httpServer.on("/reboot", HTTP_GET, handleReboot);
  httpServer.on("/reboot", HTTP_POST, handleReboot);
}

// ================= 濾網維護服務 (Filter Maintenance) =================
struct DeltaFilterMaintenance : Service::FilterMaintenance {
  SpanCharacteristic *filterChange;
  SpanCharacteristic *filterLife;
  SpanCharacteristic *resetFilter;

  DeltaFilterMaintenance() : Service::FilterMaintenance() {
    filterChange = new Characteristic::FilterChangeIndication(0);
    filterLife = new Characteristic::FilterLifeLevel(100);
    resetFilter = new Characteristic::ResetFilterIndication();
  }

  boolean update() override {
    if (resetFilter->updated() && resetFilter->getNewVal() == 1) {
      WEBLOG("[HomeKit] 指令：重置濾網計時");
      if (writeRegisterWithRetry(0x1F, 0x5A)) { // 寫入 0x5A 重置濾網[cite: 8]
        filterChange->setVal(0);
        filterLife->setVal(100);
        fanState.filterNeedChange = 0;
        fanState.filterUsedHours = 0;
        fanState.filterLifePercent = 100.0f;
        WEBLOG("[HomeKit] 濾網重置成功");
      } else {
        WEBLOG("[HomeKit] 濾網重置失敗（Modbus）");
      }
    }
    return true;
  }

  void pollFilterStatus() {
    if (!SUPPORT_FILTER_SERVICE) return;

    delay(30);
    uint8_t result = node.readHoldingRegisters(0x1C, 3); // 讀取 0x1C~0x1E[cite: 8]
    if (result == node.ku8MBSuccess) {
      fanState.filterMaxHours   = node.getResponseBuffer(0);
      fanState.filterNeedChange = node.getResponseBuffer(1);
      fanState.filterUsedHours  = node.getResponseBuffer(2);

      uint8_t changeVal = (fanState.filterNeedChange == 0x01) ? 1 : 0;
      if (filterChange->getVal() != changeVal) {
        filterChange->setVal(changeVal);
      }

      if (fanState.filterMaxHours > 0) {
        float lifePercent = 100.0f - ((float)fanState.filterUsedHours / (float)fanState.filterMaxHours * 100.0f);
        fanState.filterLifePercent = constrain(lifePercent, 0.0f, 100.0f);
        if (abs(filterLife->getVal<float>() - fanState.filterLifePercent) >= 1.0f) {
          filterLife->setVal(fanState.filterLifePercent);
        }
      }
    }
  }
};

// ================= 空氣清淨機主控制服務 (Air Purifier) =================
// CurrentAirPurifierState: 0=INACTIVE, 1=IDLE, 2=PURIFYING
// TargetAirPurifierState:  0=MANUAL, 1=AUTO（無空氣品質感測器，AUTO 仍走手動多段調速）
struct DeltaAirPurifier : Service::AirPurifier {
  SpanCharacteristic *active;
  SpanCharacteristic *currentState;
  SpanCharacteristic *targetState;
  SpanCharacteristic *rotationSpeed;
  SpanCharacteristic *statusFault;

  DeltaFilterMaintenance *filterService;
  uint32_t lastPollTime = 0;
  bool isInitialized = false;

  DeltaAirPurifier(DeltaFilterMaintenance *filter = nullptr) : Service::AirPurifier() {
    filterService = filter;
    active = new Characteristic::Active(0, true);
    currentState = new Characteristic::CurrentAirPurifierState(0);
    targetState = new Characteristic::TargetAirPurifierState(0, true); // MANUAL
    rotationSpeed = new Characteristic::RotationSpeed(50, true);
    statusFault = new Characteristic::StatusFault(0);
  }

  void initOnBoot() {
    writeRegisterWithRetry(0x06, 0x04); // 設定為多段調速[cite: 8, 10]
    fanState.speedMode = 0x04;

    int savedSpeed = (int)rotationSpeed->getVal();
    if (savedSpeed > 0) {
      uint16_t fanLevel = map(savedSpeed, 1, 100, 1, 17); // 1~17 檔位[cite: 8, 9]
      writeRegisterWithRetry(0x07, fanLevel);
      fanState.speedLevel = fanLevel;
      fanState.speedPercent = savedSpeed;
    }

    int savedActive = active->getVal();
    writeRegisterWithRetry(0x05, (savedActive == 1) ? 0x01 : 0x00); // 開關機[cite: 8, 10]
    fanState.power = (savedActive == 1) ? 0x01 : 0x00;
    currentState->setVal(savedActive ? 2 : 0);

    isInitialized = true;
  }

  boolean update() override {
    if (targetState->updated()) {
      int mode = targetState->getNewVal();
      WEBLOG("[HomeKit] 指令：目標模式 %s（Target=%d）",
             mode ? "AUTO" : "MANUAL", mode);
      // 硬體無自動感測調速；AUTO 僅供 Home App 顯示，實際仍用 RotationSpeed
    }

    if (active->updated()) {
      int targetPower = active->getNewVal();
      WEBLOG("[HomeKit] 指令：電源 %s（Active=%d）",
             targetPower ? "開" : "關", targetPower);
      if (targetPower == 1) {
        writeRegisterWithRetry(0x06, 0x04);
        fanState.speedMode = 0x04;
      }

      if (writeRegisterWithRetry(0x05, targetPower ? 0x01 : 0x00)) {
        fanState.power = targetPower ? 0x01 : 0x00;
        currentState->setVal(targetPower ? 2 : 0); // PURIFYING / INACTIVE
        WEBLOG("[HomeKit] 電源指令已寫入 Modbus");
      } else {
        statusFault->setVal(1);
        WEBLOG("[HomeKit] 電源指令失敗（Modbus）");
      }
    }

    if (rotationSpeed->updated()) {
      int targetPercent = (int)rotationSpeed->getNewVal();
      uint16_t fanLevel = (targetPercent > 0) ? map(targetPercent, 1, 100, 1, 17) : 0;
      WEBLOG("[HomeKit] 指令：風速 %d%% → 檔位 %u", targetPercent, fanLevel);
      if (targetPercent > 0) {
        if (writeRegisterWithRetry(0x07, fanLevel)) {
          fanState.speedLevel = fanLevel;
          fanState.speedPercent = targetPercent;
          WEBLOG("[HomeKit] 風速指令已寫入 Modbus");
        } else {
          statusFault->setVal(1);
          WEBLOG("[HomeKit] 風速指令失敗（Modbus）");
        }

        // 設風速時若仍為關機，自動開機（Active 同次已更新則交給上方電源邏輯）
        if (!active->updated() && active->getVal() == 0) {
          WEBLOG("[HomeKit] 風速>0，自動開機");
          writeRegisterWithRetry(0x06, 0x04);
          fanState.speedMode = 0x04;
          if (writeRegisterWithRetry(0x05, 0x01)) {
            fanState.power = 0x01;
            active->setVal(1);
            currentState->setVal(2);
          } else {
            statusFault->setVal(1);
            WEBLOG("[HomeKit] 自動開機失敗（Modbus）");
          }
        }
      } else if (!active->updated() && active->getVal() == 1) {
        // 風速設為 0% → 自動關機
        WEBLOG("[HomeKit] 風速=0，自動關機");
        if (writeRegisterWithRetry(0x05, 0x00)) {
          fanState.power = 0x00;
          active->setVal(0);
          currentState->setVal(0);
        } else {
          statusFault->setVal(1);
          WEBLOG("[HomeKit] 自動關機失敗（Modbus）");
        }
      }
    }

    lastPollTime = millis();
    return true;
  }

  void loop() override {
    if (!isInitialized) {
      // WiFi/DHCP 完成前不要做阻塞式 Modbus，否則容易出現 0.0.0.0 假連線、HomeKit 廣播壞掉
      if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        return;
      }
      initOnBoot();
      lastPollTime = millis();
      return;
    }

    if (millis() - lastPollTime >= POLL_INTERVAL) {
      lastPollTime = millis();
      pollPurifierStatus();
      if (filterService) {
        filterService->pollFilterStatus();
      }
    }
  }

  // 定時巡檢：以 ESP32 (HomeKit) 為絕對真理，若風機狀態跑掉則強制校正
  void pollPurifierStatus() {
    delay(30);
    uint8_t res1 = node.readHoldingRegisters(0x05, 3); // 讀取 0x05~0x07[cite: 8]
    if (res1 == node.ku8MBSuccess) {
      fanState.modbusOnline = true;
      uint16_t actualPower = node.getResponseBuffer(0); // 0x05[cite: 8]
      uint16_t actualMode  = node.getResponseBuffer(1); // 0x06[cite: 8]
      uint16_t actualLevel = node.getResponseBuffer(2); // 0x07[cite: 8]

      uint16_t targetPower = (active->getVal() == 1) ? 0x01 : 0x00;
      uint16_t targetLevel = map((int)rotationSpeed->getVal(), 1, 100, 1, 17);

      // 絕對真理覆寫：只要風機硬體狀態與 HomeKit 目標不符，立刻強制限時覆寫
      if (actualMode != 0x04 || actualPower != targetPower || (targetPower == 0x01 && actualLevel != targetLevel)) {
        Serial.println("[Monitor] 偵測到風機狀態與 HomeKit 不符，強制校正中...");
        initOnBoot();
        return;
      }

      fanState.power = actualPower;
      fanState.speedMode = actualMode;
      fanState.speedLevel = actualLevel;
      fanState.speedPercent = (int)rotationSpeed->getVal();

    } else {
      fanState.modbusOnline = false;
      statusFault->setVal(1);
      return;
    }

    // 讀取純硬體感測與診斷數據：0x0D ~ 0x13[cite: 8, 9]
    delay(30);
    uint8_t res2 = node.readHoldingRegisters(0x0D, 7);
    if (res2 == node.ku8MBSuccess) {
      fanState.rpm       = node.getResponseBuffer(0); // 0x0D[cite: 8]
      fanState.errorCode = node.getResponseBuffer(3); // 0x10[cite: 8, 9]
      fanState.sysStatus = node.getResponseBuffer(6); // 0x13[cite: 8, 9]

      fanState.tempError  = (fanState.errorCode & 0x20) != 0; // Bit5[cite: 9]
      fanState.motorError = (fanState.errorCode & 0x80) != 0; // Bit7[cite: 9]
      statusFault->setVal((fanState.tempError || fanState.motorError) ? 1 : 0);

      fanState.isRunning       = (fanState.sysStatus & 0x01) != 0; // Bit0[cite: 9]
      fanState.lowTempProtect  = (fanState.sysStatus & 0x40) != 0; // Bit6[cite: 9]
      fanState.halfSpeedSwitch = (fanState.sysStatus & 0x80) != 0; // Bit7[cite: 9]

      // 0=INACTIVE, 1=IDLE, 2=PURIFYING
      uint8_t newCurrentState = 0;
      if (active->getVal() == 1) {
        newCurrentState = fanState.isRunning ? 2 : 1;
      }
      if (currentState->getVal() != newCurrentState) {
        currentState->setVal(newCurrentState);
      }
    }
  }
};

// ================= 主程式 Setup & Loop =================
void setup() {
  Serial.begin(115200);

#if USE_RS485_DE_RE
  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);
#endif
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN); // RS485 鮑率 9600[cite: 7]

  node.begin(DELTA_FAN_ID, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // 勿呼叫 disableLoopWDT()：Arduino 仍會每圈 esp_task_wdt_reset()，會狂洗
  // 「task not found」。自訂 WDT 已移除；HomeSpan WDT 維持關閉即可。
  homeSpan.disableWatchdog();

  homeSpan.setSketchVersion("2.3.0-AirPurifier");
  homeSpan.setLogLevel(1); // 0=精簡, 1=含 HAP / WEBLOG 細節, 2=最完整
  // WiFi 網頁 Log：http://<IP>/log 或 http://HomeSpan-xxxx.local/log（最多 50 筆）
  homeSpan.enableWebLog(50, "pool.ntp.org", "CST-8", "log");
  homeSpan.enableAutoStartAP();
  homeSpan.setControlPin(0); // 啟用板載 BOOT 鍵作為除錯/重設按鈕

  homeSpan.setConnectionCallback([](int count) {
    if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      sawInvalidIp = true;
      Serial.println("[WiFi] 無效 IP 0.0.0.0（HomeKit mDNS 可能已壞），等待重連後重啟…");
      return;
    }

    wifiEverConnected = true;

    // HomeSpan 只在「第一次」連線呼叫 configureNetwork()；若那次是 0.0.0.0，
    // 之後拿到真 IP 也不會重建 mDNS → Home App 找不到配件，必須重啟一次。
    if (sawInvalidIp) {
      Serial.printf("[WiFi] 已取得有效 IP %s，重新啟動以修復 HomeKit 廣播…\n",
                    WiFi.localIP().toString().c_str());
      delay(300);
      ESP.restart();
    }

    if (!httpStarted) {
      registerHttpRoutes();
      httpServer.begin();
      httpStarted = true;
      Serial.printf("\n[HTTP] API：http://%s:8080/api\n", WiFi.localIP().toString().c_str());
      Serial.printf("[HTTP] 狀態：http://%s:8080/status\n\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("\n[WiFi] 已重連，IP：%s\n\n", WiFi.localIP().toString().c_str());
    }
  });

  homeSpan.begin(Category::AirPurifiers, "Delta VDB36");

  // SPAN_ACCESSORY() 已內建 AccessoryInformation + Identify，勿再 new 一次
  SPAN_ACCESSORY("台達 VDB36 空氣清淨機");
    new Characteristic::Manufacturer("Delta Electronics");
    new Characteristic::Model("VDB36AEXT-F RS485");
    new Characteristic::FirmwareRevision("2.3.0");

    DeltaFilterMaintenance *filterSrv = nullptr;
    if (SUPPORT_FILTER_SERVICE) {
      filterSrv = new DeltaFilterMaintenance();
    }
    DeltaAirPurifier *purifier = new DeltaAirPurifier(filterSrv);
    // FilterMaintenance 必須 link 到 AirPurifier，家庭 App 才會顯示濾網壽命／重置
    if (filterSrv) {
      purifier->addLink(filterSrv);
    }
}

void loop() {
  homeSpan.poll();
  httpServer.handleClient();

  // 有存帳密但一直連不上：前 20 秒照常重連；逾時則開設定熱點（阻塞至儲存/逾時後重啟）
  if (!apFallbackStarted && !wifiEverConnected && millis() >= WIFI_AP_FALLBACK_MS) {
    HS_STATUS st = homeSpan.getStatus().first;
    if (st == HS_WIFI_CONNECTING || st == HS_WIFI_SCANNING) {
      Serial.println("[WiFi] 20 秒仍連不上已存網路，自動開啟設定熱點…");
      apFallbackStarted = true;
      homeSpan.processSerialCommand("A");
    }
  }
}