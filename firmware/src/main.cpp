// CYD (ESP32-2432S028R): AP + captive web form. Screen shows two QR codes
// (1: join the WiFi, 2: open the page) with the submitted text below them.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <freertos/semphr.h>
#include <DNSServer.h>
#include <TFT_eSPI.h>
#include <qrcode.h>
#include <esp_wifi.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

static const char *AP_SSID = "CYD-Display";
static const char *AP_PASS = nullptr;        // open AP; set 8+ chars (no ; , : \ ") for WPA2
static const size_t MAX_LEN = 140;
static const int MAX_MSGS = 20;

TFT_eSPI tft;
#ifndef CHAT_HOST
#define CHAT_HOST "chat.example.com"            // set in platformio.ini; must match the certificate. The board's DNS answers it with the AP IP
#endif
static const char LOC[] = "https://" CHAT_HOST "/";
// Certificate chain + key, embedded from certs/*.pem (see platformio.ini). NUL-terminated by embed_txtfiles.
extern const uint8_t cert_start[] asm("_binary_certs_fullchain_pem_start");
extern const uint8_t cert_end[]   asm("_binary_certs_fullchain_pem_end");
extern const uint8_t key_start[]  asm("_binary_certs_key_pem_start");
extern const uint8_t key_end[]    asm("_binary_certs_key_pem_end");
// HTTP handlers run on the httpd task, the loop on another: the log is guarded, and only loop() draws.
SemaphoreHandle_t mtx;
struct Lock { Lock() { xSemaphoreTake(mtx, portMAX_DELAY); } ~Lock() { xSemaphoreGive(mtx); } };
volatile bool needRender = false;
DNSServer dns;
struct Msg { String nick; uint16_t col; String text; };
Msg msgs[MAX_MSGS]; int nMsgs = 0;
// Touch calibration (raw XPT2046 -> screen px, landscape). Adjust if taps land off-target.
#define TS_X0 200
#define TS_X1 3700
#define TS_Y0 240
#define TS_Y1 3800
static const uint32_t PAL[8] = {0xff4040, 0xff9020, 0xffe030, 0x40e060, 0x30d0ff, 0x6080ff, 0xc060ff, 0xff60c0};
bool kbOpen = false, kbShift = false, kbEditNick = false;
String kbNick = "screen", kbText;
int kbCol = 3;
// Per-client colour slots: slot i is "taken" while its owner (by MAC) is connected.
struct Slot { uint8_t mac[6]; bool used; } slots[8];
volatile int selected = 0;               // colour the NEXT client to join receives
volatile bool uiDirty = false;           // set from the WiFi event task, handled in loop()
int kickArmed = -1; uint32_t kickArmedAt = 0;
struct Ban { uint8_t mac[6]; uint32_t until; } bans[4];
static const uint32_t BAN_MS = 60000;
static uint16_t pal565(int i);
bool showQR = true;                     // hides while a client is connected; tap toggles manually

// CYD touch controller sits on its own SPI bus (VSPI-style pins).
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(33, 36);          // CS, IRQ

// Draw `text` as a QR (version 2-4, 4 px/module) on a white panel with top-left (x,y).
static const int PX = 4, PANEL = 132;
static void drawQR(int x, int y, const String &text, const char *label) {
  QRCode qr; uint8_t *buf = nullptr;
  for (int ver = 2; ver <= 4 && !buf; ver++) {
    buf = (uint8_t *)malloc(qrcode_getBufferSize(ver));
    if (qrcode_initText(&qr, buf, ver, ECC_LOW, text.c_str()) != 0) { free(buf); buf = nullptr; }
  }
  tft.fillRect(x, y, PANEL, PANEL, TFT_WHITE);
  if (buf) {
    int side = qr.size * PX, ox = x + (PANEL - side) / 2, oy = y + (PANEL - side) / 2;
    for (int j = 0; j < qr.size; j++)
      for (int i = 0; i < qr.size; i++)
        if (qrcode_getModule(&qr, i, j)) tft.fillRect(ox + i * PX, oy + j * PX, PX, PX, TFT_BLACK);
    free(buf);
  }
  tft.setTextSize(1); tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(x + (PANEL - strlen(label) * 6) / 2, y + PANEL + 4);
  tft.print(label);
}

// Greedy word wrap; honours \n. Returns line count (lines beyond maxLines are counted, not stored).
static int wrap(const String &text, int cols, String *out, int maxLines) {
  int n = 0; String line, word;
  auto flushLine = [&]() { if (n < maxLines) out[n] = line; n++; line = ""; };
  auto flushWord = [&]() {
    while ((int)word.length() > cols) {
      if (line.length()) flushLine();
      line = word.substring(0, cols); flushLine(); word = word.substring(cols);
    }
    if (!word.length()) return;
    if (line.length() && (int)(line.length() + 1 + word.length()) > cols) flushLine();
    if (line.length()) line += ' ';
    line += word; word = "";
  };
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '\r') continue;
    if (c == '\n') { flushWord(); flushLine(); } else if (c == ' ') flushWord(); else word += c;
  }
  flushWord(); if (line.length()) flushLine();
  return n;
}

// Message log, newest at the bottom, each message in its sender's colour.
static void render() {
  if (kbOpen) return;                       // keyboard owns the screen
  tft.fillScreen(TFT_BLACK);
  const int W = tft.width(), H = tft.height();
  const int gap = (W - 2 * PANEL) / 3;
  String wifi = String("WIFI:T:") + (AP_PASS ? "WPA" : "nopass") + ";S:" + AP_SSID +
                (AP_PASS ? String(";P:") + AP_PASS : String("")) + ";;";
  if (showQR) {
    drawQR(gap, 2, wifi, "1. Join WiFi");
    drawQR(2 * gap + PANEL, 2, LOC, "2. Open page");
  }
  const int size = showQR ? 1 : 2;
  const int top = showQR ? 2 + PANEL + 16 : 26, ch = 9 * size, cw = 6 * size;
  const int bottom = showQR ? 194 : H - 2;                 // QR screen reserves a strip for the colour selector
  const int rows = (bottom - top) / ch, cols = (W - 8) / cw;
  // Fill rows from the newest message upwards, then draw top-down.
  struct L { String s; uint16_t c; int nickLen; } out[24]; int nOut = 0;
  { Lock lk;                                                   // snapshot the log, then draw without the lock
    for (int m = nMsgs - 1; m >= 0 && nOut < rows; m--) {
      String lines[8];
      int n = min(wrap(msgs[m].nick + ": " + msgs[m].text, cols, lines, 8), 8);
      for (int i = n - 1; i >= 0 && nOut < rows && nOut < 24; i--) out[nOut++] = { lines[i], msgs[m].col, i == 0 ? (int)msgs[m].nick.length() + 1 : 0 };
    }
  }
  tft.setTextSize(size);
  for (int i = 0; i < nOut; i++) {
    tft.setCursor(4, top + (rows - 1 - i) * ch);
    int k = min(out[i].nickLen, (int)out[i].s.length());           // nick (and its colon) in colour, rest green
    tft.setTextColor(out[i].c, TFT_BLACK);   tft.print(out[i].s.substring(0, k));
    tft.setTextColor(TFT_GREEN, TFT_BLACK);  tft.print(out[i].s.substring(k));
  }
  if (showQR) {
    tft.setTextSize(1); tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(2, 197);
    tft.print(kickArmed >= 0 ? "Tap again to KICK (60 s block)" : "Next client's colour. Tap a taken one to kick.");
    for (int i = 0; i < 8; i++) {
      int x = i * 40 + 1, y = 208, w = 38, h = 30;
      uint32_t c = PAL[i];
      if (slots[i].used) {
        tft.fillRect(x, y, w, h, tft.color565((c >> 16) / 5, ((c >> 8) & 255) / 5, (c & 255) / 5));
        tft.drawLine(x + 4, y + 4, x + w - 5, y + h - 5, 0x8410); tft.drawLine(x + 4, y + h - 5, x + w - 5, y + 4, 0x8410);
      } else {
        tft.fillRect(x, y, w, h, pal565(i));
      }
      if (i == kickArmed) for (int t = 0; t < 3; t++) tft.drawRect(x + t, y + t, w - 2 * t, h - 2 * t, TFT_RED);
      else if (i == selected) for (int t = 0; t < 3; t++) tft.drawRect(x + t, y + t, w - 2 * t, h - 2 * t, TFT_WHITE);
    }
  } else {
    tft.fillRoundRect(W - 42, 2, 40, 22, 4, 0x31A6);
    tft.setTextSize(2); tft.setTextColor(TFT_GREEN, 0x31A6); tft.setCursor(W - 42 + 8, 6); tft.print("QR");
  }
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>
<title>CYD</title><style>body{font:18px monospace;background:#000;color:#0f0;max-width:30em;margin:1em auto;padding:0 1em}
input,textarea,button{font:inherit;background:#000;color:#0f0;border:1px solid #0a0;width:100%;box-sizing:border-box;margin:.2em 0}
.sw{display:inline-block;width:2.2em;height:2.2em;border-radius:50%;margin:.15em;border:3px solid transparent}
.sw.on{border-color:#0f0}#log div{margin:.2em 0}</style>
<input id=n placeholder="Nickname" maxlength=12>
<div id=sw></div>
<textarea id=t rows=2 maxlength=140 placeholder="Message"></textarea>
<button id=b>Send</button><div id=log></div>
<script>
const C=['#ff4040','#ff9020','#ffe030','#40e060','#30d0ff','#6080ff','#c060ff','#ff60c0'];
let col=localStorage.c||C[(Math.random()*C.length)|0];
const $=id=>document.getElementById(id);
$('n').value=localStorage.n||'';
C.forEach(c=>{const d=document.createElement('span');d.className='sw';d.style.background=c;
 d.onclick=()=>{col=c;localStorage.c=c;mark()};d.dataset.c=c;$('sw').appendChild(d)});
function mark(){[...$('sw').children].forEach(d=>d.classList.toggle('on',d.dataset.c==col))}mark();
async function load(){const r=await fetch('/log');const rows=(await r.text()).split('\n').filter(x=>x);
 $('log').replaceChildren(...rows.reverse().map(x=>{const[c,n,t]=x.split('\t');const d=document.createElement('div');
  const b=document.createElement('b');b.style.color=c;b.textContent=n+': ';d.append(b,t);return d}))}
$('b').onclick=async()=>{const t=$('t').value.trim();if(!t)return;localStorage.n=$('n').value;
 await fetch('/send',{method:'POST',body:new URLSearchParams({n:$('n').value,c:col,t})});$('t').value='';load()};
fetch('/me').then(r=>r.text()).then(t=>{if(t){col=t;localStorage.c=t;$('sw').style.display='none';$('n').style.border='3px solid '+t}});
load();setInterval(load,3000);
</script>)HTML";

static String clean(String s, size_t max) {
  s.replace('\t', ' '); s.replace('\r', ' ');
  s.trim(); return s.substring(0, max);
}

static void addMsg(String nick, uint16_t col, const String &text) {
  { Lock lk;
    if (nMsgs == MAX_MSGS) { for (int i = 1; i < MAX_MSGS; i++) msgs[i - 1] = msgs[i]; nMsgs--; }
    nick.replace('\n', ' '); msgs[nMsgs++] = { nick, col, text };
  }
  needRender = true;                                          // loop() redraws; never draw from the httpd task
}

static int slotOfMac(const uint8_t *mac) {
  for (int i = 0; i < 8; i++) if (slots[i].used && !memcmp(slots[i].mac, mac, 6)) return i;
  return -1;
}
static int nextFree(int from) {
  for (int k = 0; k < 8; k++) { int i = (from + k) % 8; if (!slots[i].used) return i; }
  return -1;
}
static bool isBanned(const uint8_t *mac) {
  for (auto &b : bans) if (b.until && (int32_t)(b.until - millis()) > 0 && !memcmp(b.mac, mac, 6)) return true;
  return false;
}
// WiFi event task: hand the selected colour to the joining MAC, then advance the selection.
static void onAP(arduino_event_id_t ev, arduino_event_info_t info) {
  if (ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t *mac = info.wifi_ap_staconnected.mac;
    if (isBanned(mac)) { esp_wifi_deauth_sta(info.wifi_ap_staconnected.aid); return; }
    if (slotOfMac(mac) >= 0) return;
    int c = slots[selected].used ? nextFree(selected) : selected;
    if (c < 0) return;                                   // all 8 colours taken: client stays uncoloured
    memcpy(slots[c].mac, mac, 6); slots[c].used = true;
    int n = nextFree(c + 1); if (n >= 0) selected = n;
    uiDirty = true;
  } else if (ev == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    int c = slotOfMac(info.wifi_ap_stadisconnected.mac);
    if (c >= 0) { slots[c].used = false; uiDirty = true; }
  }
}
// Which colour slot belongs to this HTTP client? (IP -> MAC via the AP's DHCP table)
static int slotOfIP(IPAddress ip) {
  wifi_sta_list_t sl; wifi_sta_mac_ip_list_t ml;
  if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK || esp_wifi_ap_get_sta_list_with_ip(&sl, &ml) != ESP_OK) return -1;
  for (int i = 0; i < ml.num; i++) if (ml.sta[i].ip.addr == (uint32_t)ip) return slotOfMac(ml.sta[i].mac);
  return -1;
}
static void kickSlot(int i) {
  uint16_t aid;
  for (auto &b : bans) if (!b.until || (int32_t)(b.until - millis()) <= 0) { memcpy(b.mac, slots[i].mac, 6); b.until = millis() + BAN_MS; break; }
  if (esp_wifi_ap_get_sta_aid(slots[i].mac, &aid) == ESP_OK) esp_wifi_deauth_sta(aid);
}

// ---------- HTTPS web layer ----------
static String urlDecode(const String &s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') o += ' ';
    else if (c == '%' && i + 2 < s.length() && isxdigit(s[i + 1]) && isxdigit(s[i + 2])) { o += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16); i += 2; }
    else o += c;
  }
  return o;
}
static String formArg(const String &body, const char *key) {
  String k = String(key) + "=";
  int start = 0;
  while (start <= (int)body.length()) {
    int amp = body.indexOf('&', start); if (amp < 0) amp = body.length();
    if (body.startsWith(k, start)) return urlDecode(body.substring(start + k.length(), amp));
    start = amp + 1;
  }
  return "";
}
static IPAddress peerIP(httpd_req_t *req) {
  struct sockaddr_storage ss; socklen_t len = sizeof ss;
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0) return IPAddress();
  if (ss.ss_family == AF_INET6) { const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr; return IPAddress(b[12], b[13], b[14], b[15]); }   // v4-mapped
  return IPAddress(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
}
static esp_err_t sendText(httpd_req_t *req, const char *type, const String &body) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.length());
}
static esp_err_t hRoot(httpd_req_t *req) { httpd_resp_set_type(req, "text/html"); return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1); }
static esp_err_t hMe(httpd_req_t *req) {
  int i = slotOfIP(peerIP(req));
  char b[8] = ""; if (i >= 0) snprintf(b, sizeof b, "#%06x", (unsigned)PAL[i]);
  return sendText(req, "text/plain", String(b));
}
static esp_err_t hLog(httpd_req_t *req) {
  String o;
  { Lock lk;
    for (int i = 0; i < nMsgs; i++) {
      char hex[8]; uint16_t c = msgs[i].col;      // RGB565 -> approximate #rrggbb for the web page
      snprintf(hex, sizeof hex, "#%02x%02x%02x", ((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31);
      String t = msgs[i].text; t.replace('\n', ' ');
      o += String(hex) + "\t" + msgs[i].nick + "\t" + t + "\n";
    }
  }
  return sendText(req, "text/plain", o);
}
static esp_err_t hSend(httpd_req_t *req) {
  size_t len = req->content_len;
  if (len == 0 || len > 1024) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length"); return ESP_FAIL; }
  char *buf = (char *)malloc(len + 1);
  if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, buf + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) { free(buf); return ESP_FAIL; }
    got += r;
  }
  buf[len] = 0; String body(buf); free(buf);
  String nick = clean(formArg(body, "n"), 12), text = clean(formArg(body, "t"), MAX_LEN), c = formArg(body, "c");
  if (!nick.length()) nick = "anon";
  bool ok = c.length() == 7 && c[0] == '#';
  for (int i = 1; ok && i < 7; i++) ok = isxdigit(c[i]);
  uint16_t col = TFT_WHITE;
  if (ok) { long v = strtol(c.c_str() + 1, nullptr, 16); col = tft.color565(v >> 16, (v >> 8) & 255, v & 255); }
  int si = slotOfIP(peerIP(req));
  if (si >= 0) col = pal565(si);                          // assigned colour overrides whatever the page sent
  if (text.length()) addMsg(nick, col, text);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, "", 0);
}
// Port 80: everything (captive-portal probes, http://192.168.4.1/, typed URLs) goes to the trusted https name.
static esp_err_t hRedirect(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", LOC);
  return httpd_resp_send(req, "", 0);
}
static httpd_handle_t httpsH = nullptr, httpH = nullptr;
static void startWeb() {
  httpd_ssl_config_t sc = HTTPD_SSL_CONFIG_DEFAULT();
  sc.servercert = cert_start; sc.servercert_len = cert_end - cert_start;
  sc.prvtkey_pem = key_start; sc.prvtkey_len = key_end - key_start;
  sc.httpd.max_open_sockets = 3; sc.httpd.lru_purge_enable = true;
  sc.httpd.max_uri_handlers = 8; sc.httpd.stack_size = 10240;
  esp_err_t e = httpd_ssl_start(&httpsH, &sc);
  Serial.printf("https start: %s\n", esp_err_to_name(e));
  if (e == ESP_OK) {
    static const httpd_uri_t u[] = {
      {"/", HTTP_GET, hRoot, nullptr}, {"/log", HTTP_GET, hLog, nullptr},
      {"/me", HTTP_GET, hMe, nullptr}, {"/send", HTTP_POST, hSend, nullptr}};
    for (auto &x : u) httpd_register_uri_handler(httpsH, &x);
  }
  httpd_config_t pc = HTTPD_DEFAULT_CONFIG();
  pc.server_port = 80; pc.ctrl_port = 32800;                // ctrl_port must differ from the TLS server's
  pc.uri_match_fn = httpd_uri_match_wildcard; pc.max_open_sockets = 2; pc.lru_purge_enable = true; pc.stack_size = 4096;
  if (httpd_start(&httpH, &pc) == ESP_OK) {
    static const httpd_uri_t r = {"/*", HTTP_GET, hRedirect, nullptr};
    httpd_register_uri_handler(httpH, &r);
  }
  Serial.printf("free heap after web start: %u\n", (unsigned)ESP.getFreeHeap());
}

// ---------- On-screen keyboard ----------
struct Key { const char *lab; uint8_t w; };            // w in 16 px units; each row sums to 20
static const Key R0[] = {{"1",2},{"2",2},{"3",2},{"4",2},{"5",2},{"6",2},{"7",2},{"8",2},{"9",2},{"0",2}};
static const Key R1[] = {{"q",2},{"w",2},{"e",2},{"r",2},{"t",2},{"y",2},{"u",2},{"i",2},{"o",2},{"p",2}};
static const Key R2[] = {{"a",2},{"s",2},{"d",2},{"f",2},{"g",2},{"h",2},{"j",2},{"k",2},{"l",2},{"<-",2}};
static const Key R3[] = {{"SHF",3},{"z",2},{"x",2},{"c",2},{"v",2},{"b",2},{"n",2},{"m",2},{".",3}};
static const Key R4[] = {{"NICK",3},{"COL",3},{"SPC",6},{"ESC",3},{"SEND",5}};
static const Key *ROWS[5] = {R0, R1, R2, R3, R4};
static const int ROWN[5] = {10, 10, 10, 9, 5};
static const int KB_TOP = 34, KB_H = 41;

static uint16_t pal565(int i) { uint32_t c = PAL[i]; return tft.color565(c >> 16, (c >> 8) & 255, c & 255); }

static void drawInput() {
  tft.fillRect(0, 0, tft.width(), KB_TOP - 2, TFT_BLACK);
  tft.setTextSize(2);
  String np = kbNick + (kbEditNick ? "_" : "");
  String tp = kbText + (kbEditNick ? "" : "_");
  int avail = 26 - (int)np.length() - 2;
  if ((int)tp.length() > avail) tp = tp.substring(tp.length() - avail);
  tft.setCursor(2, 8);
  tft.setTextColor(pal565(kbCol), TFT_BLACK); tft.print(np);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);     tft.print(": "); tft.print(tp);
}

static void drawKeyboard() {
  tft.fillScreen(TFT_BLACK);
  drawInput();
  tft.setTextSize(2);
  for (int r = 0; r < 5; r++) {
    int x = 0;
    for (int k = 0; k < ROWN[r]; k++) {
      const Key &key = ROWS[r][k];
      int w = key.w * 16, y = KB_TOP + r * KB_H;
      uint16_t bg = 0x31A6;                                   // dark grey
      if (!strcmp(key.lab, "SHF") && kbShift) bg = TFT_BLUE;
      if (!strcmp(key.lab, "NICK") && kbEditNick) bg = TFT_BLUE;
      if (!strcmp(key.lab, "COL")) bg = pal565(kbCol);
      if (!strcmp(key.lab, "SEND")) bg = TFT_DARKGREEN;
      tft.fillRoundRect(x + 1, y + 1, w - 2, KB_H - 2, 4, bg);
      String lab = !strcmp(key.lab, "SPC") ? "" : key.lab;
      if (lab.length() == 1 && kbShift) lab.toUpperCase();
      tft.setTextColor(!strcmp(key.lab, "COL") ? TFT_BLACK : TFT_GREEN, bg);
      tft.setCursor(x + (w - (int)lab.length() * 12) / 2, y + (KB_H - 14) / 2);
      tft.print(lab);
      x += w;
    }
  }
}

static void closeKeyboard() { kbOpen = false; render(); }
static void openKeyboard()  { kbOpen = true; kbShift = false; kbEditNick = false; drawKeyboard(); }

static void keyboardTap(int tx, int ty) {
  int r = (ty - KB_TOP) / KB_H;
  if (ty < KB_TOP || r < 0 || r > 4) return;
  int x = 0;
  for (int k = 0; k < ROWN[r]; k++) {
    const Key &key = ROWS[r][k]; int w = key.w * 16;
    if (tx >= x && tx < x + w) {
      String &target = kbEditNick ? kbNick : kbText;
      size_t maxLen = kbEditNick ? 12 : MAX_LEN;
      const char *l = key.lab;
      if (!strcmp(l, "SHF"))       { kbShift = !kbShift; drawKeyboard(); return; }
      if (!strcmp(l, "NICK"))      { kbEditNick = !kbEditNick; drawKeyboard(); return; }
      if (!strcmp(l, "COL"))       { kbCol = (kbCol + 1) % 8; drawKeyboard(); return; }
      if (!strcmp(l, "ESC"))       { closeKeyboard(); return; }
      if (!strcmp(l, "<-"))        { if (target.length()) target.remove(target.length() - 1); }
      else if (!strcmp(l, "SPC"))  { if (target.length() < maxLen) target += ' '; }
      else if (!strcmp(l, "SEND")) {
        String t = kbText; t.trim();
        if (t.length()) { String n = kbNick.length() ? kbNick : String("screen"); kbText = ""; kbOpen = false; addMsg(n, pal565(kbCol), t); }
        return;
      }
      else if (target.length() < maxLen) {
        char c = l[0];
        if (kbShift && c >= 'a' && c <= 'z') { c -= 32; kbShift = false; drawKeyboard(); }
        target += c;
      }
      drawInput(); return;
    }
    x += w;
  }
}

void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateMutex();
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  tft.init(); tft.setRotation(1);
  tft.writecommand(0x36); tft.writedata(0x20 | 0x08);   // landscape + BGR, same as the verified colour-correction app
  tft.invertDisplay(true);
  touchSPI.begin(25, 39, 32, 33);        // CLK, MISO, MOSI, CS
  ts.begin(touchSPI); ts.setRotation(1);
  WiFi.onEvent(onAP);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 8);   // channel 1, visible, up to 8 clients
  dns.start(53, "*", WiFi.softAPIP());
  startWeb();
  render();
}

void loop() {
  dns.processNextRequest();
  if (needRender) { needRender = false; render(); }
  static uint32_t lastTap = 0; static bool wasDown = false;
  bool down = ts.tirqTouched() && ts.touched();
  if (down && !wasDown && millis() - lastTap > 150) {   // rising edge, debounced
    lastTap = millis();
    TS_Point p = ts.getPoint();
    int tx = constrain(map(p.x, TS_X0, TS_X1, 0, tft.width()), 0, tft.width() - 1);
    int ty = constrain(map(p.y, TS_Y0, TS_Y1, 0, tft.height()), 0, tft.height() - 1);
    Serial.printf("touch raw %d,%d z=%d -> %d,%d\n", p.x, p.y, p.z, tx, ty);
    if (kbOpen) keyboardTap(tx, ty);
    else if (showQR && ty >= 196) {                       // colour selector / kick strip
      int i = min(tx / 40, 7);
      if (ty >= 206 && slots[i].used) {                    // taken colour: arm, then confirm to kick
        if (kickArmed == i && millis() - kickArmedAt < 3000) { kickSlot(i); kickArmed = -1; }
        else { kickArmed = i; kickArmedAt = millis(); }
        render();
      } else if (ty >= 206) { selected = i; kickArmed = -1; render(); }
    }
    else if (!showQR && tx >= tft.width() - 44 && ty < 28) { showQR = true; render(); }   // invite more clients
    else { kickArmed = -1; openKeyboard(); }
  }
  wasDown = down;
  if (uiDirty) { uiDirty = false; if (showQR) render(); }
  if (kickArmed >= 0 && millis() - kickArmedAt > 3000) { kickArmed = -1; if (showQR) render(); }

  // Codes vanish when a client joins the AP, and return when the last one leaves.
  static int lastClients = 0; static uint32_t lastPoll = 0;
  if (millis() - lastPoll > 500) {
    lastPoll = millis();
    int clients = WiFi.softAPgetStationNum();
    if (clients != lastClients) {
      if (clients > lastClients && showQR) { showQR = false; render(); }   // someone joined: back to the log
      else if (clients == 0 && lastClients > 0) { showQR = true; render(); }
      lastClients = clients;
    }
  }
}
