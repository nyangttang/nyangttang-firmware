#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

//----- 와이파이, 서버주소 -----
const char* WIFI_SSID   = "iptime-h";
const char* WIFI_PASS   = "SM-A520L66cf";
const char* SERVER      = "https://15-165-194-19.sslip.io";   // 서버 노트북 IP 
const char* DEVICE_CODE = "NYANG-0001";                 // 웹에서 등록하는 기기 코드
const char* DEVICE_KEY  = "cde585759e9a594b8c60d3f42df14d69b34c6a635920666238c4403fcbc39b5b";   // 서버 비밀키

// ----- 핀 -----
const int PHONE_PIN = 4;
const int TRIG_PIN  = 5;
const int ECHO_PIN  = 18;
const int RADAR_PIN = 17;
const int BUZZER_PIN = 16;

// ----- 기준값(자체 설정, 실제 측정 기준) -----
const bool PHONE_ACTIVE_LOW = true;   // TCRT5000 모듈(물체 있으면 LOW, 반대면 false)
const int  SEAT_MAX_CM  = 70;         // 초음파(70cm보다 가까우면 인식) 
// 사람 판단 방법: 0 = 레이더 + 초음파 둘 다 (빈 의자 방지 추가)
//                1 = 레이더만   2 = 초음파만 (예전 방식)
const int  SEAT_MODE    = 0;
const int  STABLE_COUNT = 3;          // 같은 값이 연속 3번이어야 상태 변경
const unsigned long OFFLINE_MS = 10000;

// ----- 소리 / 자리비움 알람 -----
const bool SOUND_ON = true;              // 기본값 -> 소리켜기(변경 가능)
const bool SOUND_INVADE = false;         // 친구 침입 소리 (기본은 표정만) ... 피드백 이후 수정 예정
const bool SOUND_HISS   = false;         // 마주침 소리 (기본은 표정만)
const unsigned long STUDY_SOUND_GAP_MS = 60000;   // 공부 시작 소리는 1분 넘게 쉬었다가 다시 시작할 때만
// 자리 비운 뒤 몇 초째에 알람을 울릴지 (10분, 30분, 1시간) ... 피드백이나 시연용 수정 예정
const unsigned long AWAY_ALARM_SEC[] = {600, 1800, 3600};
const int AWAY_MAX_TIMES = sizeof(AWAY_ALARM_SEC) / sizeof(AWAY_ALARM_SEC[0]);

// ----- 눈 화면 -----
Adafruit_SSD1306 eyeL(128, 64, &Wire1, -1);
Adafruit_SSD1306 eyeR(128, 64, &Wire, -1);
bool okL = false, okR = false;
const uint16_t BG = SSD1306_WHITE;    // 바탕(흰 얼굴)
const uint16_t FG = SSD1306_BLACK;    // 눈(검은 선)

// ----- 상태 -----
bool phone = false, seat = false;
int phoneCnt = 0, seatCnt = 0;
int lastCm = 999;
bool lastRadar = false;

bool studying = false;            // 폰 있음 + 사람 있음
unsigned long studyEndAt = 0;     // 마지막으로 공부가 끝난 시각
bool everStudied = false;
bool awayActive = false;          // 폰은 있는데 자리를 비운 상태인지
unsigned long awaySince = 0;
int alarmCount = 0;

char face[10] = "SLEEP";
int  land = 0, remainMin = 0;
char eventName[10] = "";
unsigned long eventUntil = 0;
long lastEventId = -1;
unsigned long lastRx = 0;            // 마지막으로 서버 응답 받은 시각

unsigned long lastRead = 0, lastSend = 0, lastDraw = 0, lastWifiTry = 0;
bool sendNow = false;
char buf[40]; uint8_t bufLen = 0;

// ---------- 소리 (기다리지 않고 재생: 화면, 통신이 멈추지 않음) ----------
struct Note { uint16_t f; uint16_t ms; };     
const Note SND_BOOT[]   = {{523, 90}, {0, 30}, {784, 140}};
const Note SND_START[]  = {{1319, 130}, {0, 40}, {1047, 320}};     // 공부 시작: 딩~동
const Note SND_PHONE[]  = {{1047, 60}, {0, 40}, {1319, 100}};
const Note SND_GOAL[]   = {{523, 80}, {659, 80}, {784, 80}, {0, 30}, {784, 260}};     // 빠바바-밤!
const Note SND_INVADE[] = {{1568, 80}, {0, 60}, {1568, 80}, {0, 60}, {1568, 80}};
const Note SND_HISS[]   = {{247, 180}, {0, 60}, {196, 260}};
const Note SND_ALARM[]  = {{1500, 200}, {0, 150}, {1500, 200}, {0, 150}, {1500, 200}};  // 뽀-뽀-뽀
#define PLAY(s) play(s, sizeof(s) / sizeof(s[0]))

const Note* song = nullptr;
int songLen = 0, songIdx = 0;
unsigned long noteEnd = 0;

void play(const Note* s, int n) {
  if (!SOUND_ON) return;
  song = s; songLen = n; songIdx = 0; noteEnd = 0;
}

void updateSound() {
  if (!song || millis() < noteEnd) return;
  if (songIdx >= songLen) { noTone(BUZZER_PIN); song = nullptr; return; }
  Note n = song[songIdx++];
  if (n.f) tone(BUZZER_PIN, n.f); else noTone(BUZZER_PIN);
  noteEnd = millis() + n.ms;
}

void playByName(const char* t) {
  if      (strcmp(t, "GOAL")   == 0) PLAY(SND_GOAL);
  else if (strcmp(t, "INVADE") == 0) { if (SOUND_INVADE) PLAY(SND_INVADE); }
  else if (strcmp(t, "HISS")   == 0) { if (SOUND_HISS)   PLAY(SND_HISS); }
  else if (strcmp(t, "START")  == 0) PLAY(SND_START);
  else if (strcmp(t, "ALARM")  == 0) PLAY(SND_ALARM);
  else if (strcmp(t, "PHONE")  == 0) PLAY(SND_PHONE);
  else if (strcmp(t, "BOOT")   == 0) PLAY(SND_BOOT);
}

// ----- 센서 -----
int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (us == 0) return 999;
  return (int)(us / 58);
}

void readSensors() {
  int raw = digitalRead(PHONE_PIN);
  bool p = PHONE_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
  lastCm = readDistanceCm();
  lastRadar = digitalRead(RADAR_PIN) == HIGH;          // 레이더: 사람(숨쉬는 것까지) 있으면 HIGH
  bool near = lastCm < SEAT_MAX_CM;
  bool s = (SEAT_MODE == 1) ? lastRadar : (SEAT_MODE == 2) ? near : (lastRadar && near);

  if (p != phone) {
    if (++phoneCnt >= STABLE_COUNT) {
      phone = p; phoneCnt = 0; sendNow = true;
    }
  }
  else phoneCnt = 0;
  if (s != seat)  { if (++seatCnt  >= STABLE_COUNT) { seat = s;  seatCnt  = 0; sendNow = true; } }
  else seatCnt = 0;
}

// ----- 공부 시작 소리: 폰 올리고 앉으면 "딩~동" (잠깐 일어났다 앉을 땐 안 나게) -----
void checkStudyStart() {
  bool now = phone && seat;
  if (now && !studying) {
    if (!everStudied || millis() - studyEndAt > STUDY_SOUND_GAP_MS) PLAY(SND_START);
    everStudied = true;
  }
  if (!now && studying) studyEndAt = millis();
  studying = now;
}

// ----- 자리비움 알람: 폰 있음 + 사람 없음이 오래 가면 부저 -----
void checkAwayAlarm() {
  bool away = phone && !seat;
  if (!away) { awayActive = false; alarmCount = 0; return; }   // 돌아오거나 폰을 들면 초기화 시키기
  if (!awayActive) { awayActive = true; awaySince = millis(); alarmCount = 0; }
  if (alarmCount >= AWAY_MAX_TIMES) return;
  unsigned long due = AWAY_ALARM_SEC[alarmCount] * 1000UL;
  if (millis() - awaySince >= due) {
    alarmCount++;
    PLAY(SND_ALARM);
    Serial.printf("ALARM,away,%d\n", alarmCount);
  }
}

// ----- 서버 통신: 센서 보내고, 표정 받기 (API 하나) -----
void copyWord(char* dst, const char* src, size_t n) { strncpy(dst, src, n - 1); dst[n - 1] = 0; }

void startEvent(const char* type) {
  copyWord(eventName, type, sizeof(eventName));
  eventUntil = millis() + (strcmp(type, "GOAL") == 0 ? 5000UL : 3000UL);
  playByName(type);                                    // 이벤트마다 소리
}

void heartbeat() {
  lastSend = millis();
  sendNow = false;
  Serial.printf("S,phone=%d,seat=%d,cm=%d,radar=%d\n", phone, seat, lastCm, lastRadar);   // 확인용
  if (WiFi.status() != WL_CONNECTED) return;

  // https 서버(배포 서버)면 암호화 연결, http(노트북 연습 서버)면 일반 연결
  static WiFiClientSecure secureClient;
  static bool secureReady = false;
  if (!secureReady) { secureClient.setInsecure(); secureReady = true; }   // 인증서 검사 생략 (학교 시연용)
  bool https = strncmp(SERVER, "https", 5) == 0;

  HTTPClient http;
  http.setReuse(true);                       // 연결을 재사용해서 매번 새로 접속하는 시간을 줄임
  String url = String(SERVER) + "/api/devices/" + DEVICE_CODE + "/heartbeat";
  if (https) http.begin(secureClient, url);
  else       http.begin(url);
  http.setConnectTimeout(4000);              // https는 첫 접속이 1~2초 걸려서 넉넉하게
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  char body[80];
  snprintf(body, sizeof(body), "{\"phone\":%s,\"seat\":%s,\"distanceCm\":%d}",
           phone ? "true" : "false", seat ? "true" : "false", lastCm);
  int code = http.POST(String(body));

  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      lastRx = millis();
      copyWord(face, doc["face"] | "SLEEP", sizeof(face));
      land = doc["landPercent"] | 0;
      remainMin = doc["remainMinutes"] | 0;
      JsonVariant ev = doc["event"];
      if (!ev.isNull()) {
        long id = ev["id"] | -1L;
        const char* type = ev["type"] | "";
        if (id != lastEventId && type[0]) { lastEventId = id; startEvent(type); }
      }
    }
  } else {
    Serial.printf("ERR,HTTP,%d\n", code);   // 음수 = 서버에 접속 못 함
  }
  http.end();
}

// ----- 시리얼 테스트 명령 -----
void handleLine(char* line) {
  lastRx = millis();
  char* type = strtok(line, ",");
  if (!type) return;
  if (strcmp(type, "F") == 0) {
    char* f = strtok(NULL, ","); char* l = strtok(NULL, ","); char* r = strtok(NULL, ",");
    if (f) copyWord(face, f, sizeof(face));
    if (l) land = atoi(l);
    if (r) remainMin = atoi(r);
  } else if (strcmp(type, "E") == 0) {
    char* e = strtok(NULL, ",");
    if (e) startEvent(e);
  } else if (strcmp(type, "B") == 0) {
    char* s = strtok(NULL, ",");                         // 꺼 둔 소리 테스트용
    if (!s) return;
    if      (strcmp(s, "INVADE") == 0) PLAY(SND_INVADE);
    else if (strcmp(s, "HISS")   == 0) PLAY(SND_HISS);
    else playByName(s);
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (bufLen) { buf[bufLen] = 0; handleLine(buf); bufLen = 0; } }
    else if (bufLen < sizeof(buf) - 1) buf[bufLen++] = c;
  }
}

// ----- 눈 그리기 (화면 하나 = 눈 하나, 128 x 64) -----
// 눈꺼풀: 가운데가 살짝 올라가고 두껍게. bend > 0 이면 아래로 휜 선(감은 눈)
void lid(Adafruit_SSD1306 &d, int y, int half, int bend, int thick) {
  int prevX = 64 - half, prevY = y;
  for (int x = 64 - half; x <= 64 + half; x += 4) {
    float t = (x - 64) / (float)half;
    int yy = y + (int)(bend * (1 - t * t));
    for (int k = 0; k < thick; k++) d.drawLine(prevX, prevY + k, x, yy + k, FG);
    prevX = x; prevY = yy;
  }
  d.fillCircle(64 - half, y + thick / 2, thick / 2, FG);   // 끝을 둥글게
  d.fillCircle(64 + half, y + thick / 2, thick / 2, FG);
}

// 반달 눈동자: 눈꺼풀 아래로 매달린 반원 + 작은 하이라이트
void pupil(Adafruit_SSD1306 &d, int cx, int y, int r) {
  d.fillCircle(cx, y, r, FG);
  d.fillRect(cx - r - 1, y - r - 1, 2 * r + 3, r + 1, BG);   // 위쪽 반 지우기
  d.fillCircle(cx - r / 3, y + r / 3, max(2, r / 6), BG);    // 하이라이트
}

void heart(Adafruit_SSD1306 &d) {
  d.fillCircle(50, 24, 14, FG);
  d.fillCircle(78, 24, 14, FG);
  d.fillTriangle(36, 29, 92, 29, 64, 60, FG);
}

// side: -1 = 왼쪽 눈, +1 = 오른쪽 눈
void drawEye(Adafruit_SSD1306 &d, int side, const char* f, const char* ev) {
  d.fillScreen(BG);
  d.setTextColor(FG);

  if (ev && strcmp(ev, "GOAL") == 0) {                 // 목표 달성: 하트 눈
    heart(d);
  } else if (ev && strcmp(ev, "INVADE") == 0) {        // 친구 침입: 놀라서 동그래진 눈 + !
    for (int r = 25; r <= 28; r++) d.drawCircle(64, 34, r, FG);
    d.fillCircle(64, 36, 11, FG);
    d.fillCircle(60, 32, 3, BG);
    if (side > 0) { d.setTextSize(3); d.setCursor(108, 2); d.print('!'); }
  } else if (ev && strcmp(ev, "HISS") == 0) {          // 마주침: > <
    for (int t = 0; t < 7; t++) {
      if (side < 0) { d.drawLine(36, 10 + t, 92, 30 + t, FG); d.drawLine(92, 30 + t, 36, 50 + t, FG); }
      else          { d.drawLine(92, 10 + t, 36, 30 + t, FG); d.drawLine(36, 30 + t, 92, 50 + t, FG); }
    }
  } else if (strcmp(f, "STUDY") == 0) {                // 공부 중: 반쯤 감은 눈 (레퍼런스 기본 얼굴)
    pupil(d, 64, 24, 30);
    lid(d, 21, 58, -3, 7);
  } else if (strcmp(f, "AWAY") == 0) {                 // 자리 비움: 눈 크게 뜨고 옆을 두리번 + ?
    pupil(d, 40, 16, 26);
    d.fillCircle(40, 16, 26, FG);                      // 동그란 눈동자 (위까지)
    d.fillCircle(32, 8, 4, BG);
    lid(d, 2, 58, -2, 5);
    if (side > 0) { d.setTextSize(3); d.setCursor(104, 36); d.print('?'); }
  } else {                                             // 휴식: 감은 눈 + zZ
    lid(d, 30, 56, 9, 7);
    if (side > 0) {
      d.setTextSize(1); d.setCursor(100, 6); d.print('z');
      d.setTextSize(2); d.setCursor(110, 0); d.print('Z');
    }
  }
}

void drawFace() {
  bool ev = eventName[0] && millis() < eventUntil;
  if (!ev) eventName[0] = 0;

  const char* f = face;
  bool offline = (millis() - lastRx) > OFFLINE_MS;
  if (offline) f = phone ? (seat ? "STUDY" : "AWAY") : "SLEEP";   // 서버 끊기면 센서만 보고 임시 표정

  if (okL) drawEye(eyeL, -1, f, ev ? eventName : nullptr);
  if (okR) drawEye(eyeR, +1, f, ev ? eventName : nullptr);

  // 작은 글씨: 왼쪽 눈 아래 = 남은 시간 / 연결 상태, 오른쪽 눈 아래 = 땅 %
  bool study = !ev && strcmp(f, "STUDY") == 0;
  if (okL) {
    eyeL.setTextSize(1); eyeL.setTextColor(FG); eyeL.setCursor(2, 56);
    if (WiFi.status() != WL_CONNECTED) eyeL.print("no wifi");
    else if (offline)                  eyeL.print("no server");
    else if (study)                    eyeL.printf("%d:%02d", remainMin / 60, remainMin % 60);
    eyeL.display();
  }
  if (okR) {
    if (study) { eyeR.setTextSize(1); eyeR.setTextColor(FG); eyeR.setCursor(100, 56); eyeR.printf("%d%%", land); }
    eyeR.display();
  }
}

// ----- 시작 -----
void setup() {
  Serial.begin(115200);
  pinMode(PHONE_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RADAR_PIN, INPUT_PULLDOWN);   // 선이 빠지면 사람 없음으로 읽히게
  pinMode(BUZZER_PIN, OUTPUT);

  Wire.begin(21, 22);     // 왼쪽 눈
  Wire1.begin(19, 23);    // 오른쪽 눈 (SDA=19, SCL=23)
  okL = eyeL.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  okR = eyeR.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (okL) eyeL.setRotation(2);   // OLED를 거꾸로(핀이 아래로) 달아서... 화면을 180도 돌림
  if (okR) eyeR.setRotation(2);
  if (!okL) Serial.println("ERR,OLED_LEFT (선 확인, 안 되면 주소를 0x3D로)");
  if (!okR) Serial.println("ERR,OLED_RIGHT (선 확인, 안 되면 주소를 0x3D로)");
  drawFace();
  PLAY(SND_BOOT);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // 재연결은 loop에서 5초마다 직접 (로그 도배 방지)

  // 일부 호환 보드는 MAC 주소가 00:00:00:00:00:00 으로 비어 있어서 공유기가 접속을 거절함.
  // 비어 있으면 우리가 정한 주소를 대신 씀 (기기가 2대 이상이면 마지막 숫자만 다르게).
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
    uint8_t myMac[6] = {0x02, 0x4E, 0x59, 0x41, 0x4E, 0x01};
    esp_wifi_set_mac(WIFI_IF_STA, myMac);
    Serial.println("MAC,비어 있어서 직접 설정함");
  }
  Serial.printf("MAC,%s\n", WiFi.macAddress().c_str());

  // 와이파이 접속 결과를 시리얼에 알려주기
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("WIFI,connected,ip=%s\n", WiFi.localIP().toString().c_str());
    } else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      int r = info.wifi_sta_disconnected.reason;
      const char* why = (r == 201) ? "와이파이 이름을 못 찾음 (이름 오타 또는 5GHz)" :
                        (r == 15 || r == 202 || r == 204 || r == 2) ? "비밀번호가 틀림" :
                        (r == 203) ? "공유기가 접속 거절 (MAC 주소 문제 등)" :
                        "기타";
      Serial.printf("WIFI,failed,reason=%d (%s)\n", r, why);
    }
  });

  // 주변 와이파이 목록 (2.4GHz만 가능)
  Serial.println("HELLO,NYANG");
  int n = WiFi.scanNetworks();
  Serial.printf("SCAN,%d개 찾음\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("SCAN,\"%s\"%s\n", WiFi.SSID(i).c_str(), WiFi.SSID(i) == WIFI_SSID ? "  <- 이거!" : "");
  }
  WiFi.scanDelete();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
  unsigned long now = millis();
  readSerial();
  updateSound();

  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > 5000) {   // 끊기면 5초마다 재연결
    lastWifiTry = now;
    WiFi.reconnect();
    Serial.println("WIFI,reconnecting");
  }

  if (now - lastRead >= 200) { lastRead = now; readSensors(); checkStudyStart(); checkAwayAlarm(); }
  if (sendNow || now - lastSend >= 1000) heartbeat();                 // 1초마다 + 바뀌면 즉시
  if (now - lastDraw >= 150) { lastDraw = now; drawFace(); }
}
