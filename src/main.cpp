/*
 * RTK1010Board — WiFi web router + LoRa  (Arduino / ESP32-S2)
 * ------------------------------------------------------------
 * Routes serial data between three endpoints, selectable from a web page:
 *
 *   GPS    : USB-CDC  <->  RTK-1010   (transparent passthrough)
 *   LoRa   : USB-CDC  <->  LoRa radio (configure / test the radio)
 *   ROVER  : LoRa -> RTK-1010 (RTCM in), RTK-1010 -> USB-CDC (position out),
 *            and USB-CDC -> RTK-1010 (so the GPS can still be configured)
 *
 * Hardware (ESP32-S2 has only 2 UARTs; UART pins are runtime-configurable via web):
 *   GPS  = UART1, default TX=GPIO1 RX=GPIO2 (onboard RTK-1010), enable = GPIO4
 *   LoRa = UART0, default TX=GPIO33 RX=GPIO35 (J5 "Conn_GPS" header, no soldering)
 *   USB-CDC = Arduino `Serial` (native USB).
 *
 * WiFi: STA (set via web UI) + AP "RTK1010", hostname rtk1010 (all overridable via web).
 *
 * The web/WiFi/OTA structure follows the esp32-web-interface example
 * (../esp32-web-interface-uart-backend), trimmed to a lean sketch.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include "driver/uart.h"
#include "soc/rtc_cntl_reg.h"   // RTC_CNTL_FORCE_DOWNLOAD_BOOT (software bootloader entry)

// ---------------------------------------------------------------- pins/ports
// ESP32-S2 has 2 UART controllers; pins are remappable via the GPIO matrix and
// are user-configurable at runtime (web UI), so these are just power-on defaults.
#define GPS_UART      UART_NUM_1
#define LORA_UART     UART_NUM_0
#define RTK_EN_PIN    4              // RTK-1010 enable (active high)

#define GPS_TX_DEF    1              // onboard RTK-1010: ESP TX=GPIO1, RX=GPIO2
#define GPS_RX_DEF    2
#define LORA_TX_DEF   33             // J5 "Conn_GPS" header GPIO33 (ESP TX -> LoRa RX)
#define LORA_RX_DEF   35             // J5 "Conn_GPS" header GPIO35 (ESP RX <- LoRa TX)
#define E220_M0_DEF   34             // E220 mode/AUX pins on the remaining free J5 GPIOs
#define E220_M1_DEF   36
#define E220_AUX_DEF  37

#define UART_RX_RING  4096
#define PUMP_BUF      512

#define GPS_DEFAULT_BAUD   115200u
#define LORA_DEFAULT_BAUD  115200u
#define LORA_MAX_BAUD      115200u   // LoRa module ceiling

// ---------------------------------------------------------------- state
enum RouteMode { MODE_GPS = 0, MODE_LORA = 1, MODE_ROVER = 2, MODE_HYBRID = 3 };
volatile RouteMode g_mode = MODE_GPS;
uint32_t g_loraBaud = LORA_DEFAULT_BAUD;
uint32_t g_gpsBaud  = GPS_DEFAULT_BAUD;
int gpsTx = GPS_TX_DEF, gpsRx = GPS_RX_DEF;     // runtime-configurable UART pins
int loraTx = LORA_TX_DEF, loraRx = LORA_RX_DEF;
int e220M0 = E220_M0_DEF, e220M1 = E220_M1_DEF, e220Aux = E220_AUX_DEF;
volatile int e220Req = 0;            // 0 none, 1 read, 2 write — serviced in the router task
volatile bool e220Busy = false, e220Ok = false;
static uint8_t e220Regs[8] = {0};    // ADDH,ADDL,REG0(SPED),REG1(OPTION),REG2(CHAN),REG3(MODE),CRYPTH,CRYPTL

// Console ring buffers (2 KB each, power-of-2 mask) for live stream viewers; head = total bytes written.
struct Ring { uint8_t buf[2048]; volatile uint32_t head; };
static Ring rGps = {}, rLora = {}, rHost = {}, rResp = {};
static void ringPush(Ring& r, const uint8_t* d, int n) { for(int i = 0; i < n; i++) r.buf[(r.head++) & 2047] = d[i]; }

// STA credentials are configured at runtime (web UI -> System tab, saved to NVS) and
// loaded over these defaults at boot. Left blank so the repo carries no real creds —
// a fresh flash comes up in AP mode (below) until you set your network.
String staSsid = "";
String staPass = "";
String apSsid  = "RTK1010";
String apPass  = "gps12345";
static const char* HOSTNAME = "rtk1010";

volatile uint32_t cntCdcRx = 0, cntCdcTx = 0, cntGpsRx = 0, cntGpsTx = 0, cntLoraRx = 0, cntLoraTx = 0;
volatile int gpsFixQual = 0;
volatile int gpsSats = 0;
volatile float gpsHdop = 0;       // horizontal dilution of precision (GGA field 8)
volatile uint32_t gpsGgaCount = 0;// GGA sentences seen (used to measure the live output rate)
volatile int gpsHz = 0;           // measured NMEA output rate (GGA/s), updated ~1 Hz
volatile double gpsLat = 0, gpsLon = 0; // last position, decimal degrees (from GGA)
volatile int gpsFixMode = 0;      // 1 none / 2 2D / 3 3D (from GSA field 2), max over ~1 s
static int gpsFixModeAcc = 0;     // accumulates the per-epoch max before publishing

Preferences prefs;
WebServer server(80);
WiFiServer navServer(10110);   // raw NMEA/RTCM stream over TCP (no DTR gate, unlike USB-CDC)
WiFiClient navClient;

static const char* modeName(RouteMode m)
{
    switch(m) { case MODE_GPS: return "gps"; case MODE_LORA: return "lora"; case MODE_ROVER: return "rover"; case MODE_HYBRID: return "hybrid"; }
    return "?";
}

// ---------------------------------------------------------------- UART setup
static void uartInit(uart_port_t port, int tx, int rx, uint32_t baud)
{
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB; // arduino-esp32 2.0.x == IDF 4.4
    if(uart_is_driver_installed(port)) uart_driver_delete(port);
    uart_driver_install(port, UART_RX_RING, 0, 0, NULL, 0);
    uart_param_config(port, &cfg);
    uart_set_pin(port, tx, rx, -1, -1);
}

// ---------------------------------------------------------------- NMEA sniff (status only)
static char ggaBuf[128];
static int  ggaLen = 0;

static void parseGGA(const char* s)
{
    // $xxGGA,time,lat,N,lon,E,fix,numSV,HDOP,...  (lat=2,N/S=3,lon=4,E/W=5,fix=6,sats=7,HDOP=8)
    gpsGgaCount++;
    int comma = 0;
    double rawLat = 0, rawLon = 0; char hLat = 0, hLon = 0;
    for(const char* p = s; *p; p++)
    {
        if(*p == ',')
        {
            comma++;
            const char* v = p + 1;
            if(comma == 2) rawLat = atof(v);
            else if(comma == 3) hLat = *v;
            else if(comma == 4) rawLon = atof(v);
            else if(comma == 5) hLon = *v;
            else if(comma == 6) gpsFixQual = atoi(v);
            else if(comma == 7) gpsSats = atoi(v);
            else if(comma == 8) { gpsHdop = (float)atof(v); break; }
        }
    }
    // NMEA lat/lon are ddmm.mmmm / dddmm.mmmm -> decimal degrees
    if(rawLat > 0) { int d = (int)(rawLat / 100); double dec = d + (rawLat - d * 100) / 60.0; gpsLat = (hLat == 'S') ? -dec : dec; }
    if(rawLon > 0) { int d = (int)(rawLon / 100); double dec = d + (rawLon - d * 100) / 60.0; gpsLon = (hLon == 'W') ? -dec : dec; }
}

volatile int gpsSatByConst[6] = {0}; // sats USED per constellation (from GSA): GPS,GLO,GAL,BDS,QZSS,NavIC

// Per-satellite table: C/N0 from GSV, used-in-fix from GSA. Read by /sats (web task)
// while the router task writes it — a benign race like the console rings.
struct SatInfo { uint16_t svid; uint8_t cno; uint8_t sys; uint32_t seen; uint32_t usedAt; };
static SatInfo gSat[80];
static volatile int gSatN = 0;
static SatInfo* satGet(int sys, int svid) {
    for(int i = 0; i < gSatN; i++) if(gSat[i].sys == (uint8_t)sys && gSat[i].svid == (uint16_t)svid) return &gSat[i];
    if(gSatN < (int)(sizeof(gSat) / sizeof(gSat[0]))) { gSat[gSatN] = { (uint16_t)svid, 0, (uint8_t)sys, 0, 0 }; return &gSat[gSatN++]; }
    return NULL;
}
static SatInfo* satFindOnly(int sys, int svid) {
    for(int i = 0; i < gSatN; i++) if(gSat[i].sys == (uint8_t)sys && gSat[i].svid == (uint16_t)svid) return &gSat[i];
    return NULL;
}

// Constellation index from an NMEA talker ($GP/$GL/$GA/$GB/$GQ/$GI) or, for the
// combined $GN… talker, the trailing systemId field: 0 GPS,1 GLO,2 GAL,3 BDS,4 QZSS,5 NavIC.
static int nmeaSysIdx(const char* s, const char* star) {
    char a = s[1], b = s[2];
    if(a == 'G') { if(b=='P') return 0; if(b=='L') return 1; if(b=='A') return 2; if(b=='B') return 3; if(b=='Q') return 4; if(b=='I') return 5; }
    if(a == 'B' && b == 'D') return 3;
    if(star && star > s) { const char* q = star - 1; while(q > s && *q != ',') q--; if(*q == ',') { int id = atoi(q + 1); if(id >= 1 && id <= 6) return id - 1; } }
    return -1;
}

// $xxGSA,mode,fix,sv1..sv12,PDOP,HDOP,VDOP[,systemId]* — count used SVs per constellation
// and stamp each listed PRN as used-in-fix in the satellite table.
static void parseGSA(const char* s)
{
    const char* star = strchr(s, '*');
    int sys = nmeaSysIdx(s, star);
    uint32_t now = millis();
    int comma = 0, used = 0;
    for(const char* p = s; *p && p != star; p++)
        if(*p == ',') {
            comma++;
            if(comma == 2) { int fm = atoi(p + 1); if(fm > gpsFixModeAcc) gpsFixModeAcc = fm; }  // 1/2/3
            if(comma >= 3 && comma <= 14) {
                char nc = *(p + 1);
                if(nc != ',' && nc != '*' && nc != '\r' && nc != '\n' && nc != 0) {
                    used++;
                    if(sys >= 0) { SatInfo* si = satFindOnly(sys, atoi(p + 1)); if(si) si->usedAt = now; }
                }
            }
        }
    if(sys >= 0) gpsSatByConst[sys] = used;
}

// $xxGSV,totMsgs,msgNum,totSVs,{svid,elev,azim,cno}x4* — capture per-satellite C/N0.
static void parseGSV(const char* s)
{
    const char* star = strchr(s, '*');
    int sys = nmeaSysIdx(s, star); if(sys < 0) sys = 0;
    uint32_t now = millis();
    int fnum = 0, curSvid = 0;
    const char* start = s;
    for(const char* q = s; ; q++) {
        if(*q == ',' || *q == '*' || *q == 0 || q == star) {
            if(fnum >= 4) {                       // satellite groups begin at field 4
                int g = (fnum - 4) & 3;           // 0 svid, 1 elev, 2 azim, 3 cno
                if(g == 0) curSvid = atoi(start);
                else if(g == 3 && curSvid > 0) { SatInfo* si = satGet(sys, curSvid); if(si) { si->cno = (uint8_t)atoi(start); si->seen = now; } }
            }
            fnum++; start = q + 1;
            if(*q == 0 || *q == '*' || q == star) break;
        }
    }
}

static void sniffGps(const uint8_t* d, int n)
{
    for(int i = 0; i < n; i++)
    {
        char c = (char)d[i];
        if(c == '$') { ggaLen = 0; ggaBuf[ggaLen++] = c; }
        else if(ggaLen > 0)
        {
            if(c == '\r' || c == '\n')
            {
                ggaBuf[ggaLen] = 0;
                if(ggaLen > 6) {
                    if(strstr(ggaBuf, "GGA")) parseGGA(ggaBuf);
                    else if(strstr(ggaBuf, "GSA")) parseGSA(ggaBuf);
                    else if(strstr(ggaBuf, "GSV")) parseGSV(ggaBuf);
                }
                if(ggaLen > 3 && ggaBuf[1] == 'P') {        // $P… proprietary reply -> response console
                    ringPush(rResp, (const uint8_t*)ggaBuf, ggaLen);
                    const uint8_t nl = '\n'; ringPush(rResp, &nl, 1);
                }
                ggaLen = 0;
            }
            else if(ggaLen < (int)sizeof(ggaBuf) - 1) ggaBuf[ggaLen++] = c;
            else ggaLen = 0;
        }
    }
}

// ---------------------------------------------------------------- RTCM3 sniff (LoRa, status only)
// Counts CRC-valid RTCM3 frames on the LoRa input and remembers the last type.
static uint32_t rtcmCrc24q(const uint8_t* buf, int size)
{
    static const uint32_t t[] = {
        0x000000, 0x01864CFB, 0x038AD50D, 0x020C99F6, 0x0793E6E1, 0x0615AA1A, 0x041933EC, 0x059F7F17,
        0x0FA18139, 0x0E27CDC2, 0x0C2B5434, 0x0DAD18CF, 0x083267D8, 0x09B42B23, 0x0BB8B2D5, 0x0A3EFE2E };
    uint32_t crc = 0;
    for(int i = 0; i < size; i++) {
        crc ^= (uint32_t)buf[i] << 16;
        crc = (crc << 4) ^ t[(crc >> 20) & 0x0F];
        crc = (crc << 4) ^ t[(crc >> 20) & 0x0F];
    }
    return crc & 0xFFFFFF;
}

volatile uint32_t rtcmCount = 0;     // CRC-valid RTCM3 frames seen on LoRa
volatile int rtcmType = 0;           // last valid message type
volatile int rtcmSatByConst[8] = {0}; // sats per constellation (idx = type/10 - 107); summed for total
#define RTCM_HIST_MAX 32
static int rtcmHistType[RTCM_HIST_MAX] = {0};   // distinct RTCM message types seen
static uint32_t rtcmHistCnt[RTCM_HIST_MAX] = {0};
static volatile int rtcmHistN = 0;
static uint8_t rtcmBuf[1029];        // max RTCM3 frame = 3 + 1023 + 3
static int rtcmIdx = 0, rtcmState = 0, rtcmLen = 0;

// Read len bits (big-endian) from a byte buffer at bit offset pos.
static uint64_t rtcmGetBits(const uint8_t* p, int pos, int len)
{
    uint64_t v = 0;
    for(int i = pos; i < pos + len; i++) v = (v << 1) | ((p[i >> 3] >> (7 - (i & 7))) & 1);
    return v;
}

static void sniffRtcm(const uint8_t* d, int n)
{
    for(int i = 0; i < n; i++) {
        uint8_t b = d[i];
        if(rtcmState == 0) { if(b == 0xD3) { rtcmBuf[0] = b; rtcmIdx = 1; rtcmState = 1; } }
        else if(rtcmState == 1) { rtcmBuf[rtcmIdx++] = b; rtcmLen = (b & 0x03) << 8; rtcmState = 2; }
        else if(rtcmState == 2) { rtcmBuf[rtcmIdx++] = b; rtcmLen |= b; rtcmState = (rtcmLen >= 1 && rtcmLen <= 1023) ? 3 : 0; }
        else { // payload + 3-byte CRC; CRC-24Q over the whole frame is 0 when valid
            rtcmBuf[rtcmIdx++] = b;
            if(rtcmIdx >= rtcmLen + 6) {
                if(rtcmCrc24q(rtcmBuf, rtcmIdx) == 0) {
                    rtcmCount++;
                    int type = (rtcmBuf[3] << 4) | (rtcmBuf[4] >> 4);
                    rtcmType = type;
                    // per-type histogram (so we can see every message the base sends)
                    int hi = 0; while(hi < rtcmHistN && rtcmHistType[hi] != type) hi++;
                    if(hi == rtcmHistN && rtcmHistN < RTCM_HIST_MAX) { rtcmHistType[rtcmHistN] = type; rtcmHistN++; }
                    if(hi < RTCM_HIST_MAX) rtcmHistCnt[hi]++;
                    // MSM4-7 (10x1..10x7) carry a 64-bit satellite mask at payload bit 73; its
                    // popcount = sats for THAT constellation. Track per constellation so the
                    // status can sum a true total across GPS/GLONASS/Galileo/BeiDou/etc.
                    if(type >= 1071 && type <= 1137 && (type % 10) >= 1 && (type % 10) <= 7 && rtcmLen >= 18) {
                        int idx = type / 10 - 107;  // 107x GPS,108x GLO,109x GAL,110x SBAS,111x QZSS,112x BDS,113x NavIC
                        if(idx >= 0 && idx < 8) rtcmSatByConst[idx] = (int)__builtin_popcountll(rtcmGetBits(rtcmBuf + 3, 73, 64));
                    }
                }
                rtcmState = 0;
            } else if(rtcmIdx >= (int)sizeof(rtcmBuf)) rtcmState = 0;
        }
    }
}

// ---------------------------------------------------------------- USB-CDC helpers
static int readCdc(uint8_t* b, size_t maxn)
{
    int n = 0;
    while(Serial.available() && n < (int)maxn) b[n++] = (uint8_t)Serial.read();
    return n;
}

// Write to USB-CDC via the Arduino USBCDC API (its USB task drives the actual TX
// completion). NOTE: USBCDC delivers data only while the host asserts DTR; the
// connection state is exposed in /status as "cdcconn" so this is observable.
extern "C" {
    bool     tud_cdc_n_connected(uint8_t itf);
    uint32_t tud_cdc_n_write_available(uint8_t itf);
}
// Deliver to USB-CDC fully when the host is reading, without blocking the router for
// long. Write only what the FIFO can take, then briefly wait for it to drain. If the
// host stalls (DTR held but not reading) give up after ~30 ms and drop the remainder.
// Earlier we dropped the remainder of every chunk immediately — at high data rates that
// shredded NMEA mid-sentence and showed up as garbage/"unknown" blocks in the GPS app.
static uint32_t cdcWrite(const uint8_t* b, uint32_t n)
{
    if(!tud_cdc_n_connected(0)) return 0;
    uint32_t sent = 0, t0 = millis();
    while(sent < n) {
        uint32_t avail = tud_cdc_n_write_available(0);
        if(avail) {
            uint32_t chunk = n - sent; if(chunk > avail) chunk = avail;
            uint32_t w = Serial.write(b + sent, chunk);
            sent += w;
            if(w < chunk) break;
        } else {
            if((uint32_t)(millis() - t0) > 30) break;   // host not draining -> drop the rest
            delay(1);
        }
    }
    return sent;
}

// Accept (a single) TCP client on the NMEA/RTCM port.
static void tcpAccept()
{
    // Accept a new client even when an old one is stale/half-open, replacing it — a
    // NMEA app that didn't close cleanly must not lock out the next connection.
    WiFiClient c = navServer.available();
    if(c) { if(navClient) navClient.stop(); navClient = c; navClient.setNoDelay(true); navClient.setTimeout(1); }
}
// Routed output -> USB-CDC (needs host DTR) AND the TCP client (no DTR). Returns CDC bytes.
static uint32_t outWrite(const uint8_t* b, uint32_t n)
{
    // TCP: write directly. WiFiClient has no availableForWrite() (returns 0), so the
    // earlier "bounded" loop silently dropped everything — this is the original path
    // that delivered NMEA correctly. tcpAccept() sets a 1 s socket send-timeout so a
    // peer that stops reading can stall the router for at most ~1 s, never forever.
    if(navClient.connected()) navClient.write(b, n);
    return cdcWrite(b, n);
}
// Host input from USB-CDC or the TCP client.
static int readHost(uint8_t* b, size_t maxn)
{
    int n = readCdc(b, maxn);
    if(n == 0 && navClient.connected())
        while(navClient.available() && n < (int)maxn) b[n++] = (uint8_t)navClient.read();
    if(n > 0) ringPush(rHost, b, n);
    return n;
}

// USB-CDC baud is virtual; the GPS/LoRa UART baud rates are set explicitly from
// the web UI (the router happily bridges ports running at different bauds), so we
// no longer mirror the host line-coding onto the UARTs.

// ---------------------------------------------------------------- E220 config
static void e220WaitAux(int timeoutMs)
{
    if(e220Aux < 0) { delay(timeoutMs); return; }
    uint32_t t = millis();
    while(digitalRead(e220Aux) == LOW && (uint32_t)(millis() - t) < (uint32_t)timeoutMs) delay(2);
    delay(5);
}

// Runs in the router task (owns LORA_UART). req 1=read regs, 2=write e220Regs.
// E220 config mode = M0=1,M1=1, UART forced to 9600 8N1; C0=write/save, C1=read.
static void doE220(int req)
{
    uint32_t saved = g_loraBaud;
    digitalWrite(e220M1, HIGH); digitalWrite(e220M0, HIGH);   // mode 3 = configuration
    e220WaitAux(100); delay(40);
    uart_set_baudrate(LORA_UART, 9600);
    uart_flush_input(LORA_UART);

    uint8_t cmd[11]; int clen;
    if(req == 2) { cmd[0] = 0xC0; cmd[1] = 0x00; cmd[2] = 0x08; memcpy(cmd + 3, e220Regs, 8); clen = 11; }
    else         { cmd[0] = 0xC1; cmd[1] = 0x00; cmd[2] = 0x08; clen = 3; }
    uart_write_bytes(LORA_UART, (const char*)cmd, clen);

    uint8_t resp[16] = {0};
    int got = uart_read_bytes(LORA_UART, resp, 11, 500 / portTICK_PERIOD_MS);
    e220Ok = (got >= 11 && (resp[0] == 0xC1 || resp[0] == 0xC0) && resp[1] == 0x00 && resp[2] == 0x08);
    if(e220Ok) memcpy(e220Regs, resp + 3, 8);

    uart_set_baudrate(LORA_UART, saved);
    digitalWrite(e220M0, LOW); digitalWrite(e220M1, LOW);     // back to mode 0 = transparent
    e220WaitAux(100); delay(20);
    uart_flush_input(LORA_UART);
}

// ---------------------------------------------------------------- router task
static void routerTask(void* arg)
{
    static uint8_t buf[PUMP_BUF];
    for(;;)
    {
        if(e220Req) { doE220(e220Req); e220Req = 0; e220Busy = false; }

        // Measure the live NMEA output rate (GGA/s) once per second — independent of
        // web polling, and the only reliable readback (FIXRATE has no query command).
        { static uint32_t rateMs = 0, rateCnt = 0; uint32_t nm = millis();
          if(nm - rateMs >= 1000) { gpsHz = (int)(gpsGgaCount - rateCnt); rateCnt = gpsGgaCount; rateMs = nm;
                                     gpsFixMode = gpsFixModeAcc; gpsFixModeAcc = 0; } }

        RouteMode m = g_mode;
        int n;
        tcpAccept();

        // Always drain + decode the GPS UART (keeps fix/sats live in every mode).
        // Forward GPS -> host (USB + TCP) only when routed to GPS (GPS or rover mode).
        n = uart_read_bytes(GPS_UART, buf, sizeof(buf), 0);
        if(n > 0) {
            cntGpsRx += n; sniffGps(buf, n); ringPush(rGps, buf, n);
            if(m == MODE_GPS || m == MODE_ROVER || m == MODE_HYBRID) cntCdcTx += outWrite(buf, n);
        }

        // Always drain + decode the LoRa UART (keeps RTCM counters live in every mode).
        // Forward LoRa -> host (USB + TCP) in LoRa mode, or LoRa -> GPS (RTCM) in rover mode.
        n = uart_read_bytes(LORA_UART, buf, sizeof(buf), 0);
        if(n > 0) {
            cntLoraRx += n; sniffRtcm(buf, n); ringPush(rLora, buf, n);
            if(m == MODE_LORA) cntCdcTx += outWrite(buf, n);
            else if(m == MODE_ROVER || m == MODE_HYBRID) { uart_write_bytes(GPS_UART, (const char*)buf, n); cntGpsTx += n; }
        }

        // Host (USB or TCP) -> the currently routed UART.
        n = readHost(buf, sizeof(buf));
        if(n > 0) {
            cntCdcRx += n;
            if(m == MODE_LORA) { uart_write_bytes(LORA_UART, (const char*)buf, n); cntLoraTx += n; }
            else if(m == MODE_GPS || m == MODE_HYBRID) { uart_write_bytes(GPS_UART, (const char*)buf, n); cntGpsTx += n; }
            // rover = output-only (host input not forwarded to the GPS)
        }

        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------- web handlers
#if 0  /* legacy embedded UI — replaced by the SPIFFS web app in data/ */
static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>RTK1010 Router</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}
header{background:#1b2733;padding:14px 18px;font-size:20px;font-weight:600}
.wrap{padding:16px;max-width:680px;margin:auto}
.card{background:#1b2733;border-radius:10px;padding:14px 16px;margin:12px 0}
.card h2{margin:0 0 10px;font-size:14px;color:#7fd1ff;text-transform:uppercase;letter-spacing:.05em}
button{background:#2a3a4a;color:#e6e6e6;border:0;border-radius:8px;padding:10px 14px;margin:4px;font-size:15px;cursor:pointer}
button.active{background:#2e7d32}button:hover{filter:brightness(1.2)}
input{background:#0f1419;color:#e6e6e6;border:1px solid #34465a;border-radius:6px;padding:8px;margin:3px 0;width:100%;box-sizing:border-box}
.grid{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;font-size:14px}
.grid div:nth-child(odd){color:#9bb}label{font-size:13px;color:#9bb}
</style></head><body>
<header>RTK1010 Router</header><div class=wrap>
<div class=card><h2>Routing mode</h2>
<button id=mgps onclick="setMode('gps')">USB &#8644; GPS</button>
<button id=mlora onclick="setMode('lora')">USB &#8644; LoRa</button>
<button id=mrover onclick="setMode('rover')">RTK rover</button>
<button id=mhybrid onclick="setMode('hybrid')">Hybrid</button>
<p style="font-size:13px;color:#9bb;margin:10px 0 0;line-height:1.5">
<b>USB &#8644; GPS</b> — your serial app talks straight to the RTK-1010 (raw NMEA / config).<br>
<b>USB &#8644; LoRa</b> — your serial app talks to the LoRa radio.<br>
<b>RTK rover</b> — LoRa feeds RTCM corrections into the GPS; corrected position streams out to USB/TCP (output only).<br>
<b>Hybrid</b> — same RTCM injection, but the GPS is fully bridged to USB/TCP both ways (read + configure).</p>
<p style="font-size:13px;color:#9bb;margin:6px 0 0">Outputs on USB-CDC (needs DTR) and TCP <b>:10110</b> (no DTR).</p></div>
<div class=card><h2>Status</h2><div class=grid id=status>...</div></div>
<div class=card><h2>Ports / baud</h2>
<label>GPS baud (match the receiver)</label><select id=gb></select>
<label>LoRa baud (max 115200)</label><select id=lbsel></select>
<label>GPS pins &mdash; TX / RX GPIO (onboard RTK = 1 / 2)</label>
<div style="display:flex;gap:8px"><input id=gtx type=number><input id=grx type=number></div>
<label>LoRa pins &mdash; TX / RX GPIO (J5 Conn_GPS = 33 / 35)</label>
<div style="display:flex;gap:8px"><input id=ltx type=number><input id=lrx type=number></div>
<button onclick="savePorts()">Apply</button></div>
<div class=card><h2>RTK-1010 config</h2>
<input id=gcmd placeholder="LOCOSYS cmd, e.g. PAIR050,200 (no $ / checksum)">
<button onclick="gc($('gcmd').value)">Send</button>
<div style="margin-top:6px">
<button onclick="gc('PLSC,VER')">Version</button><button onclick="gc('PAIR050,1000')">1Hz</button>
<button onclick="gc('PAIR050,200')">5Hz</button><button onclick="gc('PAIR050,100')">10Hz</button>
<button onclick="gc('PAIR513')">Save</button></div>
<small style="color:#789">Commands go to the GPS in any mode; replies stream on the GPS output (use GPS/Hybrid mode to read them).</small></div>
<div class=card><h2>LoRa radio (E220)</h2>
<button onclick="e2read()">Read module</button> <span id=e2ok style="font-size:13px;color:#9bb"></span>
<label>Address (0-65535)</label><input id=e2addr type=number>
<label>Channel 0-80 (<span id=e2freq>&mdash;</span>)</label><input id=e2ch type=number oninput="e2f()">
<label>Air data rate</label><select id=e2air></select>
<label>Module UART baud</label><select id=e2ub></select>
<label>TX power</label><select id=e2pwr></select>
<label>Sub-packet size</label><select id=e2sub></select>
<label>Encryption key (0-65535, write-only)</label><input id=e2key type=number value=0>
<label><input type=checkbox id=e2fx style="width:auto"> Fixed-point addressing</label>
<label><input type=checkbox id=e2rb style="width:auto"> Append RSSI byte to RX</label>
<button onclick="e2write()">Write module</button>
<label>Mode pins M0 / M1 / AUX (GPIO; AUX=-1 to disable)</label>
<div style="display:flex;gap:8px"><input id=e2m0 type=number><input id=e2m1 type=number><input id=e2aux type=number></div>
<small style="color:#789">Read first, edit, then Write. Briefly switches the radio into config mode (M0/M1 high). Wire M0/M1/AUX to the GPIOs above.</small></div>
<div class=card><h2>WiFi</h2>
<label>Station SSID</label><input id=ss><label>Station password</label><input id=sp type=password>
<label>AP SSID</label><input id=as><label>AP password</label><input id=ap type=password>
<button onclick="saveWifi()">Save &amp; reboot</button></div>
<div class=card><h2>Firmware update</h2>
<form method=POST action=/update enctype=multipart/form-data>
<input type=file name=fw accept=.bin><button type=submit>Upload &amp; flash</button></form></div>
<div class=card><button onclick="if(confirm('Reboot?'))fetch('/reboot')">Reboot</button></div></div>
<script>
var $=function(i){return document.getElementById(i)};
var GBAUDS=[9600,19200,38400,57600,115200,230400,460800,921600,1000000,2000000];
var LBAUDS=[9600,19200,38400,57600,115200];
function opts(s,a){s.innerHTML=a.map(function(b){return '<option>'+b+'</option>'}).join('')}
opts($('gb'),GBAUDS);opts($('lbsel'),LBAUDS);
var RNAMES=['GPS','GLO','GAL','SBAS','QZSS','BDS','NavIC'];
var GNAMES=['GPS','GLO','GAL','BDS','QZSS','NavIC'];
function brk(a,nm){var o=[];if(a)for(var i=0;i<a.length;i++)if(a[i]>0)o.push(nm[i]+' '+a[i]);return o.join(', ')}
function setMode(m){fetch('/mode?set='+m).then(refresh)}
function savePorts(){var d=new URLSearchParams();
d.set('gpsbaud',$('gb').value);d.set('lorabaud',$('lbsel').value);
d.set('gtx',$('gtx').value);d.set('grx',$('grx').value);
d.set('ltx',$('ltx').value);d.set('lrx',$('lrx').value);
fetch('/ports?'+d.toString()).then(refresh)}
function gc(c){if(c)fetch('/gpscmd?cmd='+encodeURIComponent(c))}
var AIR=['2.4k','2.4k','2.4k','4.8k','9.6k','19.2k','38.4k','62.5k'];
var UBP=['1200','2400','4800','9600','19200','38400','57600','115200'];
var PWR=['22 dBm','17 dBm','13 dBm','10 dBm'];var SUBP=['200 B','128 B','64 B','32 B'];
function o2(s,a){s.innerHTML=a.map(function(v,i){return '<option value='+i+'>'+v+'</option>'}).join('')}
o2($('e2air'),AIR);o2($('e2ub'),UBP);o2($('e2pwr'),PWR);o2($('e2sub'),SUBP);
function e2f(){$('e2freq').textContent=(850.125+(+$('e2ch').value||0)).toFixed(3)+' MHz'}
function e2set(d){$('e2ok').textContent=d.ok?'read OK':'no response — check wiring & M0/M1/AUX';
$('e2addr').value=d.addr;$('e2ch').value=d.ch;$('e2air').value=d.air;$('e2ub').value=d.ubaud;
$('e2pwr').value=d.pwr;$('e2sub').value=d.sub;$('e2fx').checked=!!d.fx;$('e2rb').checked=!!d.rb;
$('e2m0').value=d.m0;$('e2m1').value=d.m1;$('e2aux').value=d.aux;e2f()}
function e2read(){fetch('/loracfg').then(function(r){return r.json()}).then(e2set)}
function e2write(){var p=new URLSearchParams();p.set('write','1');
p.set('addr',$('e2addr').value||0);p.set('ch',$('e2ch').value||0);p.set('air',$('e2air').value);
p.set('ubaud',$('e2ub').value);p.set('pwr',$('e2pwr').value);p.set('sub',$('e2sub').value);
p.set('par','0');p.set('rn','0');p.set('rb',$('e2rb').checked?1:0);p.set('fx',$('e2fx').checked?1:0);
p.set('key',$('e2key').value||0);p.set('m0',$('e2m0').value);p.set('m1',$('e2m1').value);p.set('aux',$('e2aux').value);
fetch('/loracfg?'+p.toString()).then(function(r){return r.json()}).then(e2set)}
function saveWifi(){var d=new URLSearchParams();d.set('stassid',$('ss').value);d.set('stapass',$('sp').value);
d.set('apssid',$('as').value);d.set('appass',$('ap').value);fetch('/wifi',{method:'POST',body:d})}
function refresh(){return fetch('/status').then(function(r){return r.json()}).then(function(s){
['gps','lora','rover','hybrid'].forEach(function(m){$('m'+m).className=(s.mode==m?'active':'')});
var fix=['no fix','GPS','DGPS','PPS','RTK fixed','RTK float','dead-reckoning'][s.fix];if(fix===undefined)fix=s.fix;
$('status').innerHTML='<div>Mode</div><div>'+s.mode+'</div>'+
'<div>STA</div><div>'+s.sta+' @ '+s.staip+' ('+s.rssi+' dBm)</div>'+
'<div>AP clients</div><div>'+s.apclients+'</div>'+
'<div>GPS fix</div><div>'+fix+', '+s.sats+' sats used</div>'+
'<div>GPS used</div><div>'+(brk(s.gpsbyc,GNAMES)||'&mdash;')+'</div>'+
'<div>GPS baud</div><div>'+s.gpsbaud+' (GPIO'+s.gtx+'/'+s.grx+')</div>'+
'<div>LoRa baud</div><div>'+s.lorabaud+' (GPIO'+s.ltx+'/'+s.lrx+')</div>'+
'<div>USB rx/tx</div><div>'+s.cdcrx+' / '+s.cdctx+'</div>'+
'<div>GPS rx/tx</div><div>'+s.gpsrx+' / '+s.gpstx+'</div>'+
'<div>LoRa rx/tx</div><div>'+s.lorarx+' / '+s.loratx+'</div>'+
'<div>LoRa RTCM</div><div>'+s.rtcmcount+' valid'+(s.rtcmtype?' (last type '+s.rtcmtype+')':'')+'</div>'+
'<div>RTCM sats</div><div>'+(s.rtcmsats?s.rtcmsats+' total &middot; '+brk(s.rtcmbyc,RNAMES):'&mdash;')+'</div>'+
'<div>USB CDC</div><div>'+(s.cdcconn?'connected':'<span style="color:#e88">no DTR &mdash; use TCP &rarr;</span>')+'</div>'+
'<div>TCP :10110</div><div>'+(s.tcp?'client connected':'listening (DTR-free option)')+'</div>';
if(!window._i){window._i=1;$('gb').value=s.gpsbaud;$('lbsel').value=s.lorabaud;
$('gtx').value=s.gtx;$('grx').value=s.grx;$('ltx').value=s.ltx;$('lrx').value=s.lrx;
$('ss').value=s.sta;$('as').value=s.ap;}})}
setInterval(refresh,1500);refresh();
</script></body></html>)HTML";
#endif

static String fsContentType(const String& p)
{
    if(p.endsWith(".html")) return "text/html";
    if(p.endsWith(".css"))  return "text/css";
    if(p.endsWith(".js"))   return "application/javascript";
    if(p.endsWith(".json")) return "application/json";
    if(p.endsWith(".svg"))  return "image/svg+xml";
    if(p.endsWith(".png"))  return "image/png";
    if(p.endsWith(".ico"))  return "image/x-icon";
    return "text/plain";
}

// Serve a file from SPIFFS, preferring a .gz twin (streamFile auto-adds gzip encoding).
static bool fsServe(String path)
{
    int qs = path.indexOf('?'); if(qs >= 0) path = path.substring(0, qs);
    if(path.endsWith("/")) path += "index.html";
    String ct = fsContentType(path);
    bool longCache = path.endsWith(".png") || path.endsWith(".svg") || path.endsWith(".ico");
    String gz = path + ".gz";
    String use = SPIFFS.exists(gz) ? gz : (SPIFFS.exists(path) ? path : String());
    if(use.length() == 0) return false;
    File f = SPIFFS.open(use, "r");
    server.sendHeader("Cache-Control", longCache ? "public, max-age=86400" : "no-cache");
    server.streamFile(f, ct);
    f.close();
    return true;
}

static void handleRoot()     { if(!fsServe("/index.html")) server.send(200, "text/plain", "UI not flashed — run: pio run -e rtk1010_web_ota -t uploadfs"); }
static void handleNotFound() { if(!fsServe(server.uri())) server.send(404, "text/plain", "Not found"); }

static void handleStatus()
{
    int rtcmTotSats = 0, rtcmCons = 0;
    for(int ci = 0; ci < 8; ci++) { rtcmTotSats += rtcmSatByConst[ci]; if(rtcmSatByConst[ci] > 0) rtcmCons++; }

    static char j[1200];
    snprintf(j, sizeof(j),
        "{\"mode\":\"%s\",\"sta\":\"%s\",\"ap\":\"%s\",\"staip\":\"%s\",\"rssi\":%d,\"apclients\":%d,"
        "\"up\":%lu,\"heap\":%u,\"minheap\":%u,"
        "\"fix\":%d,\"fixmode\":%d,\"sats\":%d,\"hdop\":%.2f,\"gpshz\":%d,\"lat\":%.7f,\"lon\":%.7f,\"lorabaud\":%u,\"gpsbaud\":%u,"
        "\"gtx\":%d,\"grx\":%d,\"ltx\":%d,\"lrx\":%d,\"cdcconn\":%d,\"cdcavail\":%u,\"tcp\":%d,"
        "\"rtcmcount\":%u,\"rtcmtype\":%d,\"rtcmsats\":%d,\"rtcmcons\":%d,"
        "\"rtcmbyc\":[%d,%d,%d,%d,%d,%d,%d],\"gpsbyc\":[%d,%d,%d,%d,%d,%d],"
        "\"em0\":%d,\"em1\":%d,\"eaux\":%d,"
        "\"cdcrx\":%u,\"cdctx\":%u,\"gpsrx\":%u,\"gpstx\":%u,\"lorarx\":%u,\"loratx\":%u}",
        modeName(g_mode), staSsid.c_str(), apSsid.c_str(),
        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (int)WiFi.softAPgetStationNum(),
        (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
        gpsFixQual, gpsFixMode, gpsSats, gpsHdop, gpsHz, gpsLat, gpsLon, g_loraBaud, g_gpsBaud,
        gpsTx, gpsRx, loraTx, loraRx, (int)tud_cdc_n_connected(0), tud_cdc_n_write_available(0), (int)navClient.connected(),
        rtcmCount, rtcmType, rtcmTotSats, rtcmCons,
        rtcmSatByConst[0], rtcmSatByConst[1], rtcmSatByConst[2], rtcmSatByConst[3],
        rtcmSatByConst[4], rtcmSatByConst[5], rtcmSatByConst[6],
        gpsSatByConst[0], gpsSatByConst[1], gpsSatByConst[2], gpsSatByConst[3], gpsSatByConst[4], gpsSatByConst[5],
        e220M0, e220M1, e220Aux,
        cntCdcRx, cntCdcTx, cntGpsRx, cntGpsTx, cntLoraRx, cntLoraTx);
    server.send(200, "application/json", j);
}

// Per-satellite signal list for the dashboard signal bars: {v=svid, c=C/N0, s=sys, u=used}.
static void handleSats()
{
    uint32_t now = millis();
    static char j[2600];
    int p = snprintf(j, sizeof(j), "{\"sats\":[");
    bool first = true;
    for(int i = 0; i < gSatN && p < (int)sizeof(j) - 48; i++) {
        if((uint32_t)(now - gSat[i].seen) > 5000) continue;            // drop stale entries
        int used = ((uint32_t)(now - gSat[i].usedAt) < 3000) ? 1 : 0;
        p += snprintf(j + p, sizeof(j) - p, "%s{\"v\":%u,\"c\":%u,\"s\":%u,\"u\":%d}",
                      first ? "" : ",", gSat[i].svid, gSat[i].cno, gSat[i].sys, used);
        first = false;
    }
    snprintf(j + p, sizeof(j) - p, "]}");
    server.send(200, "application/json", j);
}

// Full histogram of RTCM message types seen on LoRa — to inspect the whole stream.
static void handleRtcmStats()
{
    static char j[1400];
    int p = snprintf(j, sizeof(j), "{\"frames\":%u,\"types\":[", rtcmCount);
    for(int i = 0; i < rtcmHistN && p < (int)sizeof(j) - 32; i++)
        p += snprintf(j + p, sizeof(j) - p, "%s{\"t\":%d,\"n\":%u}", i ? "," : "", rtcmHistType[i], rtcmHistCnt[i]);
    snprintf(j + p, sizeof(j) - p, "]}");
    server.send(200, "application/json", j);
}

static void handleMode()
{
    String s = server.arg("set");
    if(s == "gps") g_mode = MODE_GPS;
    else if(s == "lora") g_mode = MODE_LORA;
    else if(s == "rover") g_mode = MODE_ROVER;
    else if(s == "hybrid") g_mode = MODE_HYBRID;
    prefs.putUChar("mode", (uint8_t)g_mode);
    server.send(200, "text/plain", "ok");
}

// Send a LOCOSYS/NMEA command to the RTK-1010 (wraps body as $BODY*CS\r\n).
static void gpsNmeaSend(const char* body)
{
    uint8_t cs = 0;
    for(const char* p = body; *p; p++) cs ^= (uint8_t)*p;
    char line[180];
    int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
    if(n > 0) uart_write_bytes(GPS_UART, line, n);
}

// E220 register read/write. With ?write=1 + fields, programs the module; else reads.
static void handleLoraCfg()
{
    auto setPin = [](const char* a, int& var, const char* key, bool out) {
        if(server.hasArg(a)) { int v = server.arg(a).toInt(); if(v >= 0 && v <= 46) { var = v; if(out) { pinMode(v, OUTPUT); digitalWrite(v, LOW); } else pinMode(v, INPUT); prefs.putInt(key, v); } }
    };
    setPin("m0", e220M0, "e220m0", true);
    setPin("m1", e220M1, "e220m1", true);
    setPin("aux", e220Aux, "e220aux", false);

    if(server.hasArg("write")) {
        uint16_t addr = (uint16_t)server.arg("addr").toInt();
        int ub = server.arg("ubaud").toInt() & 7, par = server.arg("par").toInt() & 3, air = server.arg("air").toInt() & 7;
        int sub = server.arg("sub").toInt() & 3, rn = server.arg("rn").toInt() & 1, pwr = server.arg("pwr").toInt() & 3;
        int ch = server.arg("ch").toInt() & 0xFF, rb = server.arg("rb").toInt() & 1, fx = server.arg("fx").toInt() & 1;
        int lbt = server.arg("lbt").toInt() & 1, wor = server.arg("wor").toInt() & 7;
        uint16_t key = (uint16_t)server.arg("key").toInt();
        e220Regs[0] = addr >> 8; e220Regs[1] = addr & 0xFF;
        e220Regs[2] = (ub << 5) | (par << 3) | air;              // SPED
        e220Regs[3] = (sub << 6) | (rn << 5) | pwr;              // OPTION
        e220Regs[4] = ch;                                        // CHAN
        e220Regs[5] = (rb << 7) | (fx << 6) | (lbt << 4) | wor;  // TRANSMISSION_MODE: RSSI|fixed|LBT|WORcycle
        e220Regs[6] = key >> 8; e220Regs[7] = key & 0xFF;        // CRYPT (write-only)
        e220Req = 2;
    } else {
        e220Req = 1;
    }
    e220Busy = true;
    uint32_t t = millis();
    while(e220Busy && (uint32_t)(millis() - t) < 2500) delay(10);

    uint16_t addr = (e220Regs[0] << 8) | e220Regs[1];
    char j[320];
    snprintf(j, sizeof(j),
        "{\"ok\":%d,\"addr\":%u,\"ubaud\":%d,\"par\":%d,\"air\":%d,\"sub\":%d,\"rn\":%d,\"pwr\":%d,"
        "\"ch\":%d,\"freq\":\"%.3f\",\"rb\":%d,\"fx\":%d,\"lbt\":%d,\"wor\":%d,\"m0\":%d,\"m1\":%d,\"aux\":%d}",
        e220Ok ? 1 : 0, addr,
        (e220Regs[2] >> 5) & 7, (e220Regs[2] >> 3) & 3, e220Regs[2] & 7,
        (e220Regs[3] >> 6) & 3, (e220Regs[3] >> 5) & 1, e220Regs[3] & 3,
        e220Regs[4], 850.125 + e220Regs[4], (e220Regs[5] >> 7) & 1, (e220Regs[5] >> 6) & 1,
        (e220Regs[5] >> 4) & 1, e220Regs[5] & 7,
        e220M0, e220M1, e220Aux);
    server.send(200, "application/json", j);
}

static void handleGpsCmd()
{
    String c = server.arg("cmd");
    if(c.length()) {
        if(c[0] == '$') c.remove(0, 1);          // accept with or without leading $
        int star = c.indexOf('*'); if(star >= 0) c.remove(star);  // drop any *checksum
        c.trim();
        if(c.length()) gpsNmeaSend(c.c_str());
    }
    server.send(200, "text/plain", "sent");
}

// Change the RTK-1010 port baud the right way: PAIR864 only takes effect after a
// module reboot (per the LOCOSYS command list), so we tell the GPS + save it, then
// power-cycle the module via RTK_EN, then switch the ESP UART to match. Changing the
// ESP baud alone (as /ports does) just breaks the link until the GPS is rebooted.
static void handleGpsBaud()
{
    uint32_t b = (uint32_t)server.arg("baud").toInt();
    static const uint32_t kOk[] = { 115200, 230400, 460800, 921600, 3000000 };
    bool ok = false; for(uint32_t v : kOk) if(v == b) ok = true;
    if(!ok) { server.send(400, "text/plain", "unsupported baud"); return; }

    char body[40];
    snprintf(body, sizeof(body), "PAIR864,0,0,%u", (unsigned)b);
    gpsNmeaSend(body);                              // set port baud (at the current baud)
    delay(60);
    gpsNmeaSend("PAIR513");                          // persist so it survives the reboot
    uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(300));
    delay(120);

    digitalWrite(RTK_EN_PIN, LOW);                   // reboot the module so the new baud applies
    delay(250);
    digitalWrite(RTK_EN_PIN, HIGH);

    g_gpsBaud = b; uart_set_baudrate(GPS_UART, b);   // match the ESP UART + persist
    prefs.putUInt("gpsbaud", b);
    server.send(200, "text/plain", "ok");
}

// Persist GPS settings to flash. PAIR513 alone silently fails at multi-Hz, so power the
// GNSS off, save, then power it back on. Verified on this module: PAIR003 = power OFF,
// PAIR002 = power ON (opposite of the usual naming). Briefly stops GPS output (~1 s).
static void handleGpsSave()
{
    gpsNmeaSend("PAIR003"); uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(200)); delay(350);  // GNSS power off
    gpsNmeaSend("PAIR513"); uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(200)); delay(350);  // save RTC RAM -> flash
    gpsNmeaSend("PAIR002");                                                                // GNSS power on
    server.send(200, "text/plain", "ok");
}

// Restore the RTK-1010 to factory defaults (PAIR514). Like save, it needs the GNSS
// powered off first. Defaults may reset the module's port baud, so afterwards we
// auto-resync the ESP UART by trying candidate bauds until GGA sentences reappear.
static void handleGpsReset()
{
    gpsNmeaSend("PAIR003"); uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(200)); delay(350);  // power off
    gpsNmeaSend("PAIR514"); uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(200)); delay(400);  // restore defaults
    gpsNmeaSend("PAIR002");                                                                // power on
    delay(600);
    const uint32_t cand[] = { g_gpsBaud, 115200, 460800, 230400, 9600, 38400, 921600 };
    for(uint32_t b : cand) {
        uart_set_baudrate(GPS_UART, b);
        uart_flush_input(GPS_UART);
        uint32_t c0 = gpsGgaCount, t = millis();
        while((uint32_t)(millis() - t) < 700) delay(20);   // router task drains+parses meanwhile
        if(gpsGgaCount > c0 + 1) { g_gpsBaud = b; prefs.putUInt("gpsbaud", b); break; }
    }
    server.send(200, "text/plain", String("ok, gps baud ") + g_gpsBaud);
}

// Live stream tap: /console?src=gps|lora|host|resp&since=N -> raw bytes since N, X-Next header = new cursor.
static void handleConsole()
{
    String s = server.arg("src");
    Ring* r = (s == "lora") ? &rLora : (s == "host") ? &rHost : (s == "resp") ? &rResp : &rGps;
    uint32_t since = (uint32_t)strtoul(server.arg("since").c_str(), NULL, 10);
    uint32_t h = r->head;
    uint32_t start = since;
    if(start > h || h - start > 2048) start = (h > 2048) ? h - 2048 : 0;  // first call / fell behind
    uint32_t len = h - start;
    static uint8_t tmp[2048];
    for(uint32_t i = 0; i < len; i++) tmp[i] = r->buf[(start + i) & 2047];
    server.sendHeader("X-Next", String(h));
    server.setContentLength(len);
    server.send(200, "application/octet-stream", "");
    if(len) server.sendContent((const char*)tmp, len);
}

// Live per-port config: baud (uart_set_baudrate) and pins (uart_set_pin) can be
// changed on an installed driver without reinstalling it.
static void handlePorts()
{
    if(server.hasArg("gpsbaud")) {
        uint32_t b = (uint32_t)server.arg("gpsbaud").toInt();
        if(b >= 1200 && b <= 2000000) { g_gpsBaud = b; uart_set_baudrate(GPS_UART, b); prefs.putUInt("gpsbaud", b); }
    }
    if(server.hasArg("lorabaud")) {
        uint32_t b = (uint32_t)server.arg("lorabaud").toInt();
        if(b > LORA_MAX_BAUD) b = LORA_MAX_BAUD;
        if(b >= 1200) { g_loraBaud = b; uart_set_baudrate(LORA_UART, b); prefs.putUInt("lorabaud", b); }
    }
    if(server.hasArg("gtx") && server.hasArg("grx")) {
        int tx = server.arg("gtx").toInt(), rx = server.arg("grx").toInt();
        if(tx >= 0 && tx <= 46 && rx >= 0 && rx <= 46 && tx != rx) {
            gpsTx = tx; gpsRx = rx; uart_set_pin(GPS_UART, tx, rx, -1, -1);
            prefs.putInt("gpstx", tx); prefs.putInt("gpsrx", rx);
        }
    }
    if(server.hasArg("ltx") && server.hasArg("lrx")) {
        int tx = server.arg("ltx").toInt(), rx = server.arg("lrx").toInt();
        if(tx >= 0 && tx <= 46 && rx >= 0 && rx <= 46 && tx != rx) {
            loraTx = tx; loraRx = rx; uart_set_pin(LORA_UART, tx, rx, -1, -1);
            prefs.putInt("loratx", tx); prefs.putInt("lorarx", rx);
        }
    }
    server.send(200, "text/plain", "ok");
}

static void handleWifi()
{
    if(server.hasArg("stassid")) staSsid = server.arg("stassid");
    if(server.hasArg("stapass")) staPass = server.arg("stapass");
    if(server.hasArg("apssid"))  apSsid  = server.arg("apssid");
    if(server.hasArg("appass"))  apPass  = server.arg("appass");
    prefs.putString("stassid", staSsid);
    prefs.putString("stapass", staPass);
    prefs.putString("apssid", apSsid);
    prefs.putString("appass", apPass);
    server.send(200, "text/plain", "saved, rebooting");
    delay(300);
    ESP.restart();
}

static void handleReboot()
{
    server.send(200, "text/plain", "rebooting");
    delay(300);
    ESP.restart();
}

// Reboot into the ROM USB download mode (for a USB esptool flash, e.g. a partition change).
static void handleDownloadMode()
{
    server.send(200, "text/plain", "Entering USB download mode — run the USB flash now.");
    delay(300);
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

// Browser OTA (app firmware only) — mirrors the example's Update-based handler.
static void handleUpdateUpload()
{
    HTTPUpload& up = server.upload();
    if(up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
    else if(up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
    else if(up.status == UPLOAD_FILE_END) Update.end(true);
}

// ---------------------------------------------------------------- setup/loop
void setup()
{
    Serial.begin(115200); // USB-CDC (baud is virtual)

    // Clear any leftover force-download flag so normal resets boot the app.
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    SPIFFS.begin(true);  // mount web assets (format if absent); upload via `pio ... -t uploadfs`

    // Release the RTK-1010 from reset (EN active high).
    pinMode(RTK_EN_PIN, OUTPUT);
    digitalWrite(RTK_EN_PIN, LOW);
    delay(100);
    digitalWrite(RTK_EN_PIN, HIGH);

    prefs.begin("rtk", false);
    g_mode     = (RouteMode)prefs.getUChar("mode", MODE_GPS);
    g_loraBaud = prefs.getUInt("lorabaud", LORA_DEFAULT_BAUD);
    g_gpsBaud  = prefs.getUInt("gpsbaud", GPS_DEFAULT_BAUD);
    gpsTx  = prefs.getInt("gpstx", GPS_TX_DEF);
    gpsRx  = prefs.getInt("gpsrx", GPS_RX_DEF);
    loraTx = prefs.getInt("loratx", LORA_TX_DEF);
    loraRx = prefs.getInt("lorarx", LORA_RX_DEF);
    e220M0 = prefs.getInt("e220m0", E220_M0_DEF);
    e220M1 = prefs.getInt("e220m1", E220_M1_DEF);
    e220Aux = prefs.getInt("e220aux", E220_AUX_DEF);
    staSsid = prefs.getString("stassid", staSsid);
    staPass = prefs.getString("stapass", staPass);
    apSsid  = prefs.getString("apssid", apSsid);
    apPass  = prefs.getString("appass", apPass);

    uartInit(GPS_UART, gpsTx, gpsRx, g_gpsBaud);
    uartInit(LORA_UART, loraTx, loraRx, g_loraBaud);

    // E220 mode pins (default transparent: M0=0, M1=0).
    pinMode(e220M0, OUTPUT); digitalWrite(e220M0, LOW);
    pinMode(e220M1, OUTPUT); digitalWrite(e220M1, LOW);
    if(e220Aux >= 0) pinMode(e220Aux, INPUT);

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.softAP(apSsid.c_str(), apPass.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    MDNS.begin(HOSTNAME);

    // Raw NMEA/RTCM TCP server (port 10110) — lets DTR-less apps read over WiFi.
    navServer.begin();
    navServer.setNoDelay(true);

    // Network OTA (espota) — `pio run -e rtk1010_web_ota -t upload` flashes over
    // WiFi, so no USB/COM-port dance after this first flash.
    ArduinoOTA.setHostname(HOSTNAME);
    // ArduinoOTA.setPassword("rtk1010ota");  // uncomment + set upload_flags=--auth to require a password
    ArduinoOTA.begin();

    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/rtcmstats", handleRtcmStats);
    server.on("/sats", handleSats);
    server.on("/mode", handleMode);
    server.on("/ports", handlePorts);
    server.on("/gpscmd", handleGpsCmd);
    server.on("/gpsbaud", handleGpsBaud);
    server.on("/gpssave", handleGpsSave);
    server.on("/gpsreset", handleGpsReset);
    server.on("/console", handleConsole);
    server.on("/loracfg", handleLoraCfg);
    server.on("/wifi", HTTP_POST, handleWifi);
    server.on("/reboot", handleReboot);
    server.on("/download", handleDownloadMode);
    server.onNotFound(handleNotFound);   // static files from SPIFFS
    server.on("/update", HTTP_POST,
        []() { server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK"); delay(300); ESP.restart(); },
        handleUpdateUpload);
    server.begin();

    // Data pump on core 0; the web server runs from loop() on core 1.
    xTaskCreatePinnedToCore(routerTask, "router", 4096, NULL, 10, NULL, 0);
}

void loop()
{
    server.handleClient();
    ArduinoOTA.handle();
    delay(1);
}
