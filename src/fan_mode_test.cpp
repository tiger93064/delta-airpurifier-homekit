/**
 * 台達管道風機 — 全模式輪詢測試（自動流向 485，無 DE/RE）
 *
 * 順序：開機 → 低風 → 高風 → 待機 → 多段 1~17 → 關機
 * 每步間隔 STEP_SEC 秒，方便聽／看風量差異。
 *
 *   pio run -e fan_mode_test -t upload && pio device monitor -e fan_mode_test
 */

#include <Arduino.h>
#include <string.h>

#define RX_PIN 16
#define TX_PIN 17

static const uint8_t FAN_ID = 0xC8;       // 出廠位址；若不同請改
static const uint32_t BAUD = 9600;
static const uint16_t REPLY_TIMEOUT_MS = 400;
static const uint16_t STEP_SEC = 8;       // 每種模式停留秒數

static const uint16_t REG_POWER = 0x05;
static const uint16_t REG_MODE  = 0x06;
static const uint16_t REG_LEVEL = 0x07;
static const uint16_t REG_RPM   = 0x0D;

static uint16_t crc16(const uint8_t *data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

static void printHex(const uint8_t *p, int n) {
  for (int i = 0; i < n; i++) {
    Serial.printf("%02X%s", p[i], (i + 1 < n) ? " " : "");
  }
}

/**
 * 剔除 TX 迴聲。
 * 注意：FC 0x06 的從機回覆與請求完全相同，
 * 若只收到 8 byte，必須當成功 ACK，不可整段當迴聲丟棄。
 */
static int echoStart(const uint8_t *buf, int n, const uint8_t *req, int reqLen) {
  if (reqLen < 2 || n < reqLen) return 0;
  const uint8_t fc = req[1];

  if (fc == 0x06) {
    // 迴聲 + ACK = 16 byte 且兩段都等於請求
    if (n >= reqLen * 2 &&
        memcmp(buf, req, reqLen) == 0 &&
        memcmp(buf + reqLen, req, reqLen) == 0) {
      return reqLen;
    }
    // 只有一份 8 byte → 視為 ACK（不要當迴聲丟掉）
    return 0;
  }

  // FC 0x03 等：回覆與請求不同，開頭相同才是迴聲
  if (memcmp(buf, req, reqLen) == 0) return reqLen;
  return 0;
}

static int transact(const uint8_t *req, int reqLen, uint8_t *resp, int respMax) {
  while (Serial2.available()) Serial2.read();

  Serial.print("  TX ");
  printHex(req, reqLen);
  Serial.println();

  Serial2.write(req, reqLen);
  Serial2.flush();

  uint8_t buf[40];
  int n = 0;
  uint32_t t0 = millis();
  while (n < (int)sizeof(buf) && (millis() - t0) < REPLY_TIMEOUT_MS) {
    if (Serial2.available()) {
      buf[n++] = Serial2.read();
      t0 = millis();
    }
  }

  // 除錯：印原始匯流排資料（含可能的迴聲）
  Serial.print("  RAW ");
  if (n == 0) Serial.println("(無)");
  else {
    printHex(buf, n);
    Serial.println();
  }

  int start = echoStart(buf, n, req, reqLen);
  int m = n - start;
  if (m < 0) m = 0;
  if (m > respMax) m = respMax;
  if (m > 0) memcpy(resp, buf + start, m);

  Serial.print("  RX ");
  if (m == 0) Serial.println("(無)");
  else {
    printHex(resp, m);
    Serial.println();
  }
  return m;
}

static bool readReg(uint16_t reg, uint16_t *value) {
  uint8_t req[8] = {
    FAN_ID, 0x03,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    0x00, 0x01, 0, 0
  };
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  uint8_t resp[16];
  int m = transact(req, 8, resp, sizeof(resp));
  if (m < 7 || resp[0] != FAN_ID || resp[1] != 0x03 || resp[2] != 0x02) return false;
  uint16_t got = crc16(resp, 5);
  if ((got & 0xFF) != resp[5] || (got >> 8) != resp[6]) return false;
  *value = ((uint16_t)resp[3] << 8) | resp[4];
  return true;
}

static bool writeReg(uint16_t reg, uint16_t value) {
  uint8_t req[8] = {
    FAN_ID, 0x06,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    (uint8_t)(value >> 8), (uint8_t)(value & 0xFF),
    0, 0
  };
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  uint8_t resp[16];
  int m = transact(req, 8, resp, sizeof(resp));
  bool ack = (m >= 8 && memcmp(resp, req, 8) == 0);

  // 以回讀確認（比 ACK 更可靠）
  delay(100);
  uint16_t got = 0xFFFF;
  bool verified = readReg(reg, &got) && got == value;
  Serial.printf("  寫入%s ACK，回讀 0x%04X %s\n",
                ack ? "有" : "無", got, verified ? "（符合）" : "（不符）");
  return verified || ack;
}

static void holdAndReport(const char *label) {
  Serial.printf(">>> %s — 停留 %u 秒，請觀察風量\n", label, STEP_SEC);
  for (uint16_t s = 0; s < STEP_SEC; s++) {
    delay(1000);
    uint16_t rpm = 0, power = 0, mode = 0, level = 0;
    bool okRpm = readReg(REG_RPM, &rpm);
    readReg(REG_POWER, &power);
    readReg(REG_MODE, &mode);
    readReg(REG_LEVEL, &level);
    if (okRpm) {
      Serial.printf("  [%2us] power=0x%02X mode=0x%02X level=%u rpm=%u\n",
                    s + 1, power, mode, level, rpm);
    } else {
      Serial.printf("  [%2us] power=0x%02X mode=0x%02X level=%u rpm=?\n",
                    s + 1, power, mode, level);
    }
  }
  Serial.println();
}

static bool step(const char *name, uint16_t reg, uint16_t val) {
  Serial.printf("=== 設定 %s：reg 0x%02X = 0x%04X ===\n", name, reg, val);
  if (!writeReg(reg, val)) {
    Serial.println("  !! 寫入失敗");
    return false;
  }
  delay(500);  // 讓風機吃指令
  holdAndReport(name);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== 台達風機 全模式輪詢測試 ===");
  Serial.printf("位址 0x%02X，每步停留 %u 秒\n\n", FAN_ID, STEP_SEC);

  Serial2.begin(BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(300);

  // 探測
  uint16_t probe = 0;
  if (!readReg(REG_POWER, &probe)) {
    Serial.println("風機無回應，請檢查配線／位址。");
    return;
  }
  Serial.printf("連線成功，目前 power=0x%04X\n\n", probe);

  // --- 開機 ---
  step("開機", REG_POWER, 0x01);

  // --- 固定風量模式（0x06）---
  step("低風量", REG_MODE, 0x05);
  step("高風量", REG_MODE, 0x06);
  step("待機",   REG_MODE, 0x07);

  // --- 多段調速 1~17 ---
  Serial.println("=== 切換多段調速模式 0x04 ===");
  if (!writeReg(REG_MODE, 0x04)) {
    Serial.println("  !! 多段模式寫入失敗");
  } else {
    delay(500);
    for (uint16_t lv = 1; lv <= 17; lv++) {
      char name[24];
      snprintf(name, sizeof(name), "多段速度 %u/17", lv);
      step(name, REG_LEVEL, lv);
    }
  }

  // --- 關機 ---
  step("關機", REG_POWER, 0x00);

  Serial.println("=== 全模式測試結束 ===");
  Serial.println("按 Reset 可再跑一輪。");
}

void loop() {
  delay(2000);
}
