#include "HomeSpan.h"
#include <ModbusMaster.h>
#include <WebServer.h>
#include <esp_task_wdt.h>

// ================= 硬體腳位與參數定義 =================
#define RX_PIN 25        // 接至 JSM485ESA 的 RO
#define TX_PIN 26        // 接至 JSM485ESA 的 DI
#define DE_RE_PIN 4      // 接至 JSM485ESA 的 DE 與 RE

#define WDT_TIMEOUT 15   // 看門狗超時時間 (秒)
const uint8_t DELTA_FAN_ID = 0xC8; // 台達風機預設地址 200 (0xC8)[cite: 8]
const uint32_t POLL_INTERVAL = 10000; // 定時讀取狀態間隔 (10秒)

// VDB36AEXT-F 支援濾網功能
const bool SUPPORT_FILTER_SERVICE = true; 

ModbusMaster node;
WebServer httpServer(80); // 建立 Port 80 的 HTTP JSON 伺服器

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
  digitalWrite(DE_RE_PIN, HIGH);
  delayMicroseconds(500);
}

void postTransmission() {
  delayMicroseconds(500);
  digitalWrite(DE_RE_PIN, LOW);
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
      if (writeRegisterWithRetry(0x1F, 0x5A)) { // 寫入 0x5A 重置濾網[cite: 8]
        filterChange->setVal(0);
        filterLife->setVal(100);
        fanState.filterNeedChange = 0;
        fanState.filterUsedHours = 0;
        fanState.filterLifePercent = 100.0f;
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

// ================= 風機主控制服務 (Fan Service) =================
struct DeltaDuctFan : Service::Fan {
  SpanCharacteristic *active;
  SpanCharacteristic *currentState;
  SpanCharacteristic *rotationSpeed;
  SpanCharacteristic *statusFault;

  DeltaFilterMaintenance *filterService;
  uint32_t lastPollTime = 0;
  bool isInitialized = false;

  DeltaDuctFan(DeltaFilterMaintenance *filter = nullptr) : Service::Fan() {
    filterService = filter;
    active = new Characteristic::Active(0, true);
    currentState = new Characteristic::CurrentFanState(0);
    rotationSpeed = new Characteristic::RotationSpeed(50, true);
    statusFault = new Characteristic::StatusFault(0);
  }

  void initFanOnBoot() {
    writeRegisterWithRetry(0x06, 0x04); // 設定為多段調速[cite: 8, 10]
    fanState.speedMode = 0x04;

    int savedSpeed = rotationSpeed->getVal();
    if (savedSpeed > 0) {
      uint16_t fanLevel = map(savedSpeed, 1, 100, 1, 17); // 1~17 檔位[cite: 8, 9]
      writeRegisterWithRetry(0x07, fanLevel);
      fanState.speedLevel = fanLevel;
      fanState.speedPercent = savedSpeed;
    }

    int savedActive = active->getVal();
    writeRegisterWithRetry(0x05, (savedActive == 1) ? 0x01 : 0x00); // 開關機[cite: 8, 10]
    fanState.power = (savedActive == 1) ? 0x01 : 0x00;

    isInitialized = true;
  }

  boolean update() override {
    if (active->updated()) {
      int targetPower = active->getNewVal();
      if (targetPower == 1) {
        writeRegisterWithRetry(0x06, 0x04);
        fanState.speedMode = 0x04;
      }

      if (writeRegisterWithRetry(0x05, targetPower ? 0x01 : 0x00)) {
        fanState.power = targetPower ? 0x01 : 0x00;
        currentState->setVal(targetPower ? 2 : 0);
      } else {
        statusFault->setVal(1);
      }
    }

    if (rotationSpeed->updated()) {
      int targetPercent = rotationSpeed->getNewVal();
      if (targetPercent > 0) {
        uint16_t fanLevel = map(targetPercent, 1, 100, 1, 17);
        if (writeRegisterWithRetry(0x07, fanLevel)) {
          fanState.speedLevel = fanLevel;
          fanState.speedPercent = targetPercent;
        } else {
          statusFault->setVal(1);
        }
      }
    }

    lastPollTime = millis();
    return true;
  }

  void loop() override {
    if (!isInitialized) {
      initFanOnBoot();
      lastPollTime = millis();
      return;
    }

    if (millis() - lastPollTime >= POLL_INTERVAL) {
      lastPollTime = millis();
      pollFanStatus();
      if (filterService) {
        filterService->pollFilterStatus();
      }
    }
  }

  // 定時巡檢：以 ESP32 (HomeKit) 為絕對真理，若風機狀態跑掉則強制校正
  void pollFanStatus() {
    delay(30);
    uint8_t res1 = node.readHoldingRegisters(0x05, 3); // 讀取 0x05~0x07[cite: 8]
    if (res1 == node.ku8MBSuccess) {
      fanState.modbusOnline = true;
      uint16_t actualPower = node.getResponseBuffer(0); // 0x05[cite: 8]
      uint16_t actualMode  = node.getResponseBuffer(1); // 0x06[cite: 8]
      uint16_t actualLevel = node.getResponseBuffer(2); // 0x07[cite: 8]

      uint16_t targetPower = (active->getVal() == 1) ? 0x01 : 0x00;
      uint16_t targetLevel = map(rotationSpeed->getVal(), 1, 100, 1, 17);

      // 絕對真理覆寫：只要風機硬體狀態與 HomeKit 目標不符，立刻強制限時覆寫
      if (actualMode != 0x04 || actualPower != targetPower || (targetPower == 0x01 && actualLevel != targetLevel)) {
        Serial.println("[Monitor] 偵測到風機狀態與 HomeKit 不符，強制校正中...");
        initFanOnBoot();
        return;
      }

      fanState.power = actualPower;
      fanState.speedMode = actualMode;
      fanState.speedLevel = actualLevel;
      fanState.speedPercent = rotationSpeed->getVal();

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

  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN); // RS485 鮑率 9600[cite: 7]

  node.begin(DELTA_FAN_ID, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  homeSpan.setSketchVersion("2.2.0-Master");
  homeSpan.enableAutoStartAP();
  homeSpan.setControlPin(0); // 啟用板載 BOOT 鍵作為除錯/重設按鈕

  homeSpan.setWifiCallback([]() {
    httpServer.on("/", HTTP_GET, handleStatusJson);
    httpServer.on("/status", HTTP_GET, handleStatusJson);
    httpServer.begin();
    Serial.printf("\n[HTTP] JSON API 已啟動：http://%s/status\n\n", WiFi.localIP().toString().c_str());
  });

  homeSpan.begin(Category::Fans, "Delta VDB36 Fan");

  SPAN_ACCESSORY();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("台達 VDB36 管道風機");
      new Characteristic::Manufacturer("Delta Electronics");
      new Characteristic::Model("VDB36AEXT-F RS485");
      new Characteristic::FirmwareRevision("2.2.0");

    DeltaFilterMaintenance *filterSrv = nullptr;
    if (SUPPORT_FILTER_SERVICE) {
      filterSrv = new DeltaFilterMaintenance();
    }
    new DeltaDuctFan(filterSrv);

  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdt_config);
  #else
    esp_task_wdt_init(WDT_TIMEOUT, true);
  #endif
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  homeSpan.poll();
  httpServer.handleClient();
}