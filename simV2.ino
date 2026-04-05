#include <EEPROM.h>
#include "time.h"
#include <sys/time.h>
#include <Arduino.h>
#include "esp_system.h"

#define RX_PIN 16
#define TX_PIN 17
#define LED_PIN 2
#define RAIN_PIN 5
#define BAT_PIN 14
HardwareSerial SerialAT(2);

// ================= CONFIG =================
#define AT_QUEUE_SIZE 40
#define SMS_QUEUE_SIZE 10
#define AT_BUFFER_MAX 1024

#define EEPROM_SIZE 32
#define ADDR_ID 0

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
uint32_t resetTime;
bool reset_trigger= false;

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
String number = "+84335644677";
// ================= DATA =================
char urlBuffer[400];
char IdTram[8] = "032026";

int batVol = 12;
int dh = 4600;
int dt = 140;
int numberOfClicks10m = 0;
int numberOfClicks24h = 0;
float currentVoltage = 0.0;

// ================= FUNCTION PROTOTYPES =================
void atAdd(String cmd, String expect = "OK", uint32_t timeout = 5000, uint8_t retry = 2);
void atStartNext();
void atLoop();
void processResponse(String cmd, String res);
void processSMSQueue();
void sendHTTP(bool needSyncTime = false);
void checkSendHTTP();
void blinkLED(int times);
bool parseHttpAction(String res, int &method, int &status, int &length);
bool parseSecondsFromServer(String raw, unsigned long &secondsOfDay);
void setTimeFromSecondsOfDay(unsigned long secondsOfDay);
void handleSMSCommand(String sender, String content);
void queueSMSReply(String phone, String text);
void updateBatteryVoltage();
float getBatteryVoltage() ;

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

// ================= EEPROM =================
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

// ================= LED =================
void blinkLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
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
    t.tm_mon  = 3;   // April
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

  blinkLED(3);
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
    } else {
      Serial.println("SET ID ERROR");
      queueSMSReply(number, "ERROR");
    }
  } 
  if (contentLower == "reset") {
    reset_trigger = true;
    resetTime = millis();
    Serial.println("RESET COMMAND RECEIVED");
    queueSMSReply(number, "RESETTING...");
  }
  else {
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
  // ===== SMS READ =====
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

  // ===== SMS SEND STEP 1 =====
  if (cmd.startsWith("AT+CMGS=")) {
    smsSendStep1 = false;
    smsSendStep2 = true;
    smsSendQueued = false;

    Serial.println("SMS PROMPT READY");
    SerialAT.print(smsReplyText);
    SerialAT.write(26); // Ctrl+Z
  }

  // ===== SMS SEND STEP 2 =====
  if (smsSendStep2 && res.indexOf("+CMGS:") != -1) {
    smsSendStep2 = false;
    smsSendStep1 = false;
    smsSendQueued = false;
    lastSMSNumber = "";
    smsReplyText = "";
    Serial.println("SMS SEND OK");
  }

  // ===== HTTPACTION =====
  if (cmd == "AT+HTTPACTION=0") {
    int method = 0, status = 0, length = 0;

    if (parseHttpAction(res, method, status, length)) {
      Serial.printf("HTTPACTION: method=%d status=%d len=%d\n", method, status, length);

      if (status == 200) {
        if (timeReadPending && length > 0) {
          httpBodyLen = length;
          atAdd("AT+HTTPREAD=0," + String(length), "+HTTPREAD:", 10000, 2);
        } else {
          httpBusy = false;
          timeReadPending = false;
        }
      } else {
        Serial.println("HTTPACTION FAIL");
        httpBusy = false;
        timeReadPending = false;
      }
    } else {
      Serial.println("PARSE HTTPACTION FAIL");
      httpBusy = false;
      timeReadPending = false;
    }
  }

  // ===== HTTPREAD =====
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

  // ===== Detect SMS new =====
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

  // ===== CMGS prompt =====
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
  sprintf(urlBuffer,
    "AT+HTTPPARA=\"URL\",\"http://113.160.225.84:2018/muahanquoc/write4g_data.php?id_tram=%s&Nhiet_do=%i&Do_am=%i&Muaob=%lu&Mua24h=%lu&Nguon=%i&key=2019a\"",
    IdTram, dt, dh,
    numberOfClicks10m,
    numberOfClicks24h,
    batVol
  );
  Serial.println("ID" + String(IdTram));
  Serial.println("rain 10m count: " + String(numberOfClicks10m) + " | 24h count: " + String(numberOfClicks24h));

  atAdd(urlBuffer);
  atAdd("AT+HTTPACTION=0", "+HTTPACTION:", 15000, 3);
  //// reset
  struct tm t;
  
  numberOfClicks10m = 0;
  if (t.tm_hour == 0 && t.tm_min < 1 && t.tm_sec < 30) 
  {
    resetTime = millis();
    reset_trigger = true;
  }
  


}

// ================= TIME SLOT =================
void checkSendHTTP() {
  if (!timeSynced) return;
  if (httpBusy) return;

  struct tm t;
  if (!getLocalTime(&t, 1000)) return;
  
  if ((t.tm_min % 10 == 0) && t.tm_sec < 5) {
    static int lastMin = -1;

    if (lastMin != t.tm_min) {
      lastMin = t.tm_min;
      Serial.println("SEND HTTP DATA");
      sendHTTP(false);
    }
  }
}


//==================Read VALUE==============
void updateBatteryVoltage() {
  static unsigned long lastUpdateTime = 0;
  const unsigned long interval = 1000;

  if (millis() - lastUpdateTime >= interval) {
    lastUpdateTime = millis();

    int raw = analogRead(BAT_PIN);
    float instantVoltage = (raw * 0.0042118) + 0.5247;
    currentVoltage = (instantVoltage * 0.6) + (currentVoltage * 0.4);
  }
}

float getBatteryVoltage() {
  return currentVoltage;
}

void CountRain() {
  static uint8_t lastRawState = HIGH;         // trạng thái đọc trực tiếp gần nhất
  static uint8_t stableState  = HIGH;         // trạng thái đã ổn định
  static uint32_t lastDebounceTime = 0;
  const uint32_t debounceDelay = 100;         // 100 ms chống nhiễu

  uint8_t reading = digitalRead(RAIN_PIN);

  // Nếu tín hiệu vừa thay đổi thì reset thời gian chống dội
  if (reading != lastRawState) {
    lastDebounceTime = millis();
    lastRawState = reading;
  }

  // Chỉ cập nhật khi tín hiệu giữ ổn định đủ 100ms
  if ((millis() - lastDebounceTime) >= debounceDelay) {
    if (stableState != reading) {
      stableState = reading;

      // Đếm khi có cạnh xuống HIGH -> LOW
      if (stableState == LOW) {
        numberOfClicks10m++;
        numberOfClicks24h++;
        //Serial.println("rain 10m count: " + String(numberOfClicks10m) +
  //                " | 24h count: " + String(numberOfClicks24h));
      }
    }
  }
}
void reset()
{
  if (reset_trigger && resetTime + 1000 < millis() )
  {
    atAdd("AT+CFUN=1,1"); 
    Serial.println("Sent reset command to SIM module: AT+CFUN=1,1");
    delay(1000); // cho SIM nhan lenh
    Serial.println("Restarting ESP32...");
    esp_restart();
  }
}
// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  SerialAT.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  pinMode(LED_PIN, OUTPUT);
  pinMode(RAIN_PIN, INPUT_PULLUP);
  pinMode(BAT_PIN, INPUT);
  int raw = analogRead(BAT_PIN);
  currentVoltage = (raw * 0.0042118) + 0.5247;

  EEPROM.begin(EEPROM_SIZE);
  Serial.print(__DATE__);
  Serial.println(__TIME__);

  Serial.println("Startup");
  Serial.println("Load ID");
  loadID();

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
  atLoop();
  processSMSQueue();
  checkSendHTTP();
  digitalWrite(LED_PIN, httpBusy);
  CountRain();
  updateBatteryVoltage();
  //resetValue();
  while (Serial.available()) {
    SerialAT.write(Serial.read());
  }
}