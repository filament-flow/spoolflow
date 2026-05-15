// ============================================================
//  SpoolFlow v5 – ESP32 NFC Filament Tracker
//  PN5180 Dual-Protocol: ISO 14443A (NTAG) + ISO 15693 (Prusa)
//  MW Service 3D | FilamentFlow Ecosystem
// ============================================================
//  Pinout:
//    PN5180 : NSS=5, BUSY=27, RST=16, SCK=18, MOSI=23, MISO=19
//    OLED   : SDA=21, SCL=22  (I2C 0x3C, SSD1306 128x64)
//    Tasten : BTN_L=32, BTN_R=33, BTN_ADD=25, BTN_OK=26
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <PN5180.h>
#include <PN5180ISO14443.h>
#include <PN5180ISO15693.h>

// ── PINS ─────────────────────────────────────────────────────
#define PIN_NSS       5
#define PIN_BUSY      27
#define PIN_RST       16
#define PIN_BTN_L     32
#define PIN_BTN_R     33
#define PIN_BTN_ADD   25
#define PIN_BTN_OK    26

// ── DISPLAY ──────────────────────────────────────────────────
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_H        64

// ── EINSTELLUNGEN ────────────────────────────────────────────
#define AP_SSID          "SpoolFlow-Setup"
#define AP_PASS          "spoolflow"
#define PREF_NS          "spoolflow"
#define API_TIMEOUT_MS   10000
#define STATE_RESET_MS   3500
#define NFC_DEBOUNCE_MS  1000


// ── STRUCTS (vor allen Funktionen!) ──────────────────────────

struct NfcResult {
  bool   fresh    = false;
  String tagId    = "";
  String protocol = "";
  String info     = "";
  String material = "";
  String brand    = "";
  String color    = "";
  String tagType  = "";
};

struct PrusaTag {
  String hwUid   = "";  // Hardware-UID (Fallback)
  String tagId   = "";  // ID aus URL-Record (z.B. "4d7f10bc58")
  String material= "";  // CBOR Key 10
  String color   = "";  // Farbe aus CBOR
  bool   valid   = false;
};

struct NtagResult {
  String tagId   = "";
  String material= "";
  String brand   = "";
  String color   = "";
  int    minTemp = 0;
  int    maxTemp = 0;
  String tagType = "";   // "filamentflow"|"anycubic"|"openspool"|"unknown"
};

enum AppState {
  IDLE,
  SCANNING,        // /nfc/scan laeuft
  NOT_LINKED,      // Tag unbekannt -> In App verknuepfen
  BOOKING_INPUT,   // Gramm eingeben (1/10/100g Tasten)
  BOOKING,         // API-Call laeuft
  STATE_SUCCESS,
  STATE_ERROR
};

// ── GLOBALE OBJEKTE ──────────────────────────────────────────

Adafruit_SSD1306  display(OLED_W, OLED_H, &Wire, -1);
PN5180ISO14443    nfc14443(PIN_NSS, PIN_BUSY, PIN_RST);
PN5180ISO15693    nfc15693(PIN_NSS, PIN_BUSY, PIN_RST);
WebServer         server(80);
Preferences       prefs;
// kein Mutex nötig (single-thread)

String cfg_ssid    = "";
String cfg_pass    = "";
String cfg_api_key = "";
String cfg_email   = "";
bool   configMode  = false;

NfcResult nfcShared;
static bool nfcLastPresent = false;  // global statt lokal im Task

AppState appState     = IDLE;
int      bookingGrams = 0;
unsigned long btnHoldStart = 0;
String   activeTagId  = "";
String   activeInfo   = "";
unsigned long stateTimer  = 0;
unsigned long lastTagTime = 0;

// ── DISPLAY ───────────────────────────────────────────────────

void dispHeader(const char* title) {
  display.setTextColor(WHITE); display.setTextSize(1);
  display.setCursor(0, 0);  display.print("SpoolFlow v5");
  display.setCursor(84, 0); display.print(WiFi.status() == WL_CONNECTED ? "[WiFi]" : "[----]");
  display.drawLine(0, 9, 127, 9, WHITE);
  display.setCursor(0, 12); display.print(title);
}
void showIdle() {
  display.clearDisplay(); dispHeader("Bereit");
  display.setCursor(0, 28); display.print("Spule ans Lesegeraet");
  display.setCursor(0, 38); display.print("halten..."); display.display();
}
void showTagFound(const String& proto, const String& info) {
  display.clearDisplay(); dispHeader("Tag erkannt!");
  display.setCursor(0, 22); display.print("Prot: " + proto);
  display.setCursor(0, 34); display.print(info.substring(0, 20));
  display.display();
}
void showBookingInput(int grams, const String& info) {
  display.clearDisplay(); dispHeader("Buchung");
  // Info-Zeile (Filament-Name)
  display.setTextSize(1);
  display.setCursor(0, 12); display.print(info.substring(0,21));
  // Grosser Gramm-Wert
  display.setTextSize(2);
  String gStr = String(grams) + "g";
  int gx = max(0, (128 - (int)gStr.length() * 12) / 2);
  display.setCursor(gx, 26);
  display.print(gStr);
  // Tasten-Leiste unten
  display.setTextSize(1);
  display.drawLine(0, 52, 127, 52, WHITE);
  display.setCursor(0,  55); display.print("+100");
  display.setCursor(36, 55); display.print("+10");
  display.setCursor(72, 55); display.print("+1");
  display.setCursor(110,55); display.print("OK");
  display.display();
}
void showScanning() {
  display.clearDisplay(); dispHeader("Pruefe Tag...");
  display.setCursor(10, 32); display.print("Bitte warten..."); display.display();
}
void showNotLinked(const String& mat) {
  display.clearDisplay(); dispHeader("Nicht verknuepft");
  display.setCursor(0, 22); display.print("In App verknuepfen:");
  display.setCursor(0, 32); display.print("NFC Tags -> Link");
  if (mat.length() > 0) { display.setCursor(0, 46); display.print(mat.substring(0,20)); }
  display.setCursor(0, 57); display.print("[OK] Zurueck");
  display.display();
}
void showBooking() {
  display.clearDisplay(); dispHeader("Sende...");
  display.setCursor(20, 32); display.print("Bitte warten..."); display.display();
}
void showResult(bool ok, const String& l1, const String& l2 = "") {
  display.clearDisplay(); dispHeader(ok ? "Erfolg!" : "Fehler");
  display.setCursor(0, 24); display.print(l1);
  if (l2.length()) { display.setCursor(0, 34); display.print(l2); } display.display();
}

// ── HILFSFUNKTIONEN ──────────────────────────────────────────

String bytesToHex(uint8_t* b, int len) {
  String s = "";
  for (int i = 0; i < len; i++) {
    if (b[i] < 0x10) s += "0";
    s += String(b[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// ── ISO 14443A: NTAG ID aus NDEF lesen ───────────────────────
// Statischer Puffer – kein Stack-Verbrauch

static uint8_t s_ntagBuf[256];
static uint8_t s_uid15[8];
static uint8_t s_raw15[320];

String extractIdFromUrl(const String& url) {
  int idx = url.indexOf("?id=");
  if (idx < 0) idx = url.indexOf("&id=");
  if (idx < 0) return "";
  String id = url.substring(idx + 4);
  int a = id.indexOf('&'); if (a >= 0) id = id.substring(0, a);
  id.trim(); return id;
}


// Magic Byte 0x7B an Page 4, Rohdaten: Pages 10-19=Brand/Material, 20=Color ABGR, 24=Temps
// ── ISO 14443A: NTAG lesen ────────────────────────────────────
// Liest FilamentFlow-NDEF-ID falls vorhanden, sonst UID als Fallback
// Anycubic/Bambu/unbekannte Tags: UID wird zurückgegeben (reicht für Tracking)

NtagResult readNTAGfull() {
  NtagResult res; res.tagType = "unknown";
  int rawLen = 0;
  memset(s_ntagBuf, 0, 256);

  // Seiten lesen (crash-sicher, NULL-Check)
  unsigned long tStart = millis();
  for (int p = 4; p < 68 && rawLen < 252; p++) {
    if (millis() - tStart > 400) break;  // Max 400ms
    uint8_t cmd[2] = {0x30, (uint8_t)p};
    if (!nfc14443.sendData(cmd, 2)) break;
    delay(5);
    uint8_t* r = nfc14443.readData(16);
    if (!r) break;
    int copy = min(16, 252 - rawLen);
    memcpy(s_ntagBuf + rawLen, r, copy);
    rawLen += copy;
    bool term = false;
    for (int j = max(0, rawLen-16); j < rawLen; j++) if (s_ntagBuf[j] == 0xFE) { term=true; break; }
    if (term) break;
  }
  if (rawLen < 4) return res;

  // NDEF TLV suchen (FilamentFlow-Tags haben 0x03-Block)
  int i = 0;
  while (i < rawLen - 2) {
    if (i >= rawLen) break;
    uint8_t tt = s_ntagBuf[i++];
    if (tt == 0xFE) break;
    if (tt == 0x00) continue;
    if (i >= rawLen) break;
    uint16_t tl = s_ntagBuf[i++];
    if (tl == 0xFF) { if (i + 1 >= rawLen) break; tl = ((uint16_t)s_ntagBuf[i] << 8) | s_ntagBuf[i+1]; i += 2; }
    if (tt != 0x03) { if (i + tl <= rawLen) i += tl; else break; continue; }
    if (i + tl > rawLen) break;
    int ne = i + tl;
    while (i < ne - 2) {
      if (i + 2 > ne) break;
      uint8_t fl = s_ntagBuf[i++];
      bool sr = (fl & 0x10); bool il = (fl & 0x08); bool ME = (fl >> 6) & 1;
      uint8_t tlen = s_ntagBuf[i++];
      uint32_t pl;
      if (sr) { if(i>=ne)break; pl = s_ntagBuf[i++]; }
      else { if(i+3>=ne)break; pl=((uint32_t)s_ntagBuf[i]<<24)|((uint32_t)s_ntagBuf[i+1]<<16)|((uint32_t)s_ntagBuf[i+2]<<8)|s_ntagBuf[i+3]; i+=4; }
      if (il) { if(i>=ne)break; i++; }
      if (i + tlen > ne) break;
      uint8_t tnf = fl & 0x07;
      char tc = (tlen > 0) ? (char)s_ntagBuf[i] : 0;
      i += tlen;
      if (i + (int)pl > ne) break;
      const uint8_t* pay = s_ntagBuf + i;
      // URL-Record → FilamentFlow-ID
      if (tnf == 1 && tc == 'U' && pl > 1) {
        String u = "";
        for (uint32_t q = 1; q < min(pl, (uint32_t)100); q++) u += (char)pay[q];
        String id = extractIdFromUrl(u);
        if (id.length() > 0) { res.tagId = id; res.tagType = "filamentflow"; }
      }
      // Text-Record → 3. Zeile = FilamentFlow-ID
      if (tnf == 1 && tc == 'T' && pl > 1) {
        uint8_t ll = pay[0] & 0x3F;
        String txt = "";
        for (uint32_t q = 1+ll; q < min(pl,(uint32_t)100); q++) txt += (char)pay[q];
        int n1 = txt.indexOf('\n');
        if (n1 >= 0) { int n2 = txt.indexOf('\n', n1+1); if (n2 >= 0) { String id = txt.substring(n2+1); id.trim(); if (id.length()) res.tagId = id; } }
      }
      i += (int)pl;
      if (ME) break;
    }
    i = ne; break;
  }
  return res;
}


// ── CBOR Rohdaten-Scanner (kein Rekursion, crash-sicher) ──────
// Liest einen CBOR-uint-Key, gibt den Key-Wert zurück, off wird vorwärtsbewegt
// Gibt -1 zurück wenn kein uint-Key
static int cborNextUintKey(const uint8_t* b, int len, int& off) {
  if (off >= len) return -1;
  uint8_t ini = b[off]; uint8_t maj = ini>>5; uint8_t ai = ini&0x1F;
  if (maj != 0) return -1;  // Nur uint-Keys
  off++;
  if (ai <= 23) return (int)ai;
  if (ai == 24 && off < len) return (int)b[off++];
  if (ai == 25 && off+1 < len) { int v=((int)b[off]<<8)|b[off+1]; off+=2; return v; }
  return -1;
}

// Liest Text-String, gibt "" wenn kein Text
static String cborNextText(const uint8_t* b, int len, int& off) {
  if (off >= len) return "";
  uint8_t ini = b[off]; uint8_t maj = ini>>5; uint8_t ai = ini&0x1F;
  if (maj != 3) return "";
  off++;
  uint32_t slen = 0;
  if      (ai <= 23) slen = ai;
  else if (ai == 24 && off < len)   slen = b[off++];
  else if (ai == 25 && off+1 < len) { slen=((uint16_t)b[off]<<8)|b[off+1]; off+=2; }
  if (off + (int)slen > len) return "";
  String s = "";
  for (uint32_t i = 0; i < slen; i++) s += (char)b[off+i];
  off += (int)slen;
  return s;
}

// Überspringt einen CBOR-Wert OHNE Rekursion
// Funktioniert nur für flache Werte (uint, int, text, bytes)
// Für Container: überspringt den Header und geht zum nächsten Byte
static int cborSkipValue(const uint8_t* b, int len, int off) {
  if (off >= len) return len;
  uint8_t ini = b[off]; uint8_t maj = ini>>5; uint8_t ai = ini&0x1F;
  off++;
  if (ai == 31) return off;  // Indefinite start/break – nur Header
  uint32_t val = 0;
  if      (ai <= 23) val = ai;
  else if (ai == 24 && off < len)   { val = b[off++]; }
  else if (ai == 25 && off+1 < len) { val = ((uint16_t)b[off]<<8)|b[off+1]; off+=2; }
  else if (ai == 26 && off+3 < len) {
    val=((uint32_t)b[off]<<24)|((uint32_t)b[off+1]<<16)|((uint32_t)b[off+2]<<8)|b[off+3]; off+=4;
  }
  else if (ai == 27 && off+7 < len) { off+=8; val=0; }
  if (maj == 2 || maj == 3) {
    off += (int)val;
    if (off > len) off = len;
  }
  return off;
}

// Parst CBOR-Payload – linearer Scan, kein Stack-Risiko
// Wir kennen die Struktur: A1 02 18D2 | BF | Key Val Key Val ... FF
void parseCBORPayload(const uint8_t* pay, int plen, PrusaTag& res) {
  int off = 0;
  if (off >= plen) return;

  // Meta-Section überspringen (map mit 1 Paar: key=2, val=uint)
  // Format: A1 02 18 D2 (4 Bytes)
  if ((pay[0]>>5)==5) {
    // Map-Header lesen
    uint8_t ai = pay[0]&0x1F; off++;
    uint32_t pairs = 0;
    if      (ai <= 23) pairs = ai;
    else if (ai == 24 && off < plen) pairs = pay[off++];
    // Jedes Paar überspringen (key + value)
    for (uint32_t p = 0; p < pairs && off < plen && p < 4; p++) {
      off = cborSkipValue(pay, plen, off);  // Key
      off = cborSkipValue(pay, plen, off);  // Value
    }
  }

  // Indefinite Map suchen (0xBF)
  // Wenn nicht direkt, bis zu 8 Bytes vorwärts suchen
  int searchEnd = min(off + 8, plen);
  while (off < searchEnd && pay[off] != 0xBF) off++;
  if (off >= plen || pay[off] != 0xBF) return;
  off++;  // BF überspringen

  // Key-Value Paare linear scannen (max 64 Paare)
  for (int pair = 0; pair < 64 && off < plen; pair++) {
    if (pay[off] == 0xFF) break;  // Break code

    // Key: muss uint sein
    int keyOff = off;
    int key = cborNextUintKey(pay, plen, off);
    if (key < 0) {
      // Kein uint-Key – überspringen und weitermachen
      off = cborSkipValue(pay, plen, keyOff);
      off = cborSkipValue(pay, plen, off);
      continue;
    }

    // Value abhängig vom Key behandeln
    if (off >= plen) break;
    uint8_t vMaj = pay[off] >> 5;
    uint8_t vAi  = pay[off] & 0x1F;

    if (vMaj == 3) {
      // Text-String
      String val = cborNextText(pay, plen, off);
      if (key == 5  && res.tagId.length()   == 0) res.tagId   = val;
      if (key == 10 && res.material.length() == 0) res.material = val;
      // Weitere String-Felder die interessant sein könnten
    }
    else if (vMaj == 4 && key == 6 && res.color.length() == 0) {
      // Array → Farbe [R, G, B, A]
      int ao = off + 1;  // Array-Header überspringen
      int rgb[3] = {-1,-1,-1};
      for (int ci = 0; ci < 3 && ao < plen; ci++) {
        uint8_t bv = pay[ao];
        if      (bv <= 23)                  { rgb[ci] = bv;       ao++; }
        else if (bv == 0x18 && ao+1 < plen) { rgb[ci] = pay[ao+1]; ao+=2; }
        else break;
      }
      if (rgb[0]>=0 && rgb[1]>=0 && rgb[2]>=0) {
        char cb[8]; snprintf(cb, sizeof(cb), "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
        res.color = String(cb);
        Serial.println("Prusa Farbe: " + res.color);
      }
      off = cborSkipValue(pay, plen, off);
    }
    else {
      // Alle anderen Werte überspringen
      int newOff = cborSkipValue(pay, plen, off);
      if (newOff <= off) { off++; }  // Sicherheit: immer vorwärts
      else off = newOff;
    }

    // Wenn wir schon alle drei Felder haben – fertig
    if (res.tagId.length() > 0 && res.material.length() > 0 && res.color.length() > 0) break;
  }
}


PrusaTag readPrusaTag() {
  PrusaTag res;
  memset(s_uid15,0,8);
  if (nfc15693.getInventory(s_uid15)!=ISO15693_EC_OK) return res;
  bool allZero=true; for(int i=0;i<8;i++) if(s_uid15[i]!=0){allZero=false;break;}
  if (allZero) return res;
  res.hwUid=bytesToHex(s_uid15,8);

  // Blöcke lesen
  memset(s_raw15,0,sizeof(s_raw15)); int tb=0;
  unsigned long blockStart = millis();
  for(int b=0;b<30;b++){
    // Gesamt-Timeout: max 500ms fuer alle Bloecke
    if (millis() - blockStart > 500) {
      Serial.println("Block-Timeout!");
      break;
    }
    uint8_t blk[4];
    ISO15693ErrorCode brc = nfc15693.readSingleBlock(s_uid15,b,blk,4);
    if(brc!=ISO15693_EC_OK) break;
    memcpy(s_raw15+b*4,blk,4); tb=(b+1)*4;
    delay(2);
  }
  if(tb<8){res.valid=true;return res;}

  // TLV ab Byte 4
  int i=4;
  while(i<tb-2){
    uint8_t t=s_raw15[i++]; if(t==0xFE)break; if(t==0x00)continue;
    int l; if(s_raw15[i]==0xFF&&i+2<tb){l=((int)s_raw15[i+1]<<8)|s_raw15[i+2];i+=3;}else{l=s_raw15[i++];}
    if(t!=0x03){i+=l;continue;}
    // NDEF Message: beide Records durchlaufen
    const uint8_t* msg=s_raw15+i; int mlen=l; int pos=0;
    while(pos<mlen){
      uint8_t fl=msg[pos++];
      bool ME=(fl>>6)&1, SR=(fl>>4)&1, IL=(fl>>3)&1; uint8_t tnf=fl&7;
      uint8_t tl=msg[pos++];
      uint32_t pl; if(SR){pl=msg[pos++];}else{pl=((uint32_t)msg[pos]<<24)|((uint32_t)msg[pos+1]<<16)|((uint32_t)msg[pos+2]<<8)|msg[pos+3];pos+=4;}
      if(IL)pos++;
      // Typ lesen
      String rType=""; for(uint8_t q=0;q<tl;q++) rType+=(char)msg[pos+q]; pos+=tl;
      const uint8_t* pay=msg+pos;
      // Record 1: URL → Tag-ID extrahieren
      if(tnf==1 && rType=="U" && pl>1){
        String urlBody=""; for(uint32_t q=1;q<pl;q++) urlBody+=(char)pay[q];
        int sl=urlBody.lastIndexOf('/');
        if(sl>=0) res.tagId=urlBody.substring(sl+1);
        Serial.println("OPT URL: " + urlBody + " -> ID: " + res.tagId);
      }
      // Record 2: MIME → CBOR dekodieren
      if(tnf==2 && pl>0){
        parseCBORPayload(pay,(int)pl,res);
        Serial.println("OPT Material: " + res.material);
      }
      pos+=(int)pl;
      if(ME) break;
    }
    res.valid=true; break;
  }
  return res;
}

// ── NFC-TASK (FreeRTOS Core 0) ───────────────────────────────

// NFC einmalig initialisieren
void nfcInit() {
  Serial.println("NFC Init...");
  nfc14443.begin(); delay(100);
  nfc15693.begin(); delay(100);
  nfc14443.reset(); delay(200);
  Serial.println("NFC bereit");
}

// NFC-Scan: wird direkt aus loop() aufgerufen (kein separater Task)
void pollNFC() {
  bool& lastPresent = nfcLastPresent;
    // ── ISO 14443A ──────────────────────────────────────────
    nfc14443.reset(); delay(30);
    nfc14443.setupRF(); delay(20);

    uint8_t uid14[10];
    uint8_t ul = nfc14443.readCardSerial(uid14);

    if (ul > 0) {
      Serial.print("14443A UID: "); Serial.println(bytesToHex(uid14, ul));
      if (!lastPresent) {
        nfc14443.reset(); delay(30); nfc14443.setupRF(); delay(20);
        NtagResult nr = readNTAGfull();
        String useId = nr.tagId.length() > 0 ? nr.tagId : bytesToHex(uid14, ul);
        String info  = nr.tagType == "filamentflow" ? "FilamentFlow" : "NTAG";
        Serial.println("14443A ID=" + useId + " Type=" + nr.tagType + " Mat=" + nr.material);
        if (true) {
          nfcShared.fresh    = true;
          nfcShared.tagId    = useId;
          nfcShared.protocol = "14443A";
          nfcShared.info     = info;
          nfcShared.material = nr.material;
          nfcShared.brand    = nr.brand;
          nfcShared.color    = nr.color;
          nfcShared.tagType  = nr.tagType;
        }
        lastPresent = true;
      }
      nfc14443.reset(); delay(50);
      nfcLastPresent = true; return;
    }

    // Reset zwischen Protokollen – yield damit IDLE laeuft
    nfc14443.reset();
    delay(50);

    // ── ISO 15693 (OpenPrintTag) ────────────────────────────
    // Schritt 1: Nur UID prüfen (schnell, kein Block-Lesen)
    nfc15693.reset();
    delay(50);
    nfc15693.setupRF();
    delay(30);

    uint8_t uid15check[8]; memset(uid15check, 0, 8);
    bool tag15Present = (nfc15693.getInventory(uid15check) == ISO15693_EC_OK);
    // Falsch-Positiv prüfen
    if (tag15Present) {
      bool allZero = true;
      for (int i=0;i<8;i++) if(uid15check[i]!=0){allZero=false;break;}
      if (allZero) tag15Present = false;
    }

    nfc15693.reset();
    delay(50);

    if (tag15Present) {
      if (!lastPresent) {
        // Schritt 2: Nur beim ERSTEN Detect Blöcke lesen + CBOR parsen
        nfc15693.reset();
        delay(30);
        nfc15693.setupRF();
        delay(20);

        PrusaTag pt = readPrusaTag();
        nfc15693.reset();
        delay(30);

        String useId = pt.tagId.length() > 0 ? pt.tagId : bytesToHex(uid15check, 8);
        Serial.println("15693 ID: " + useId + " | Mat: " + pt.material);

        if (true) {
          nfcShared.fresh    = true;
          nfcShared.tagId    = useId;
          nfcShared.protocol = "15693";
          nfcShared.info     = pt.material.length()>0 ? pt.material.substring(0,16) : "Prusa";
          nfcShared.material = pt.material;
          nfcShared.brand    = "Prusament";
          nfcShared.color    = pt.color;
          nfcShared.tagType  = "openprinttag";
          
        }
        lastPresent = true;
      }
      nfcLastPresent = true; return;
    }

    if (lastPresent) { Serial.println("Tag weg."); lastPresent = false; }
}

// ── API ───────────────────────────────────────────────────────

bool doBooking(const String& tagId, int amount, bool isReturn) {
  if (WiFi.status() != WL_CONNECTED) return false;
  DynamicJsonDocument doc(256);
  doc["user_email"]     = cfg_email;
  doc["nfc_tag_id"]     = tagId;
  doc["consume_amount"] = amount;
  if (isReturn) doc["action"] = "return";
  doc["return_type"] = "grams";
  doc["return_amount"] = amount;
  String body; serializeJson(doc, body);

  WiFiClientSecure sc; sc.setInsecure();
  HTTPClient http; http.setTimeout(API_TIMEOUT_MS);
  http.begin(sc, String("https://filament-flow.com/api/nfc/booking"));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", cfg_api_key);
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "{\"error\":\"HTTP\"}";
  Serial.println("API -> " + String(code));
  Serial.println("Response: " + resp.substring(0, 200));
  http.end(); sc.stop(); delay(200);

  if (code == 200) return true;
  StaticJsonDocument<256> rd;
  if (deserializeJson(rd, resp)) return false;
  return !rd.containsKey("error");
}

// ── NFC SCAN (Registrierung + Status-Check) ──────────────────

// Rueckgabe: "known" | "new" | "unregistered" | "error"
String doScan(const String& tagId, const String& material, const String& brand, const String& color = "", const String& tagType = "") {
  if (WiFi.status() != WL_CONNECTED) return "error";
  DynamicJsonDocument doc(512);
  doc["uid"]        = tagId;
  doc["user_email"] = cfg_email;
  if (material.length() > 0) doc["material"]  = material;
  if (brand.length()   > 0) doc["brand"]     = brand;
  if (color.length()   > 0) doc["color_hex"] = color;
  if (tagType.length() > 0) doc["ndef_type"]  = tagType;
  doc["device_id"]  = "spoolflow-v5";
  String body; serializeJson(doc, body);

  WiFiClientSecure sc; sc.setInsecure();
  HTTPClient http; http.setTimeout(API_TIMEOUT_MS);
  http.begin(sc, String("https://filament-flow.com/api/nfc/scan"));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", cfg_api_key);
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "{\"error\":\"HTTP\"}";
  Serial.println("SCAN -> " + String(code) + " " + resp.substring(0,120));
  http.end(); sc.stop(); delay(200);

  // HTTP 403 = Lizenz fehlt
  if (code == 403) return "license_error";

  StaticJsonDocument<512> rd;
  if (deserializeJson(rd, resp)) return "error";
  String status = rd["status"] | "error";
  return status;
}

// ── KONFIGURATION ─────────────────────────────────────────────

void loadConfig() {
  prefs.begin(PREF_NS, true);
  cfg_ssid    = prefs.getString("ssid",   "");
  cfg_pass    = prefs.getString("pass",   "");
  cfg_api_key = prefs.getString("apikey", "");
  cfg_email   = prefs.getString("email",  "");
  prefs.end();
}
void saveConfig(const String& s, const String& p, const String& k, const String& e) {
  prefs.begin(PREF_NS, false);
  prefs.putString("ssid",s); prefs.putString("pass",p);
  prefs.putString("apikey",k); prefs.putString("email",e);
  prefs.end();
}

// ── WEBSERVER ─────────────────────────────────────────────────

const char* SETUP_HTML = R"(<!DOCTYPE html><html><head>
<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>SpoolFlow Setup</title>
<style>body{font-family:Arial;background:#1a1a2e;color:#eee;padding:20px;margin:0}
.card{background:#16213e;border-radius:12px;padding:24px;max-width:480px;margin:0 auto}
h1{color:#00d4ff;text-align:center}
label{display:block;margin:10px 0 4px;color:#aaa;font-size:14px}
input{width:100%;padding:10px;border-radius:8px;border:1px solid #333;background:#0f3460;color:#fff;box-sizing:border-box}
button{width:100%;padding:12px;background:#00d4ff;color:#000;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;margin-top:16px}
.hint{background:#0f3460;border-radius:8px;padding:10px;font-size:12px;color:#aaa;margin:10px 0}
</style></head><body><div class='card'><h1>SpoolFlow v5</h1>
<div class='hint'>API Key + E-Mail aus FilamentFlow App: Einstellungen -&gt; SpoolFlow</div>
<form action='/save' method='POST'>
<label>WLAN Name</label><input name='ssid' required>
<label>WLAN Passwort</label><input name='pass' type='password'>
<label>ESP32 API Key (aus FilamentFlow App: Einstellungen)</label><input name='apikey' required>
<label>E-Mail Adresse</label><input name='email' type='email' required>
<button type='submit'>Speichern &amp; Starten</button>
</form></div></body></html>)";

void startSetupMode() {
  configMode = true;
  display.clearDisplay(); dispHeader("Setup-Modus");
  display.setCursor(0, 18); display.print("WLAN: SpoolFlow-Setup");
  display.setCursor(0, 28); display.print("Pass: spoolflow");
  display.setCursor(0, 40); display.print("http://192.168.4.1");
  display.display();
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", []() { server.send(200, "text/html", SETUP_HTML); });
  server.on("/save", HTTP_POST, []() {
    saveConfig(server.arg("ssid"), server.arg("pass"),
               server.arg("apikey"), server.arg("email"));
    server.send(200, "text/html",
      "<h2 style='color:#00ff88;font-family:Arial;text-align:center;padding:40px'>Gespeichert!</h2>");
    delay(2000); ESP.restart();
  });
  server.begin();
}

// ── TASTEN ────────────────────────────────────────────────────

bool btnPressed(int pin) {
  if (digitalRead(pin) != LOW) return false;
  delay(25); return digitalRead(pin) == LOW;
}
void waitRelease(int pin) { while (digitalRead(pin) == LOW) delay(10); delay(20); }

// ── SETUP ─────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n=========================");
  Serial.println("  SpoolFlow v5 - PN5180");
  Serial.println("=========================");

  pinMode(PIN_BTN_L,   INPUT_PULLUP);
  pinMode(PIN_BTN_R,   INPUT_PULLUP);
  pinMode(PIN_BTN_ADD, INPUT_PULLUP);
  pinMode(PIN_BTN_OK,  INPUT_PULLUP);

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED Fehler!"); for (;;) delay(1000);
  }
  display.setTextColor(WHITE); display.clearDisplay();
  dispHeader("Starte..."); display.display();
  loadConfig();
  delay(800);

  if (digitalRead(PIN_BTN_OK) == LOW) { startSetupMode(); return; }
  if (cfg_ssid.length() == 0)        { startSetupMode(); return; }

  display.clearDisplay(); dispHeader("WLAN...");
  display.setCursor(0, 22); display.print(cfg_ssid); display.display();
  WiFi.mode(WIFI_STA); WiFi.begin(cfg_ssid.c_str(), cfg_pass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) { delay(500); Serial.print("."); tries++; }
  if (WiFi.status() != WL_CONNECTED) { startSetupMode(); return; }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());
  Serial.println("Heap: " + String(ESP.getFreeHeap()));

  nfcInit();
  showIdle();
}

// ── LOOP (Core 1) ─────────────────────────────────────────────

void loop() {
  if (configMode) { server.handleClient(); return; }

  unsigned long now = millis();

  // NFC nur im IDLE-State scannen (sonst traege Menues)
  if (appState == IDLE) {
    pollNFC();
  }

  if (appState == IDLE) {
    if (nfcShared.fresh && now - lastTagTime > NFC_DEBOUNCE_MS) {
        activeTagId        = nfcShared.tagId;
        String proto       = nfcShared.protocol;
        String info        = nfcShared.info;
        String material    = nfcShared.material;
        String brand       = nfcShared.brand;
        nfcShared.fresh    = false;
        
        lastTagTime = now;

        // Tag-Info kurz anzeigen
        showTagFound(proto, info); delay(400);

        // /nfc/scan aufrufen
        showScanning();
        String color   = nfcShared.color;
        String tagType = nfcShared.tagType;
        String scanStatus = doScan(activeTagId, material, brand, color, tagType);
        Serial.println("Scan-Status: " + scanStatus);

        if (scanStatus == "known" || scanStatus == "migrated") {
          // Tag verknuepft -> direkt zur Gramm-Eingabe
          bookingGrams = 0;
          activeInfo   = info;
          appState = BOOKING_INPUT;
          showBookingInput(bookingGrams, activeInfo);
        } else if (scanStatus == "new" || scanStatus == "unregistered") {
          // Tag nicht verknuepft -> Hinweis anzeigen
          appState = NOT_LINKED;
          showNotLinked(material);
          stateTimer = millis();
        } else if (scanStatus == "license_error") {
          // Lizenz fehlt
          showResult(false, "Lizenz fehlt!", "SF/BN Key noetig");
          appState = STATE_ERROR; stateTimer = millis();
        } else {
          // Fehler (kein WiFi etc.)
          showResult(false, "Scan-Fehler", "WiFi pruefen");
          appState = STATE_ERROR; stateTimer = millis();
        }
    } else {
      if (nfcShared.fresh) nfcShared.fresh = false;
    }
    return;
  }

  // NOT_LINKED: OK-Taste -> zurueck, Auto-Reset nach 10s
  if (appState == NOT_LINKED) {
    if (btnPressed(PIN_BTN_OK)) { waitRelease(PIN_BTN_OK); appState=IDLE; showIdle(); }
    if (millis() - stateTimer > 10000) { appState=IDLE; nfcLastPresent=false; showIdle(); }
    return;
  }

  if (appState == BOOKING_INPUT) {
    // +100g
    if (btnPressed(PIN_BTN_L)) {
      waitRelease(PIN_BTN_L);
      bookingGrams += 100;
      showBookingInput(bookingGrams, activeInfo);
      return;
    }
    // +10g
    if (btnPressed(PIN_BTN_R)) {
      waitRelease(PIN_BTN_R);
      bookingGrams += 10;
      showBookingInput(bookingGrams, activeInfo);
      return;
    }
    // +1g
    if (btnPressed(PIN_BTN_ADD)) {
      waitRelease(PIN_BTN_ADD);
      bookingGrams += 1;
      showBookingInput(bookingGrams, activeInfo);
      return;
    }
    // OK: kurz = Buchen, lang (>1.5s) = Abbrechen
    if (digitalRead(PIN_BTN_OK) == LOW) {
      if (btnHoldStart == 0) btnHoldStart = millis();
      if (millis() - btnHoldStart > 1500) {
        waitRelease(PIN_BTN_OK);
        btnHoldStart = 0; bookingGrams = 0;
        appState = IDLE; nfcLastPresent=false; showIdle();
      }
      return;
    } else if (btnHoldStart > 0) {
      // Kurzer Druck losgelassen
      btnHoldStart = 0;
      if (bookingGrams <= 0) return;  // Nichts eingegeben
      appState = BOOKING; showBooking();
      bool ok = doBooking(activeTagId, bookingGrams, false);
      if (ok) showResult(true,  "Gebucht!", "-" + String(bookingGrams) + "g");
      else    showResult(false, "Buchung fehl-", "geschlagen!");
      appState = ok ? STATE_SUCCESS : STATE_ERROR;
      stateTimer = millis(); bookingGrams = 0;
    }
    return;
  }

  if ((appState==STATE_SUCCESS||appState==STATE_ERROR) && millis()-stateTimer>STATE_RESET_MS) {
    appState=IDLE; nfcLastPresent=false; showIdle();
  }
}
