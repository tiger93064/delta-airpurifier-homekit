/**
 * 台達管道風機 — Modbus 位址掃描
 * 自動流向 485（無 DE/RE）；會剔除 TX 迴聲再解析回覆。
 *
 *   pio run -e fan_scan -t upload && pio device monitor -e fan_scan
 */

#include <Arduino.h>
#include <string.h>

static const uint32_t BAUD = 9600;
static const uint16_t REPLY_TIMEOUT_MS = 400;

static uint8_t pinRx = 25;
static uint8_t pinTx = 26;
static uint32_t totalRxBytes = 0;   // 含可能的迴聲
static uint32_t realRxBytes = 0;    // 剔除迴聲後

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

static void reopenSerial() {
  Serial2.end();
  delay(50);
  Serial2.begin(BAUD, SERIAL_8N1, pinRx, pinTx);
  delay(100);
}

static void printHex(const uint8_t *p, int n) {
  for (int i = 0; i < n; i++) {
    Serial.printf("%02X%s", p[i], (i + 1 < n) ? " " : "");
  }
}

/**
 * 讀 holding。自動流向模組常把 TX 迴聲打進 RX，
 * 這裡會把開頭與請求相同的 8 byte 當迴聲丟掉。
 */
static int modbusProbe(uint8_t slave, uint16_t reg, uint16_t *value, bool *ok,
                       uint8_t *rawOut, int *rawLen) {
  *ok = false;
  *rawLen = 0;

  uint8_t req[8] = {
    slave, 0x03,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    0x00, 0x01, 0, 0
  };
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  while (Serial2.available()) Serial2.read();

  Serial.printf("  TX [%u] ", slave);
  printHex(req, 8);
  Serial.println();

  Serial2.write(req, 8);
  Serial2.flush();

  uint8_t buf[40];
  int n = 0;
  uint32_t t0 = millis();
  while (n < (int)sizeof(buf) && (millis() - t0) < REPLY_TIMEOUT_MS) {
    if (Serial2.available()) {
      buf[n++] = Serial2.read();
      t0 = millis();  // 有資料就延長
    }
  }
  totalRxBytes += n;

  // 剔除與請求相同的迴聲前綴
  int start = 0;
  if (n >= 8 && memcmp(buf, req, 8) == 0) {
    start = 8;
  } else if (n > 0 && n < 8) {
    // 殘碎迴聲：若像請求開頭就整段丟掉
    if (buf[0] == req[0]) {
      start = n;
    }
  }

  int m = n - start;
  if (m < 0) m = 0;
  realRxBytes += m;
  *rawLen = m;
  if (m > 0 && rawOut) {
    memcpy(rawOut, buf + start, m);
  }

  if (m >= 7 && buf[start] == slave && buf[start + 1] == 0x03 && buf[start + 2] == 0x02) {
    uint16_t got = crc16(buf + start, 5);
    if ((got & 0xFF) == buf[start + 5] && (got >> 8) == buf[start + 6]) {
      *value = ((uint16_t)buf[start + 3] << 8) | buf[start + 4];
      *ok = true;
    }
  }
  return m;
}

static uint8_t found[16];
static uint8_t foundCount = 0;

static void scanRange(uint8_t from, uint8_t to) {
  Serial.printf("  掃描 %u~%u (RX=GPIO%u TX=GPIO%u)\n", from, to, pinRx, pinTx);
  uint32_t realBefore = realRxBytes;

  for (uint16_t addr = from; addr <= to; addr++) {
    uint16_t val = 0;
    bool ok = false;
    uint8_t raw[40];
    int rawLen = 0;

    int m = modbusProbe((uint8_t)addr, 0x00, &val, &ok, raw, &rawLen);
    if (!ok) {
      m = modbusProbe((uint8_t)addr, 0x05, &val, &ok, raw, &rawLen);
    }

    if (ok) {
      Serial.printf("  ★ 找到 %u (0x%02X) 值=0x%04X\n", addr, addr, val);
      if (foundCount < sizeof(found)) found[foundCount++] = (uint8_t)addr;
    } else if (m > 0) {
      Serial.printf("  ? 位址 %u 剔除迴聲後仍有 %d byte: ", addr, m);
      printHex(raw, m > 16 ? 16 : m);
      Serial.println();
    }

    if ((addr % 20) == 0) Serial.printf("  ... %u\n", addr);
    delay(20);
  }

  Serial.printf("  此輪有效回傳 %lu byte（已扣 TX 迴聲）\n",
                (unsigned long)(realRxBytes - realBefore));
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== 台達風機 位址掃描（自動流向／剔迴聲）===");
  Serial.println("先前「1 byte 無法解析」多半是 TX 迴聲，不是風機。");
  Serial.println("階段1 有雜訊、階段2 無 → TTL 腳位用階段1（TX→25 RX→26）。");
  Serial.println("若仍找不到：對調風機 A/B，確認強-N 有電。");
  Serial.println();

  foundCount = 0;
  totalRxBytes = 0;
  realRxBytes = 0;

  // 正確腳位：模組 TX→25、RX→26
  pinRx = 25;
  pinTx = 26;
  reopenSerial();
  Serial.println("【階段1】出廠範圍 200~254");
  scanRange(200, 254);

  if (foundCount == 0) {
    Serial.println("【階段2】完整 1~247（同腳位）");
    scanRange(1, 247);
  }

  // 若完全沒有效資料，再試對調腳位一次
  if (foundCount == 0 && realRxBytes == 0) {
    pinRx = 26;
    pinTx = 25;
    reopenSerial();
    Serial.println("【階段3】對調腳位掃 200~254");
    scanRange(200, 254);
  }

  Serial.println();
  if (foundCount == 0) {
    Serial.println("結果：找不到從機。");
    Serial.printf("統計：原始 RX %lu byte，扣迴聲後 %lu byte\n",
                  (unsigned long)totalRxBytes, (unsigned long)realRxBytes);
    if (realRxBytes == 0) {
      Serial.println("下一步：對調風機 A↔B 後按 Reset。");
      Serial.println("仍無則查風機強-N 電源／換 5V 供 485 模組。");
    }
  } else {
    Serial.printf("結果：找到 %u 台 — ", foundCount);
    for (uint8_t i = 0; i < foundCount; i++) {
      Serial.printf("%u(0x%02X)%s", found[i], found[i],
                    (i + 1 < foundCount) ? ", " : "\n");
    }
  }
  Serial.println("結束。");
}

void loop() {
  delay(1000);
}
