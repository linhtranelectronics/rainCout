#include <EEPROM.h>
#include "time.h"
#include <sys/time.h>
#include <Arduino.h>
#include "esp_system.h"
#include "esp_task_wdt.h"

#define RX_PIN 20
#define TX_PIN 21
#define LED_PIN 10
#define RAIN_LED_PIN 1
#define RAIN_PIN 5
#define BAT_PIN 7
#define SIM_PWR_PIN 9

HardwareSerial SerialAT(1);

// ================= CONFIG =================
#define AT_QUEUE_SIZE 40
#define SMS_QUEUE_SIZE 10

#define EEPROM_SIZE 64
#define ADDR_ID 0
#define ADDR_RAIN24H 8
#define RAIN_STORE_MAGIC 0xA5
#define WDT_TIMEOUT_SEC 600

// ================= AT STRUCT =================
struct ATCommand {
  String cmd;
  String expect;
  uint8_t retry;
  uint32_t timeout;
};

// ================= QUEUE =================
ATCommand atQueue[AT_QUEUE_SIZE];
int qHead = 0, qTail = 0;

// ================= ENGINE =================
bool atRunning = false;
ATCommand current;

String atBuffer = "";
String lastCmd = "";

unsigned long atStart = 0;
uint8_t retryCount = 0;

// ================= RESET =================
uint32_t resetTime = 0;
bool reset_trigger = false;

// ================= STATE =================
bool httpBusy = false;
bool timeSynced = false;
bool firstHttpNeedSyncTime = true;
bool timeReadPending = false;
int httpBodyLen = 0;

// ================= SMS =================
String smsQueue[SMS_QUEUE_SIZE];
int smsHead = 0, smsTail = 0;

String lastSMSNumber = "";
bool smsSendStep1 = false;
bool smsSendStep2 = false;
bool smsSendQueued = false;
String smsReplyText = "";
String number = "+84909296141";

// ================= DATA =================
char urlBuffer[400];
char IdTram[8] = "032026";

int batVol = 12;
int dh = 4600;
int dt = 140;

uint32_t numberOfClicks10m = 0;
uint32_t numberOfClicks24h = 0;
float currentVoltage = 0.0;

// snapshot 10p dang gui
uint32_t pendingHttpRain10m = 0;
bool pendingHttpCommit10m = false;

// lan gui khoi dong de dong bo gio
uint32_t startupRain24hToSend = 0;
bool startupRain24hAvailable = false;
uint32_t startupSentRain24h = 0;
bool startupSentRain24hValid = false;

// quan ly du lieu 24h sau khi sync time
bool rainStoreLoaded = false;
bool rainDateValidated = false;
bool needSendAfterRestore = false;
uint32_t bootRainClicksBeforeValidate = 0;

// quan ly chuyen ngay 00:00
bool midnightRolloverPending = false;
bool pendingHttpMidnightCommit = false;
uint32_t midnightFrozenRain24h = 0;
uint32_t midnightNewDayClicks = 0;
uint16_t rolloverNewYear = 0;
uint8_t rolloverNewMonth = 0;
uint8_t rolloverNewDay = 0;
int currentDayKey = -1;

struct __attribute__((packed)) Rain24hStore {
  uint8_t magic;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint32_t clicks24h;
  uint8_t checksum;
};

Rain24hStore savedRainStore;

// ================= LED NON-BLOCKING =================
// LED_PIN = led he thong/http
bool ledPulseActive = false;
uint8_t ledPulseTogglesRemaining = 0;
bool ledPulseState = false;
unsigned long ledPulseLastMillis = 0;
unsigned long ledPulseInterval = 80;
bool ledBaseState = false;

// RAIN_LED_PIN = led bao xung mua
bool rainLedPulseActive = false;
uint8_t rainLedPulseTogglesRemaining = 0;
bool rainLedPulseState = false;
unsigned long rainLedPulseLastMillis = 0;
unsigned long rainLedPulseInterval = 50;

// ================= FUNCTION PROTOTYPES =================
void atAdd(String cmd, String expect = "OK", uint32_t timeout = 5000, uint8_t retry = 2);
void atStartNext();
void atLoop();
void processResponse(String cmd, String res);
void processSMSQueue();
void sendHTTP(bool needSyncTime = false);
void checkSendHTTP();

bool parseHttpAction(String res, int &method, int &status, int &length);
bool parseSecondsFromServer(String raw, unsigned long &secondsOfDay);
void setTimeFromSecondsOfDay(unsigned long secondsOfDay);
void handleSMSCommand(String sender, String content);
void queueSMSReply(String phone, String text);
void updateBatteryVoltage();
float getBatteryVoltage();
void CountRain();
void resetTask();

uint8_t calcRainStoreChecksum(const Rain24hStore &data);
bool loadRain24hStoreFromEEPROM();
void saveRain24hStoreToEEPROM(uint16_t year, uint8_t month, uint8_t day, uint32_t clicks24h);
void clearRain24hStoreEEPROM();
void restoreRain24hAfterTimeSync();
void saveRain24hIfNeededEveryMinute();
void commitRain10mAfterHttpSuccess();
void commitMidnightRolloverAfterHttpSuccess();
void handleRainDayChange();

void blinkLED(uint8_t times, unsigned long intervalMs = 200);
void updateBlinkLED();
void applyLedState();

void pulseRainLED();
void updateRainLED();
void applyRainLedState();
void initWatchdog();
void feedWatchdog();


// ================= TRAM LIST =================
struct Tram {
  const char* id;
  const char* name;
};

Tram tramList[] = {
  {"032026","test1"},{"032027","test2"},{"032028","test3"},
  {"091537","Hoa Phu"},{"091501","Phong My"},{"091562","Thang Binh 1"},
  {"091564","Ladee"},{"091536","Hoa Bac"},{"091561","Nui Thanh 1"},
  {"091645","Tra Phong"},{"091644","Son Ba"},{"091643","Son Mua"},
  {"091565","Tra Don"},{"091563","TrHy"},{"091560","Tam Tra"}
};

#define TRAM_COUNT (sizeof(tramList) / sizeof(tramList[0]))

// ================= EEPROM ID =================
void loadID() {
  for (int i = 0; i < 6; i++) {
    IdTram[i] = EEPROM.read(ADDR_ID + i);
    if (IdTram[i] == 0xFF) {
      strcpy(IdTram, "032026");
      IdTram[6] = '\0';
      return;
    }
  }
  IdTram[6] = '\0';

  Serial.print("SET ID: ");
  Serial.println(IdTram);
}

void saveID() {
  for (int i = 0; i < 6; i++) {
    EEPROM.write(ADDR_ID + i, IdTram[i]);
  }
  EEPROM.commit();
}

// ================= EEPROM RAIN24H =================
uint8_t calcRainStoreChecksum(const Rain24hStore &data) {
  uint8_t sum = 0;
  const uint8_t *p = (const uint8_t*)&data;
  for (size_t i = 0; i < sizeof(Rain24hStore) - 1; i++) {
    sum ^= p[i];
  }
  return sum;
}

bool loadRain24hStoreFromEEPROM() {
  EEPROM.get(ADDR_RAIN24H, savedRainStore);

  if (savedRainStore.magic != RAIN_STORE_MAGIC) {
    rainStoreLoaded = false;
    startupRain24hAvailable = false;
    startupRain24hToSend = 0;
    return false;
  }

  uint8_t cs = calcRainStoreChecksum(savedRainStore);
  if (cs != savedRainStore.checksum) {
    rainStoreLoaded = false;
    startupRain24hAvailable = false;
    startupRain24hToSend = 0;
    return false;
  }

  rainStoreLoaded = true;
  startupRain24hAvailable = true;
  startupRain24hToSend = savedRainStore.clicks24h;

  Serial.printf("RAIN24H EEPROM LOAD OK: %04u-%02u-%02u | %lu\n",
                savedRainStore.year,
                savedRainStore.month,
                savedRainStore.day,
                (unsigned long)savedRainStore.clicks24h);
  return true;
}

void saveRain24hStoreToEEPROM(uint16_t year, uint8_t month, uint8_t day, uint32_t clicks24h) {
  Rain24hStore data;
  data.magic = RAIN_STORE_MAGIC;
  data.year = year;
  data.month = month;
  data.day = day;
  data.clicks24h = clicks24h;
  data.checksum = 0;
  data.checksum = calcRainStoreChecksum(data);

  EEPROM.put(ADDR_RAIN24H, data);
  EEPROM.commit();

  savedRainStore = data;
  rainStoreLoaded = true;
  startupRain24hAvailable = true;
  startupRain24hToSend = clicks24h;

  Serial.printf("RAIN24H EEPROM SAVE: %04u-%02u-%02u | %lu\n",
                year, month, day, (unsigned long)clicks24h);
}

void clearRain24hStoreEEPROM() {
  Rain24hStore data = {};
  EEPROM.put(ADDR_RAIN24H, data);
  EEPROM.commit();

  rainStoreLoaded = false;
  startupRain24hAvailable = false;
  startupRain24hToSend = 0;
  savedRainStore = {};
  Serial.println("RAIN24H EEPROM CLEARED");
}

void restoreRain24hAfterTimeSync() {
  if (rainDateValidated) return;

  struct tm t;
  if (!getLocalTime(&t, 1000)) return;

  uint16_t year = t.tm_year + 1900;
  uint8_t month = t.tm_mon + 1;
  uint8_t day = t.tm_mday;

  if (rainStoreLoaded &&
      savedRainStore.year == year &&
      savedRainStore.month == month &&
      savedRainStore.day == day) {

    numberOfClicks24h = savedRainStore.clicks24h + bootRainClicksBeforeValidate;
    Serial.printf("RAIN24H RESTORED SAME DAY: %lu\n", (unsigned long)numberOfClicks24h);
  } else {
    numberOfClicks24h = bootRainClicksBeforeValidate;
    clearRain24hStoreEEPROM();
    saveRain24hStoreToEEPROM(year, month, day, numberOfClicks24h);
    Serial.printf("RAIN24H NEW DAY -> RESET: %lu\n", (unsigned long)numberOfClicks24h);
  }

  bootRainClicksBeforeValidate = 0;
  startupRain24hAvailable = false;
  startupRain24hToSend = 0;

  rainDateValidated = true;
  currentDayKey = (year * 10000) + (month * 100) + day;

  if (startupSentRain24hValid && startupSentRain24h != numberOfClicks24h) {
    needSendAfterRestore = true;
    Serial.printf("STARTUP DATA DIFF -> RESEND CORRECT 24H: sent=%lu restored=%lu\n",
                  (unsigned long)startupSentRain24h,
                  (unsigned long)numberOfClicks24h);
  } else {
    needSendAfterRestore = false;
    Serial.printf("STARTUP DATA SAME -> NO RESEND: %lu\n",
                  (unsigned long)numberOfClicks24h);
  }
}

void saveRain24hIfNeededEveryMinute() {
  if (!timeSynced || !rainDateValidated) return;

  struct tm t;
  if (!getLocalTime(&t, 1000)) return;

  static int lastCheckedMinuteKey = -1;
  int minuteKey = (t.tm_yday * 1440) + (t.tm_hour * 60) + t.tm_min;

  if (minuteKey == lastCheckedMinuteKey) return;
  lastCheckedMinuteKey = minuteKey;

  if (midnightRolloverPending) {
    Serial.println("EEPROM CHECK 1M -> SKIP, WAIT MIDNIGHT SEND OLD DATA");
    return;
  }

  uint16_t year = t.tm_year + 1900;
  uint8_t month = t.tm_mon + 1;
  uint8_t day = t.tm_mday;

  bool needSave = (!rainStoreLoaded) ||
                  (savedRainStore.year != year) ||
                  (savedRainStore.month != month) ||
                  (savedRainStore.day != day) ||
                  (savedRainStore.clicks24h != numberOfClicks24h);

  if (needSave) {
    Serial.printf("EEPROM CHECK 1M -> UPDATE: %04u-%02u-%02u | %lu\n",
                  year, month, day, (unsigned long)numberOfClicks24h);
    saveRain24hStoreToEEPROM(year, month, day, numberOfClicks24h);
  } else {
    Serial.println("EEPROM CHECK 1M -> NO CHANGE");
  }
}

void handleRainDayChange() {
  if (!timeSynced || !rainDateValidated) return;

  struct tm t;
  if (!getLocalTime(&t, 1000)) return;

  uint16_t year = t.tm_year + 1900;
  uint8_t month = t.tm_mon + 1;
  uint8_t day = t.tm_mday;
  int dayKey = (year * 10000) + (month * 100) + day;

  if (currentDayKey == -1) {
    currentDayKey = dayKey;
    return;
  }

  if (dayKey != currentDayKey && !midnightRolloverPending) {
    midnightRolloverPending = true;
    midnightFrozenRain24h = numberOfClicks24h;
    midnightNewDayClicks = 0;
    rolloverNewYear = year;
    rolloverNewMonth = month;
    rolloverNewDay = day;
    currentDayKey = dayKey;

    Serial.printf("DAY CHANGED -> WAIT SEND OLD DATA FIRST: old24h=%lu | newDay=%04u-%02u-%02u\n",
                  (unsigned long)midnightFrozenRain24h,
                  rolloverNewYear, rolloverNewMonth, rolloverNewDay);
  }
}

void commitRain10mAfterHttpSuccess() {
  if (!pendingHttpCommit10m) return;

  if (numberOfClicks10m >= pendingHttpRain10m) {
    numberOfClicks10m -= pendingHttpRain10m;
  } else {
    numberOfClicks10m = 0;
  }

  Serial.printf("HTTP OK -> COMMIT RAIN10M, remain=%lu\n", (unsigned long)numberOfClicks10m);

  pendingHttpRain10m = 0;
  pendingHttpCommit10m = false;
}

void commitMidnightRolloverAfterHttpSuccess() {
  if (!pendingHttpMidnightCommit) return;

  if (!midnightRolloverPending) {
    pendingHttpMidnightCommit = false;
    return;
  }

  numberOfClicks24h = midnightNewDayClicks;
  saveRain24hStoreToEEPROM(rolloverNewYear, rolloverNewMonth, rolloverNewDay, numberOfClicks24h);

  Serial.printf("HTTP OK -> MIDNIGHT ROLLOVER COMMIT, new24h=%lu\n",
                (unsigned long)numberOfClicks24h);

  midnightFrozenRain24h = 0;
  midnightNewDayClicks = 0;
  midnightRolloverPending = false;
  pendingHttpMidnightCommit = false;

  reset_trigger = true;
  resetTime = millis();
}

// ================= UTIL =================
const char* findTramName(String id) {
  for (int i = 0; i < TRAM_COUNT; i++) {
    if (id == tramList[i].id) return tramList[i].name;
  }
  return NULL;
}

bool parseHttpAction(String res, int &method, int &status, int &length) {
  int p = res.indexOf("+HTTPACTION:");
  if (p == -1) return false;

  int endLine = res.indexOf('\n', p);
  String line = (endLine == -1) ? res.substring(p) : res.substring(p, endLine);
  line.trim();

  int matched = sscanf(line.c_str(), "+HTTPACTION: %d,%d,%d", &method, &status, &length);
  return matched == 3;
}

bool parseSecondsFromServer(String raw, unsigned long &secondsOfDay) {
  raw.trim();

  int firstTilde = raw.indexOf('~');
  int secondTilde = raw.indexOf('~', firstTilde + 1);

  if (firstTilde == -1 || secondTilde == -1) return false;

  String secStr = raw.substring(firstTilde + 1, secondTilde);
  secStr.trim();

  if (secStr.length() == 0) return false;

  secondsOfDay = strtoul(secStr.c_str(), NULL, 10);
  if (secondsOfDay > 86399) return false;

  return true;
}

// ================= SYSTEM LED NON-BLOCKING =================
void applyLedState() {
  if (ledPulseActive) {
    digitalWrite(LED_PIN, ledPulseState ? HIGH : LOW);
  } else {
    digitalWrite(LED_PIN, ledBaseState ? HIGH : LOW);
  }
}

void blinkLED(uint8_t times, unsigned long intervalMs) {
  if (times == 0) return;

  ledPulseActive = true;
  ledPulseTogglesRemaining = times * 2;
  ledPulseState = false;
  ledPulseLastMillis = millis();
  ledPulseInterval = intervalMs;

  applyLedState();
}

void updateBlinkLED() {
  ledBaseState = httpBusy;

  if (!ledPulseActive) {
    applyLedState();
    return;
  }

  if (millis() - ledPulseLastMillis >= ledPulseInterval) {
    ledPulseLastMillis = millis();
    ledPulseState = !ledPulseState;

    if (ledPulseTogglesRemaining > 0) {
      ledPulseTogglesRemaining--;
    }

    if (ledPulseTogglesRemaining == 0) {
      ledPulseActive = false;
      ledPulseState = false;
    }

    applyLedState();
  }
}

// ================= RAIN LED NON-BLOCKING =================
void applyRainLedState() {
  if (rainLedPulseActive) {
    digitalWrite(RAIN_LED_PIN, rainLedPulseState ? HIGH : LOW);
  } else {
    digitalWrite(RAIN_LED_PIN, LOW);
  }
}

void pulseRainLED() {
  rainLedPulseActive = true;
  rainLedPulseTogglesRemaining = 2;   // 1 lan nhay = HIGH + LOW
  rainLedPulseState = false;
  rainLedPulseLastMillis = millis();
  rainLedPulseInterval = 50;
  applyRainLedState();
}

void updateRainLED() {
  if (!rainLedPulseActive) {
    applyRainLedState();
    return;
  }

  if (millis() - rainLedPulseLastMillis >= rainLedPulseInterval) {
    rainLedPulseLastMillis = millis();
    rainLedPulseState = !rainLedPulseState;

    if (rainLedPulseTogglesRemaining > 0) {
      rainLedPulseTogglesRemaining--;
    }

    if (rainLedPulseTogglesRemaining == 0) {
      rainLedPulseActive = false;
      rainLedPulseState = false;
    }

    applyRainLedState();
  }
}

// ================= WATCHDOG =================
void initWatchdog() {
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("WDT INIT FAIL: %d\n", err);
  }

  err = esp_task_wdt_add(NULL);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("WDT ADD FAIL: %d\n", err);
  }

  Serial.printf("WDT ENABLED: %d seconds\n", WDT_TIMEOUT_SEC);
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

// ================= TIME =================
void setTimeFromSecondsOfDay(unsigned long secondsOfDay) {
  int hh = secondsOfDay / 3600;
  int mm = (secondsOfDay % 3600) / 60;
  int ss = secondsOfDay % 60;

  struct tm t;
  bool hasCurrentTime = getLocalTime(&t, 1000);

  if (!hasCurrentTime) {
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 3;
    t.tm_mday = 3;
  }

  t.tm_hour = hh;
  t.tm_min  = mm;
  t.tm_sec  = ss;
  t.tm_isdst = -1;

  time_t epoch = mktime(&t);
  struct timeval now = { epoch, 0 };
  settimeofday(&now, NULL);

  timeSynced = true;
  firstHttpNeedSyncTime = false;

  Serial.printf("TIME SYNC OK: %02d:%02d:%02d\n", hh, mm, ss);

  struct tm checkTime;
  if (getLocalTime(&checkTime, 1000)) {
    Serial.printf("LOCAL TIME = %04d-%02d-%02d %02d:%02d:%02d\n",
                  checkTime.tm_year + 1900,
                  checkTime.tm_mon + 1,
                  checkTime.tm_mday,
                  checkTime.tm_hour,
                  checkTime.tm_min,
                  checkTime.tm_sec);
  }

  blinkLED(3, 120);
  restoreRain24hAfterTimeSync();
}

// ================= SMS SEND =================
void queueSMSReply(String phone, String text) {
  lastSMSNumber = phone;
  smsReplyText = text;
  smsSendStep1 = true;
  smsSendStep2 = false;
  smsSendQueued = false;
}

void handleSMSCommand(String sender, String content) {
  content.trim();
  Serial.println("SMS FROM: " + sender);
  Serial.println("SMS CONTENT: " + content);

  String contentLower = content;
  contentLower.toLowerCase();

  if (contentLower.startsWith("a") && contentLower.length() >= 7) {
    String newID = contentLower.substring(1, 7);
    const char* name = findTramName(newID);

    if (name != NULL) {
      strcpy(IdTram, newID.c_str());
      saveID();

      String reply = "ID:" + newID + " - " + String(name);
      Serial.println("SET ID OK -> " + reply);
      queueSMSReply(number, reply);
    }
  } else if (contentLower == "@123reset") {
    reset_trigger = true;
    resetTime = millis();
    Serial.println("RESET COMMAND RECEIVED");
    queueSMSReply(number, "RESETTING...");
  } else {
    Serial.println("SMS SYNTAX ERROR");
    queueSMSReply(number, "ERROR");
  }
}

// ================= AT CORE =================
void atAdd(String cmd, String expect, uint32_t timeout, uint8_t retry) {
  int next = (qTail + 1) % AT_QUEUE_SIZE;
  if (next == qHead) {
    Serial.println("AT QUEUE FULL");
    return;
  }

  atQueue[qTail] = {cmd, expect, retry, timeout};
  qTail = next;
}

void atStartNext() {
  if (qHead == qTail) return;

  current = atQueue[qHead];
  qHead = (qHead + 1) % AT_QUEUE_SIZE;

  atBuffer = "";
  lastCmd = current.cmd;
  retryCount = 0;

  Serial.println("Sent to SIM >> " + current.cmd);
  SerialAT.println(current.cmd);

  atStart = millis();
  atRunning = true;
}

// ================= RESPONSE =================
void processResponse(String cmd, String res) {
  if (cmd.startsWith("AT+CMGR")) {
    String smsStatus = "";
    String sender = "";
    String content = "";

    int cmgrPos = res.indexOf("+CMGR:");
    if (cmgrPos != -1) {
      int lineEnd = res.indexOf('\n', cmgrPos);
      String header = (lineEnd == -1) ? res.substring(cmgrPos) : res.substring(cmgrPos, lineEnd);
      header.trim();

      int q1 = header.indexOf('"');
      int q2 = header.indexOf('"', q1 + 1);
      if (q1 != -1 && q2 != -1) {
        smsStatus = header.substring(q1 + 1, q2);
      }

      int q3 = header.indexOf('"', q2 + 1);
      int q4 = header.indexOf('"', q3 + 1);
      if (q3 != -1 && q4 != -1) {
        sender = header.substring(q3 + 1, q4);
      }

      if (lineEnd != -1) {
        String tail = res.substring(lineEnd + 1);
        int okPos = tail.lastIndexOf("OK");
        if (okPos != -1) {
          tail = tail.substring(0, okPos);
        }
        tail.trim();
        content = tail;
      }
    }

    Serial.println("SMS STATUS: " + smsStatus);
    Serial.println("SMS FROM: " + sender);
    Serial.println("SMS CONTENT: " + content);

    if ((smsStatus == "REC UNREAD" || smsStatus == "REC READ") &&
        sender.length() > 0 &&
        content.length() > 0) {
      handleSMSCommand(sender, content);
    } else {
      Serial.println("IGNORE NON-INCOMING SMS");
    }
  }

  if (cmd.startsWith("AT+CMGS=")) {
    smsSendStep1 = false;
    smsSendStep2 = true;
    smsSendQueued = false;

    Serial.println("SMS PROMPT READY");
    SerialAT.print(smsReplyText);
    SerialAT.write(26);
  }

  if (smsSendStep2 && res.indexOf("+CMGS:") != -1) {
    smsSendStep2 = false;
    smsSendStep1 = false;
    smsSendQueued = false;
    lastSMSNumber = "";
    smsReplyText = "";
    Serial.println("SMS SEND OK");
  }

  if (cmd == "AT+HTTPACTION=0") {
    int method = 0, status = 0, length = 0;

    if (parseHttpAction(res, method, status, length)) {
      Serial.printf("HTTPACTION: method=%d status=%d len=%d\n", method, status, length);

      if (status == 200) {
        if (timeReadPending && length > 0) {
          httpBodyLen = length;
          atAdd("AT+HTTPREAD=0," + String(length), "+HTTPREAD:", 10000, 2);
        } else {
          commitRain10mAfterHttpSuccess();
          commitMidnightRolloverAfterHttpSuccess();
          httpBusy = false;
          timeReadPending = false;
        }
      } else {
        Serial.println("HTTPACTION FAIL");
        pendingHttpCommit10m = false;
        pendingHttpRain10m = 0;
        pendingHttpMidnightCommit = false;
        httpBusy = false;
        timeReadPending = false;
      }
    } else {
      Serial.println("PARSE HTTPACTION FAIL");
      pendingHttpCommit10m = false;
      pendingHttpRain10m = 0;
      pendingHttpMidnightCommit = false;
      httpBusy = false;
      timeReadPending = false;
    }
  }

  if (cmd.startsWith("AT+HTTPREAD")) {
    String body = "";

    int firstHdr = res.indexOf("+HTTPREAD:");
    if (firstHdr != -1) {
      int firstLineEnd = res.indexOf('\n', firstHdr);
      if (firstLineEnd != -1) {
        body = res.substring(firstLineEnd + 1);
      }
    }

    int tailHdr = body.indexOf("+HTTPREAD: 0");
    if (tailHdr != -1) {
      body = body.substring(0, tailHdr);
    }

    body.trim();

    Serial.println("TIME BODY = " + body);

    unsigned long secondsOfDay = 0;
    if (parseSecondsFromServer(body, secondsOfDay)) {
      setTimeFromSecondsOfDay(secondsOfDay);
    } else {
      Serial.println("PARSE TIME BODY FAIL");
    }

    timeReadPending = false;
    httpBusy = false;
  }
}

// ================= AT LOOP =================
void atLoop() {
  while (SerialAT.available()) {
    char c = SerialAT.read();
    atBuffer += c;
    Serial.write(c);
  }

  int cmtiPos = atBuffer.indexOf("+CMTI:");
  if (cmtiPos != -1) {
    int commaPos = atBuffer.indexOf(',', cmtiPos);
    int lineEnd = atBuffer.indexOf('\n', cmtiPos);

    if (commaPos != -1 && lineEnd != -1) {
      String slot = atBuffer.substring(commaPos + 1, lineEnd);
      slot.trim();

      int next = (smsTail + 1) % SMS_QUEUE_SIZE;
      if (next != smsHead) {
        smsQueue[smsTail] = slot;
        smsTail = next;
        Serial.println("SMS NEW SLOT: " + slot);
      }

      atBuffer.remove(cmtiPos, lineEnd - cmtiPos + 1);
    }
  }

  if (!atRunning) {
    atStartNext();
    return;
  }

  if (current.cmd.startsWith("AT+CMGS=") && atBuffer.indexOf(">") != -1) {
    processResponse(lastCmd, atBuffer);
    atRunning = false;
    return;
  }

  if (atBuffer.indexOf(current.expect) != -1) {
    if (current.expect == "+HTTPACTION:") {
      int p = atBuffer.indexOf("+HTTPACTION:");
      int endLine = atBuffer.indexOf('\n', p);
      if (endLine == -1) return;
    }

    if (current.cmd.startsWith("AT+HTTPREAD")) {
      int firstHdr = atBuffer.indexOf("+HTTPREAD:");
      if (firstHdr == -1) return;

      int secondHdr = atBuffer.indexOf("+HTTPREAD: 0", firstHdr + 1);
      if (secondHdr == -1) {
        return;
      }
    }

    processResponse(lastCmd, atBuffer);
    atRunning = false;
    return;
  }

  if (millis() - atStart > current.timeout) {
    if (retryCount < current.retry) {
      retryCount++;

      Serial.printf("AT TIMEOUT -> retry %d/%d: %s\n",
                    retryCount, current.retry, current.cmd.c_str());

      atBuffer = "";
      SerialAT.println(current.cmd);
      atStart = millis();
      return;
    }

    Serial.println("AT FAIL: " + current.cmd);
    atRunning = false;

    if (current.cmd.startsWith("AT+HTTPREAD") || current.cmd == "AT+HTTPACTION=0") {
      pendingHttpCommit10m = false;
      pendingHttpRain10m = 0;
      pendingHttpMidnightCommit = false;
      httpBusy = false;
      timeReadPending = false;
    }

    if (current.cmd.startsWith("AT+CMGS=")) {
      smsSendStep1 = false;
      smsSendStep2 = false;
      smsSendQueued = false;
      lastSMSNumber = "";
      smsReplyText = "";
    }
  }
}

// ================= SMS =================
void processSMSQueue() {
  if (atRunning) return;

  if (smsSendStep1 && !smsSendStep2 && !smsSendQueued &&
      lastSMSNumber.length() > 0 && smsReplyText.length() > 0) {
    smsSendQueued = true;
    atAdd("AT+CMGS=\"" + lastSMSNumber + "\"", ">", 10000, 2);
    return;
  }

  if (smsHead == smsTail) return;

  String slot = smsQueue[smsHead];
  smsHead = (smsHead + 1) % SMS_QUEUE_SIZE;

  atAdd("AT+CMGR=" + slot);
  atAdd("AT+CMGD=" + slot);
}

// ================= HTTP =================
void sendHTTP(bool needSyncTime) {
  if (httpBusy) return;

  httpBusy = true;
  timeReadPending = needSyncTime;
  batVol = getBatteryVoltage();

  uint32_t rain10mToSend = 0;
  uint32_t rain24hToSend = 0;

  pendingHttpMidnightCommit = false;

  if (needSyncTime) {
    rain10mToSend = 0;

    if (startupRain24hAvailable) {
      rain24hToSend = startupRain24hToSend + bootRainClicksBeforeValidate;
    } else if (rainStoreLoaded) {
      rain24hToSend = savedRainStore.clicks24h + bootRainClicksBeforeValidate;
    } else {
      rain24hToSend = bootRainClicksBeforeValidate;
    }

    startupSentRain24h = rain24hToSend;
    startupSentRain24hValid = true;

    pendingHttpCommit10m = false;
    pendingHttpRain10m = 0;
  } else {
    rain10mToSend = numberOfClicks10m;
    rain24hToSend = midnightRolloverPending ? midnightFrozenRain24h : numberOfClicks24h;

    pendingHttpRain10m = rain10mToSend;
    pendingHttpCommit10m = true;
    pendingHttpMidnightCommit = midnightRolloverPending;
  }

  snprintf(urlBuffer, sizeof(urlBuffer),
    "AT+HTTPPARA=\"URL\",\"http://113.160.225.84:2018/muahanquocnghean/write_nghean.php?id_tram=%s&Nhiet_do=%i&Do_am=%i&Muaob=%lu&Mua24h=%lu&Nguon=%i&key=@2026\"",
    IdTram, dt, dh,
    (unsigned long)rain10mToSend,
    (unsigned long)rain24hToSend,
    batVol
  );

  Serial.println("ID " + String(IdTram));
  Serial.println("rain 10m send: " + String((unsigned long)rain10mToSend) +
                 " | 24h send: " + String((unsigned long)rain24hToSend) +
                 (midnightRolloverPending ? " | MIDNIGHT_OLD_DAY" : ""));

  atAdd(urlBuffer);
  atAdd("AT+HTTPACTION=0", "+HTTPACTION:", 15000, 3);
}

// ================= TIME SLOT =================
void checkSendHTTP() {
  if (!timeSynced) return;
  if (httpBusy) return;

  struct tm t;
  if (!getLocalTime(&t, 1000)) return;

  if ((t.tm_min % 10 == 0) && (t.tm_sec < 10 && t.tm_sec >= 1)) {
    static int lastMin = -1;

    if (lastMin != t.tm_min) {
      lastMin = t.tm_min;
      Serial.println("SEND HTTP DATA");
      sendHTTP(false);
    }
  }
}

// ================= BATTERY =================
void updateBatteryVoltage() {
  static unsigned long lastUpdateTime = 0;
  const unsigned long interval = 1000;

  if (millis() - lastUpdateTime >= interval) {
    lastUpdateTime = millis();

    int raw = analogRead(BAT_PIN);
    float instantVoltage = (raw * 0.0042118f) + 0.5247f;
    currentVoltage = (instantVoltage * 0.6f) + (currentVoltage * 0.4f);
  }
}

float getBatteryVoltage() {
  return currentVoltage;
}

// ================= RAIN COUNT =================
void CountRain() {
  static uint8_t lastRawState = HIGH;
  static uint8_t stableState  = HIGH;
  static uint32_t lastDebounceTime = 0;
  const uint32_t debounceDelay = 100;

  uint8_t reading = digitalRead(RAIN_PIN);

  if (reading != lastRawState) {
    lastDebounceTime = millis();
    lastRawState = reading;
  }

  if ((millis() - lastDebounceTime) >= debounceDelay) {
    if (stableState != reading) {
      stableState = reading;

      if (stableState == LOW) {
        numberOfClicks10m++;

        if (rainDateValidated) {
          if (midnightRolloverPending) {
            midnightNewDayClicks++;
          } else {
            numberOfClicks24h++;
          }
        } else {
          bootRainClicksBeforeValidate++;
        }

        pulseRainLED();
      }
    }
  }
}

// ================= RESET =================
void resetTask() {
  if (reset_trigger && resetTime + 2000 < millis()) {
    reset_trigger = false;

    Serial.println("MIDNIGHT SEND DONE -> RESTART SYSTEM");
    SerialAT.println("AT+CFUN=1,1");
    delay(300);
    digitalWrite(SIM_PWR_PIN, HIGH);
    delay(300);
    esp_restart();
  }
}

// ================= SETUP =================
void setup() {
  pinMode(SIM_PWR_PIN, OUTPUT);
  digitalWrite(SIM_PWR_PIN, LOW);

  Serial.begin(115200);
  SerialAT.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(RAIN_LED_PIN, OUTPUT);
  digitalWrite(RAIN_LED_PIN, LOW);

  pinMode(RAIN_PIN, INPUT_PULLUP);
  pinMode(BAT_PIN, INPUT);

  int raw = analogRead(BAT_PIN);
  currentVoltage = (raw * 0.0042118f) + 0.5247f;

  EEPROM.begin(EEPROM_SIZE);

  initWatchdog();
  loadRain24hStoreFromEEPROM();

  Serial.print(__DATE__);
  Serial.println(__TIME__);

  Serial.println("Startup");
  Serial.println("Load ID");
  loadID();

  delay(2000);

  atAdd("AT");
  atAdd("AT+CMGF=1");
  atAdd("AT+CNMI=2,1,0,0,0");
  atAdd("AT+CGDCONT=1,\"IP\",\"v-internet\"");
  atAdd("AT+CGACT=1,1");
  atAdd("AT+HTTPINIT", "OK", 1000, 0);

  Serial.println("Startup HTTP sync time");
  sendHTTP(true);


  Serial.println("Startup Done");
}

// ================= LOOP =================
void loop() {
  feedWatchdog();

  atLoop();
  processSMSQueue();
  checkSendHTTP();

  if (needSendAfterRestore && timeSynced && !httpBusy && rainDateValidated) {
    needSendAfterRestore = false;
    Serial.println("SEND RESTORED RAIN DATA AFTER TIME SYNC");
    sendHTTP(false);
  }

  CountRain();
  updateBatteryVoltage();
  saveRain24hIfNeededEveryMinute();
  handleRainDayChange();
  resetTask();

  updateBlinkLED();
  updateRainLED();

  while (Serial.available()) {
    SerialAT.write(Serial.read());
  }
}