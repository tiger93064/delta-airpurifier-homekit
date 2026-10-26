/**
 * 台達管道風機 — RS485 關機（自動流向，無 DE/RE）
 * 寫入暫存器 0x05 = 0x00
 *
 *   pio run -e fan_off -t upload && pio device monitor -e fan_off
 */

#include <Arduino.h>
#include <string.h>

#define RX_PIN 25  // ← 485 模組 TX
#define TX_PIN 26  // ← 485 模組 RX

static const uint32_t BAUD = 9600;
static const uint16_t REPLY_TIMEOUT_MS = 400;
static const uint16_t REG_POWER = 0x05;
static const uint16_t POWER_OFF = 0x00;

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

/** 送 raw 幀，回傳剔除 TX 迴聲後的 RX 長度；資料寫入 resp */
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

  // FC06 回覆=請求：單份 8 byte 當 ACK；兩份才剔第一段迴聲
  int start = 0;
  if (reqLen >= 2 && req[1] == 0x06) {
    if (n >= reqLen * 2 &&
        memcmp(buf, req, reqLen) == 0 &&
        memcmp(buf + reqLen, req, reqLen) == 0) {
      start = reqLen;
    }
  } else if (n >= reqLen && memcmp(buf, req, reqLen) == 0) {
    start = reqLen;
  }

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

static bool modbusReadHolding(uint8_t slave, uint16_t reg, uint16_t *value) {
  uint8_t req[8] = {
    slave, 0x03,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    0x00, 0x01, 0, 0
  };
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  uint8_t resp[16];
  int m = transact(req, 8, resp, sizeof(resp));
  if (m < 7 || resp[0] != slave || resp[1] != 0x03 || resp[2] != 0x02) return false;
  uint16_t got = crc16(resp, 5);
  if ((got & 0xFF) != resp[5] || (got >> 8) != resp[6]) return false;
  *value = ((uint16_t)resp[3] << 8) | resp[4];
  return true;
}

static bool modbusWriteSingle(uint8_t slave, uint16_t reg, uint16_t value) {
  uint8_t req[8] = {
    slave, 0x06,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    (uint8_t)(value >> 8), (uint8_t)(value & 0xFF),
    0, 0
  };
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  uint8_t resp[16];
  int m = transact(req, 8, resp, sizeof(resp));
  // 寫入成功回覆應與請求相同（8 byte）
  return m >= 8 && memcmp(resp, req, 8) == 0;
}

static bool powerOffAt(uint8_t addr) {
  Serial.printf("--- 對位址 %u (0x%02X) 關機 ---\n", addr, addr);
  uint16_t cur = 0;
  if (modbusReadHolding(addr, REG_POWER, &cur)) {
    Serial.printf("  目前 0x05 = 0x%04X\n", cur);
  } else {
    Serial.println("  讀取失敗，仍嘗試寫入關機…");
  }

  if (!modbusWriteSingle(addr, REG_POWER, POWER_OFF)) {
    Serial.println("  寫入關機失敗。");
    return false;
  }
  Serial.println("  關機指令已送出。");

  delay(300);
  if (modbusReadHolding(addr, REG_POWER, &cur)) {
    Serial.printf("  回讀 0x05 = 0x%04X %s\n",
                  cur, cur == POWER_OFF ? "（已關機）" : "（非關機）");
    return cur == POWER_OFF;
  }
  return true;  // 寫入有 ACK，回讀失敗仍算大致成功
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== 台達風機 RS485 關機 ===");
  Serial.println("接線: 模組TX→GPIO25, 模組RX→GPIO26");

  Serial2.begin(BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(300);

  // 1) 先試出廠位址 200
  if (powerOffAt(0xC8)) {
    Serial.println("完成（位址 200）。");
    return;
  }

  // 2) 掃 200~254，對有回應的寫關機
  Serial.println("預設位址無回應，掃描 200~254…");
  for (uint16_t a = 200; a <= 254; a++) {
    uint16_t v = 0;
    if (!modbusReadHolding((uint8_t)a, REG_POWER, &v) &&
        !modbusReadHolding((uint8_t)a, 0x00, &v)) {
      continue;
    }
    Serial.printf("找到位址 %u，執行關機\n", a);
    if (powerOffAt((uint8_t)a)) {
      Serial.println("完成。");
      return;
    }
  }

  Serial.println("找不到風機。請對調 A/B 或先跑 fan_scan。");
}

void loop() {
  delay(2000);
}
