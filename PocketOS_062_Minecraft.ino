/*
  PocketOS 0.6.1 - light, phone-style UI for the ESP32-2432S028 "CYD"
  (2.8" 240x320, ILI9341 + XPT2046) using LovyanGFX (install from Library Manager).
  Board: ESP32 Dev Module.

  - Wallpapers drawn in code (4 to choose from), glossy app icons, dock, status bar
  - NTP clock (Europe/Berlin), countdown timer, stopwatch, lightweight HTTP text browser
  - NEW: swipe left/right on the home screen to change page (dots show the page)
  - NEW: Flappy bird and Tic-Tac-Toe (vs CPU)
  - Shared posts removed
  - NEW: Minecraft-style block builder app (landscape, page 2 of the home screen)
  - 1 s tap cooldown after closing an app (no accidental tap-through)
  - No framebuffer: everything is drawn straight to the display (tiny RAM use)
*/
#include <Arduino.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>

// ---------- CYD hardware config ----------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;
public:
  LGFX(void) {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = false;      // colors inverted? set to true
      cfg.rgb_order = false;   // red/blue swapped? set to true
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 3700;
      cfg.y_max = 200;
      cfg.pin_int = 36;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI3_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;

// microSD / TF slot (common ESP32-2432S028 CYD pinout)
SPIClass sdSPI(HSPI);
static bool sdReady = false;
static String sdCurrentPath = "/";
static String sdEntryName[4];
static bool sdEntryIsDir[4];
static uint32_t sdEntrySize[4];
static int sdEntryCount = 0;
static const int SD_CS_PIN = 5;
static const int SD_SCK_PIN = 18;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;
static Preferences prefs;

// ---------- network configuration ----------
// Fill these in with your own 2.4 GHz Wi-Fi details (needed for clock sync + browser).
static const char* WIFI_SSID = "FRITZ!Box 7560 JW";
static const char* WIFI_PASSWORD = "43056257095358603413";
static bool wifiReady = false;
static String browserUrl = "http://example.com";
static String browserText = "Enter an HTTP URL, then tap Load.";
static bool browserEditing = false;
static String browserLinks[3];
static int browserLinkCount = 0;
static unsigned long timerStartedAt = 0;
static unsigned long timerRemainingMs = 60000UL;
static bool timerRunning = false;
static unsigned long stopwatchAccumulatedMs = 0;
static unsigned long stopwatchStartedAt = 0;
static bool stopwatchRunning = false;
static unsigned long lastFeatureRedraw = 0;

// ---------- colors (0xRRGGBB) ----------
static const uint32_t L_BG=0xF2F2F7, L_CARD=0xFFFFFF, L_TEXT=0x1C1C1E, L_MUTED=0x8E8E93,
  L_LINE=0xD1D1D6, C_WHITE=0xFFFFFF, C_BLACK=0x000000, C_RED=0xFF3B30, C_GREEN=0x34C759;
static const uint32_t ACC[5]={0x0A84FF,0xAF52DE,0x34C759,0xFF9500,0xFF2D55};
static const uint32_t WALL_TOP[4]={0x4DA3FF,0xFFB36B,0xA8F0C6,0xC9B6FF};
static const uint32_t WALL_BOT[4]={0x1250D6,0xE0457B,0x2FAE8F,0x6A4CE0};
static const char* const WALL_NAME[4]={"Ocean","Sunset","Mint","Lavender"};
static const int BR_P[4]={25,50,75,100};
static const int BR_V[4]={64,128,192,255};

#define F_S  (&lgfx::fonts::Font2)
#define F_M  (&lgfx::fonts::FreeSansBold9pt7b)
#define F_L  (&lgfx::fonts::FreeSansBold12pt7b)
#define F_XL (&lgfx::fonts::FreeSansBold24pt7b)

// ---------- state ----------
String page="lock", pinCode="1234", entered="", dial="";
bool pinEnabled=true, labelsOn=true, clockOn=true, firstRun=true;
int wall=0, accentIdx=0, brIdx=3, setScroll=0, homePage=0;
const int HOME_PAGES=2;
unsigned long lastTap=0, lastClock=0, lastMin=0, resetArm=0, homeLockUntil=0;
String cDisp="0"; double cAcc=0; char cOp=0; bool cNew=true;
int fBest=0;

uint32_t accent(){ return ACC[accentIdx]; }

static uint32_t mix(uint32_t a,uint32_t b,int t){
  int ar=(a>>16)&255, ag=(a>>8)&255, ab=a&255, br=(b>>16)&255, bg=(b>>8)&255, bb=b&255;
  int r=ar+(br-ar)*t/255, g=ag+(bg-ag)*t/255, bl=ab+(bb-ab)*t/255;
  return ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)bl;
}

// ---------- text helpers ----------
template<class F> void txt(int x,int y,const char* s,uint32_t col,const F* f,int al=0){
  tft.setFont(f); tft.setTextColor(col);
  int w=tft.textWidth(s);
  if(al==1) x-=w/2; else if(al==2) x-=w;
  tft.drawString(s,x,y);
}
template<class F> void txtc(int cx,int cy,const char* s,uint32_t col,const F* f){
  tft.setFont(f); tft.setTextColor(col);
  int w=tft.textWidth(s), h=tft.fontHeight();
  tft.drawString(s,cx-w/2,cy-h/2);
}
template<class F> void txtcS(int cx,int cy,const char* s,uint32_t col,const F* f){ // with shadow (for wallpaper)
  txtc(cx+1,cy+1,s,0x14284B,f);
  txtc(cx,cy,s,col,f);
}

void localTimeStr(char* b,size_t n){
  struct tm ti;
  if(getLocalTime(&ti, 10)) strftime(b,n,"%H:%M",&ti);
  else { unsigned long sec=millis()/1000; snprintf(b,n,"%02lu:%02lu",sec/3600,(sec/60)%60); }
}
void uptimeStr(char* b,size_t n){ localTimeStr(b,n); }
bool hit(int x,int y,int a,int b,int w,int h){ return x>=a && x<a+w && y>=b && y<b+h; }

// ---------- settings storage ----------
void save(){
  prefs.begin("pocketos",false);
  prefs.putInt("wall",wall); prefs.putInt("acc",accentIdx); prefs.putInt("br",brIdx);
  prefs.putBool("pinon",pinEnabled); prefs.putBool("lbl",labelsOn); prefs.putBool("clk",clockOn);
  prefs.putBool("first",firstRun); prefs.putString("pin",pinCode);
  prefs.end();
}
void load(){
  prefs.begin("pocketos",true);
  wall=constrain(prefs.getInt("wall",0),0,3); accentIdx=constrain(prefs.getInt("acc",0),0,4);
  brIdx=constrain(prefs.getInt("br",3),0,3);
  pinEnabled=prefs.getBool("pinon",true); labelsOn=prefs.getBool("lbl",true);
  clockOn=prefs.getBool("clk",true); firstRun=prefs.getBool("first",true);
  pinCode=prefs.getString("pin","1234");
  fBest=prefs.getInt("fbest",0);
  prefs.end();
}

// ---------- wallpaper (gradient + light rays, drawn row by row) ----------
uint32_t wallColorAt(int y){ return mix(WALL_TOP[wall],WALL_BOT[wall],y*255/319); }

void wallRows(int y0,int y1){
  static float ts[6], te[6];
  static bool init=false;
  if(!init){
    for(int k=0;k<6;k++){
      float a=(-60+k*20)*0.0174533f;
      ts[k]=tanf(a); te[k]=tanf(a+10*0.0174533f);
    }
    init=true;
  }
  if(y0<0) y0=0;
  if(y1>320) y1=320;
  for(int y=y0;y<y1;y++){
    uint32_t c=wallColorAt(y);
    tft.drawFastHLine(0,y,240,c);
    uint32_t rc=mix(c,C_WHITE,34);
    int dy=335-y;
    for(int k=0;k<6;k++){
      int x1=120+(int)(dy*ts[k]), x2=120+(int)(dy*te[k]);
      if(x1<0) x1=0;
      if(x2>239) x2=239;
      if(x2>x1) tft.drawFastHLine(x1,y,x2-x1+1,rc);
    }
  }
}

// ---------- icons ----------
enum { I_PHONE, I_MSG, I_CAM, I_SET, I_PHOTO, I_CLOCK, I_CALC, I_FILES, I_WEATHER, I_MUSIC, I_TERM, I_LOCK, I_INFO,
       I_BROWSER, I_TIMER, I_STOPWATCH, I_FLAPPY, I_TTT, I_MC };
static const uint32_t IC[19]={0x30C85E,0x2AD36A,0x8E8E93,0x8E8E93,0xFFFFFF,0x2C2C2E,0xFF9500,0x2F8CFF,0x4DB2FF,0xAF52DE,0x1C1C1E,0xFF3B30,0x5AC8FA,
                              0x0A84FF,0xFF9500,0xAF52DE,0x4DC3FF,0xFF375F,0x5B8DEF};

static int kk(int v,int sz){ int r=v*sz/48; return r<1?1:r; }
#define K(v) kk((v),sz)

void drawIcon(int id,int x,int y,int sz){
  uint32_t b=IC[id], w=C_WHITE;
  int r=sz/4;
  uint32_t hi=mix(b,C_WHITE,90), mid=mix(b,C_WHITE,45), lo=mix(b,C_BLACK,28);
  for(int i=0;i<sz;i++){            // glossy vertical gradient inside a rounded square
    int inset=0;
    if(i<r){ float dy=r-i-0.5f; inset=r-(int)sqrtf((float)r*r-dy*dy+0.5f); }
    else if(i>=sz-r){ float dy=i-(sz-r)+0.5f; inset=r-(int)sqrtf((float)r*r-dy*dy+0.5f); }
    int t=i*255/(sz-1);
    uint32_t c = t<128 ? mix(hi,mid,t*2) : mix(b,lo,(t-128)*2);
    tft.drawFastHLine(x+inset,y+i,sz-2*inset,c);
  }
  tft.drawRoundRect(x,y,sz,sz,r,mix(b,C_WHITE,110));
  int cx=x+sz/2, cy=y+sz/2;
  switch(id){
    case I_PHONE:
      tft.drawWideLine(cx-K(10),cy+K(10),cx+K(10),cy-K(10),K(4),w);
      tft.fillCircle(cx-K(10),cy+K(10),K(6),w);
      tft.fillCircle(cx+K(10),cy-K(10),K(6),w);
      break;
    case I_MSG:
      tft.fillRoundRect(cx-K(17),cy-K(14),K(34),K(24),K(8),w);
      tft.fillTriangle(cx-K(10),cy+K(8),cx-K(3),cy+K(8),cx-K(12),cy+K(16),w);
      break;
    case I_CAM:
      tft.fillRoundRect(cx-K(17),cy-K(9),K(34),K(24),K(4),w);
      tft.fillRect(cx-K(8),cy-K(13),K(10),K(5),w);
      tft.fillCircle(cx,cy+K(3),K(8),b);
      tft.fillCircle(cx,cy+K(3),K(5),w);
      break;
    case I_SET:
      for(int a=0;a<8;a++){
        float t=a*0.785398f;
        tft.fillCircle(cx+(int)(cosf(t)*K(12)),cy+(int)(sinf(t)*K(12)),K(3),w);
      }
      tft.fillCircle(cx,cy,K(10),w);
      tft.fillCircle(cx,cy,K(4),b);
      break;
    case I_PHOTO: {
      static const uint32_t pc[8]={0xFF3B30,0xFF9500,0xFFCC00,0x34C759,0x30B0C7,0x0A84FF,0xAF52DE,0xFF2D55};
      for(int a=0;a<8;a++){
        float t=a*0.785398f;
        tft.fillCircle(cx+(int)(cosf(t)*K(9)),cy+(int)(sinf(t)*K(9)),K(7),pc[a]);
      }
      tft.fillCircle(cx,cy,K(4),w);
      break; }
    case I_CLOCK:
      tft.fillCircle(cx,cy,K(18),w);
      tft.drawWideLine(cx,cy,cx,cy-K(12),1.5f,L_TEXT);
      tft.drawWideLine(cx,cy,cx+K(8),cy+K(3),1.5f,L_TEXT);
      tft.fillCircle(cx,cy,K(2),0xFF3B30);
      break;
    case I_CALC:
      tft.fillRoundRect(cx-K(15),cy-K(18),K(30),K(10),K(2),w);
      for(int rr=0;rr<2;rr++)
        for(int cc=0;cc<3;cc++)
          tft.fillRoundRect(cx-K(15)+cc*K(11),cy-K(4)+rr*K(11),K(8),K(8),K(2),w);
      break;
    case I_FILES:
      tft.fillRoundRect(cx-K(17),cy-K(13),K(14),K(8),K(2),w);
      tft.fillRoundRect(cx-K(17),cy-K(9),K(34),K(24),K(3),w);
      break;
    case I_WEATHER:
      tft.fillCircle(cx+K(7),cy-K(7),K(9),0xFFD60A);
      tft.fillCircle(cx-K(9),cy+K(7),K(7),w);
      tft.fillCircle(cx+K(1),cy+K(3),K(10),w);
      tft.fillCircle(cx+K(11),cy+K(8),K(6),w);
      tft.fillRect(cx-K(9),cy+K(8),K(20),K(6),w);
      break;
    case I_MUSIC:
      for(int a=180;a<=360;a+=15){
        float t=a*0.0174533f;
        tft.fillCircle(cx+(int)(cosf(t)*K(14)),cy+K(2)+(int)(sinf(t)*K(14)),K(2),w);
      }
      tft.fillRoundRect(cx-K(18),cy+K(1),K(9),K(15),K(3),w);
      tft.fillRoundRect(cx+K(9),cy+K(1),K(9),K(15),K(3),w);
      break;
    case I_TERM:
      tft.drawWideLine(cx-K(13),cy-K(8),cx-K(5),cy,1.5f,0x30D158);
      tft.drawWideLine(cx-K(5),cy,cx-K(13),cy+K(8),1.5f,0x30D158);
      tft.fillRect(cx+K(1),cy+K(7),K(12),K(3),0x30D158);
      break;
    case I_LOCK:
      tft.fillRoundRect(cx-K(8),cy-K(17),K(16),K(18),K(7),w);
      tft.fillRoundRect(cx-K(4),cy-K(13),K(8),K(14),K(3),b);
      tft.fillRoundRect(cx-K(12),cy-K(3),K(24),K(19),K(4),w);
      tft.fillCircle(cx,cy+K(6),K(2),b);
      break;
    case I_INFO:
      tft.fillCircle(cx,cy,K(17),w);
      tft.fillCircle(cx,cy-K(8),K(2),b);
      tft.fillRect(cx-K(2),cy-K(3),K(4),K(14),b);
      break;
    case I_BROWSER:
      tft.drawCircle(cx,cy,K(16),w); tft.drawFastHLine(cx-K(15),cy,K(30),w);
      tft.drawEllipse(cx,cy,K(7),K(16),w); tft.drawFastVLine(cx,cy-K(15),K(30),w);
      break;
    case I_TIMER:
      tft.drawCircle(cx,cy+K(2),K(15),w); tft.drawRoundRect(cx-K(5),cy-K(19),K(10),K(5),K(2),w);
      tft.drawWideLine(cx,cy+K(2),cx,cy-K(8),K(2),w); tft.drawWideLine(cx,cy+K(2),cx+K(8),cy+K(6),K(2),w);
      break;
    case I_STOPWATCH:
      tft.drawCircle(cx,cy+K(2),K(15),w); tft.drawRoundRect(cx-K(5),cy-K(19),K(10),K(5),K(2),w);
      tft.drawWideLine(cx,cy+K(2),cx+K(8),cy-K(6),K(2),w);
      break;
    case I_FLAPPY:
      tft.fillCircle(cx-K(2),cy,K(12),0xFFD60A);
      tft.fillCircle(cx-K(7),cy+K(3),K(6),0xFFB300);
      tft.fillCircle(cx+K(3),cy-K(4),K(4),w);
      tft.fillCircle(cx+K(4),cy-K(4),K(2),C_BLACK);
      tft.fillTriangle(cx+K(9),cy-K(1),cx+K(19),cy+K(2),cx+K(9),cy+K(5),0xFF9500);
      break;
    case I_TTT:
      tft.fillRect(cx-K(7),cy-K(17),K(3),K(34),w);
      tft.fillRect(cx+K(5),cy-K(17),K(3),K(34),w);
      tft.fillRect(cx-K(17),cy-K(7),K(34),K(3),w);
      tft.fillRect(cx-K(17),cy+K(5),K(34),K(3),w);
      tft.drawWideLine(cx-K(15),cy-K(15),cx-K(8),cy-K(8),1.5f,w);
      tft.drawWideLine(cx-K(8),cy-K(15),cx-K(15),cy-K(8),1.5f,w);
      tft.drawCircle(cx+K(11),cy+K(11),K(4),w);
      break;
    case I_MC:
      tft.fillTriangle(cx,cy-K(15),cx+K(15),cy-K(7),cx,cy+K(1),0x5DA83A);
      tft.fillTriangle(cx,cy-K(15),cx,cy+K(1),cx-K(15),cy-K(7),0x5DA83A);
      tft.fillTriangle(cx-K(15),cy-K(7),cx,cy+K(1),cx,cy+K(16),0x8B5A2B);
      tft.fillTriangle(cx-K(15),cy-K(7),cx,cy+K(16),cx-K(15),cy+K(8),0x8B5A2B);
      tft.fillTriangle(cx,cy+K(1),cx+K(15),cy-K(7),cx+K(15),cy+K(8),0x5E3B1B);
      tft.fillTriangle(cx,cy+K(1),cx+K(15),cy+K(8),cx,cy+K(16),0x5E3B1B);
      break;
  }
}

// ---------- status bar ----------
void drawStatusBar(bool onWall){
  uint32_t fg = onWall ? C_WHITE : L_TEXT;
  if(onWall) wallRows(0,22); else tft.fillRect(0,0,240,22,L_BG);
  if(clockOn){ char b[12]; uptimeStr(b,sizeof(b)); txt(10,3,b,fg,F_S); }
  for(int rad=4;rad<=12;rad+=4)                       // wifi icon
    for(int a=-45;a<=45;a+=15){
      float t=a*0.0174533f;
      tft.fillCircle(170+(int)(sinf(t)*rad),17-(int)(cosf(t)*rad),1,fg);
    }
  tft.fillCircle(170,17,2,fg);
  tft.drawRoundRect(196,6,26,12,3,fg);                 // battery
  tft.fillRoundRect(198,8,22,8,2,fg);
  tft.fillRect(223,9,3,6,fg);
}

// ---------- shared widgets ----------
void appBar(const char* title){
  tft.fillScreen(L_BG);
  drawStatusBar(false);
  tft.drawWideLine(20,31,12,40,2.0f,accent());
  tft.drawWideLine(12,40,20,49,2.0f,accent());
  txt(34,29,title,L_TEXT,F_L);
  tft.fillRoundRect(90,308,60,4,2,0xC7C7CC);
}
void drawDots(int cy,int n,bool onWall,uint32_t col){
  if(onWall) wallRows(cy-9,cy+10); else tft.fillRect(60,cy-9,120,19,L_BG);
  uint32_t ring=onWall?C_WHITE:L_MUTED;
  for(int i=0;i<4;i++){
    int cx=120+(i-2)*28+14;
    if(i<n) tft.fillCircle(cx,cy,7,col); else tft.drawCircle(cx,cy,7,ring);
  }
}
static const char* const KEYS_PIN[12]={"1","2","3","4","5","6","7","8","9","C","0","OK"};
static const char* const KEYS_DIAL[12]={"1","2","3","4","5","6","7","8","9","*","0","#"};

void drawKeypad(int cy0,const char* const* lab,bool onWall,uint32_t lastCol){
  for(int i=0;i<12;i++){
    int cx=50+(i%3)*70, cy=cy0+(i/3)*44;
    if(i==11 && lastCol){
      tft.fillCircle(cx,cy,19,lastCol);
      txtc(cx,cy,lab[i],C_WHITE,F_L);
    } else if(onWall){
      tft.fillCircle(cx,cy,19,mix(wallColorAt(cy),C_WHITE,125));
      txtc(cx,cy,lab[i],0x1C2B4A,F_L);
    } else {
      tft.fillCircle(cx,cy,19,L_CARD);
      tft.drawCircle(cx,cy,19,L_LINE);
      txtc(cx,cy,lab[i],L_TEXT,F_L);
    }
  }
}
int keyAt(int x,int y,int cy0){
  for(int i=0;i<12;i++){
    int cx=50+(i%3)*70, cy=cy0+(i/3)*44;
    if(abs(x-cx)<=30 && abs(y-cy)<=22) return i;
  }
  return -1;
}
void chevronR(int y){
  tft.drawWideLine(214,y+14,220,y+21,1.5f,L_MUTED);
  tft.drawWideLine(220,y+21,214,y+28,1.5f,L_MUTED);
}
void drawToggle(int x,int y,bool on){
  tft.fillRoundRect(x,y,44,24,12,on?accent():L_LINE);
  tft.fillCircle(on?x+32:x+12,y+12,10,C_WHITE);
}
void openPage(String p);
void toast(const char* m){
  tft.setFont(F_M);
  int w=tft.textWidth(m)+32;
  tft.fillRoundRect(120-w/2,138,w,34,17,L_TEXT);
  txtc(120,155,m,C_WHITE,F_M);
  delay(1000);
  openPage(page);
}

// ---------- lock ----------
void drawLockClock(){
  wallRows(44,108);
  char b[12]; uptimeStr(b,sizeof(b));
  txtcS(120,64,b,C_WHITE,F_XL);
  txtcS(120,98,!pinEnabled?"Tap to unlock":(firstRun?"Default PIN: 1234":"Enter PIN"),C_WHITE,F_S);
}
void drawLock(){
  wallRows(0,320);
  drawStatusBar(true);
  txtcS(120,36,"PocketOS",C_WHITE,F_M);
  drawLockClock();
  if(pinEnabled){
    drawDots(126,entered.length(),true,C_WHITE);
    drawKeypad(162,KEYS_PIN,true,accent());
  } else {
    drawIcon(I_LOCK,100,170,40);
    txtcS(120,240,"Tap anywhere",C_WHITE,F_S);
  }
}

// ---------- home (2 pages, swipe to change) ----------
static const int GRID_ICON[9]={I_SET,I_PHOTO,I_CLOCK,I_CALC,I_FILES,I_WEATHER,I_MUSIC,I_TERM,I_LOCK};
static const char* const GRID_NAME[9]={"Settings","Wallpaper","Clock","Calc","Files","Weather","Music","Terminal","Lock"};
static const char* const GRID_PAGE[9]={"settings","wall","clock","calc","files","weather","music","term","lock"};
static const int P2_ICON[6]={I_BROWSER,I_TIMER,I_STOPWATCH,I_FLAPPY,I_TTT,I_MC};
static const char* const P2_NAME[6]={"Browser","Timer","Stopwatch","Flappy","TicTacToe","Minecraft"};
static const char* const P2_PAGE[6]={"browser","timer","stopwatch","flappy","ttt","mc"};
static const int DOCK_ICON[3]={I_PHONE,I_MSG,I_CAM};
static const char* const DOCK_PAGE[3]={"phone","messages","camera"};

void drawHome(){
  wallRows(0,320);
  drawStatusBar(true);
  int n = (homePage==0) ? 9 : 6;
  for(int i=0;i<n;i++){
    int cx=40+(i%3)*80, top=34+(i/3)*72;
    drawIcon(homePage==0?GRID_ICON[i]:P2_ICON[i],cx-25,top,50);
    if(labelsOn) txtcS(cx,top+60,homePage==0?GRID_NAME[i]:P2_NAME[i],C_WHITE,F_S);
  }
  for(int i=0;i<HOME_PAGES;i++){                       // page dots
    int cx=120+(i*2-(HOME_PAGES-1))*7;
    tft.fillCircle(cx,252,3,i==homePage?C_WHITE:mix(wallColorAt(252),C_WHITE,110));
  }
  tft.fillRoundRect(10,260,220,52,16,mix(wallColorAt(286),C_WHITE,100));
  tft.drawRoundRect(10,260,220,52,16,mix(wallColorAt(286),C_WHITE,170));
  for(int i=0;i<3;i++) drawIcon(DOCK_ICON[i],50+i*70-21,265,42);
}

// ---------- settings ----------
static const int NSET=10;
static const char* const SET_LABEL[NSET]={"Wallpaper","Accent color","Brightness","PIN lock","Change PIN","Icon labels","Status bar clock","Lock now","About","Reset settings"};

void settingRow(int idx,int y){
  tft.fillRoundRect(10,y,220,42,12,L_CARD);
  txt(22,y+10,SET_LABEL[idx],idx==9?C_RED:L_TEXT,F_M);
  char b[16];
  switch(idx){
    case 0: txt(206,y+13,WALL_NAME[wall],L_MUTED,F_S,2); chevronR(y); break;
    case 1: tft.fillCircle(212,y+21,9,accent()); break;
    case 2: snprintf(b,sizeof(b),"%d%%",BR_P[brIdx]); txt(216,y+13,b,L_MUTED,F_S,2); break;
    case 3: drawToggle(176,y+9,pinEnabled); break;
    case 5: drawToggle(176,y+9,labelsOn); break;
    case 6: drawToggle(176,y+9,clockOn); break;
    case 4: case 7: case 8: chevronR(y); break;
    default: break;
  }
}
void drawSettings(){
  appBar("Settings");
  tft.fillTriangle(189,32,181,44,197,44,setScroll>0?accent():L_LINE);
  tft.fillTriangle(215,44,207,32,223,32,setScroll+5<NSET?accent():L_LINE);
  for(int i=0;i<5;i++){
    int idx=setScroll+i;
    if(idx>=NSET) break;
    settingRow(idx,62+i*48);
  }
}
void drawPinChange(){
  appBar("New PIN");
  txtc(120,76,"Enter 4 digits",L_MUTED,F_S);
  drawDots(100,entered.length(),false,accent());
  drawKeypad(138,KEYS_PIN,false,accent());
}
void drawWallpaperPage(){
  appBar("Wallpaper");
  for(int i=0;i<4;i++){
    int x=10+(i%2)*116, y=62+(i/2)*122, w=104, h=110;
    for(int j=0;j<h;j+=2) tft.fillRect(x,y+j,w,2,mix(WALL_TOP[i],WALL_BOT[i],j*255/(h-1)));
    uint32_t oc=(i==wall)?accent():L_LINE;
    tft.drawRect(x-1,y-1,w+2,h+2,oc);
    if(i==wall){ tft.drawRect(x-2,y-2,w+4,h+4,oc); tft.drawRect(x-3,y-3,w+6,h+6,oc); }
    txtc(x+w/2,y+h-14,WALL_NAME[i],C_WHITE,F_M);
  }
}

// ---------- apps ----------
void drawClockFace(){
  const int cx=120, cy=138;
  struct tm ti;
  int hh=0, mm=0, ss=0;
  if(getLocalTime(&ti, 5)){ hh=ti.tm_hour%12; mm=ti.tm_min; ss=ti.tm_sec; }
  else { unsigned long s=millis()/1000; hh=(s/3600)%12; mm=(s/60)%60; ss=s%60; }
  tft.fillCircle(cx,cy,66,L_LINE);
  tft.fillCircle(cx,cy,64,L_CARD);
  for(int i=0;i<12;i++){
    float a=i*0.523599f;
    int r0=(i%3==0)?52:57;
    tft.drawLine(cx+(int)(sinf(a)*r0),cy-(int)(cosf(a)*r0),cx+(int)(sinf(a)*61),cy-(int)(cosf(a)*61),L_TEXT);
  }
  float ah=((hh+mm/60.0f)*30.0f)*0.0174533f, am=((mm+ss/60.0f)*6.0f)*0.0174533f, as=(ss*6.0f)*0.0174533f;
  tft.drawWideLine(cx,cy,cx+(int)(sinf(ah)*32),cy-(int)(cosf(ah)*32),2.5f,L_TEXT);
  tft.drawWideLine(cx,cy,cx+(int)(sinf(am)*48),cy-(int)(cosf(am)*48),1.8f,L_TEXT);
  tft.drawWideLine(cx,cy,cx+(int)(sinf(as)*54),cy-(int)(cosf(as)*54),1.0f,accent());
  tft.fillCircle(cx,cy,4,accent());
  tft.fillRect(0,214,240,38,L_BG);
  char b[12];
  if(getLocalTime(&ti, 5)) strftime(b,sizeof(b),"%H:%M:%S",&ti);
  else { unsigned long s=millis()/1000; snprintf(b,sizeof(b),"%02lu:%02lu:%02lu",s/3600,(s/60)%60,s%60); }
  txtc(120,232,b,L_TEXT,F_XL);
}
void drawClock(){
  appBar("Clock");
  drawClockFace();
  txtc(120,268,wifiReady?"German local time":"Local time (waiting for Wi-Fi)",L_MUTED,F_S);
  lastClock=millis();
}

static const char* const CK[20]={"C","+/-","%","/","7","8","9","*","4","5","6","-","1","2","3","+","0",".","<","="};
String fmtNum(double v){
  if(isnan(v)||isinf(v)) return "Error";
  char b[24]; snprintf(b,sizeof(b),"%.8g",v);
  return String(b);
}
bool calcApply(){
  double v=cDisp.toDouble();
  switch(cOp){
    case '+': cAcc+=v; break;
    case '-': cAcc-=v; break;
    case '*': cAcc*=v; break;
    case '/': if(v==0){ cDisp="Error"; cOp=0; cNew=true; return false; } cAcc/=v; break;
    default: cAcc=v;
  }
  cDisp=fmtNum(cAcc);
  return true;
}
void calcKey(const String& k){
  if(cDisp=="Error"){ cDisp="0"; cAcc=0; cOp=0; cNew=true; if(k=="C") return; }
  if(k=="C"){ cDisp="0"; cAcc=0; cOp=0; cNew=true; }
  else if(k=="+/-"){ if(cDisp!="0"){ if(cDisp[0]=='-') cDisp.remove(0,1); else cDisp="-"+cDisp; } }
  else if(k=="%"){ cDisp=fmtNum(cDisp.toDouble()/100.0); cNew=true; }
  else if(k=="<"){ if(!cNew){ cDisp.remove(cDisp.length()-1); if(cDisp==""||cDisp=="-") cDisp="0"; } }
  else if(k=="="){ if(cOp){ if(calcApply()){ cOp=0; cNew=true; } } }
  else if(k=="+"||k=="-"||k=="*"||k=="/"){
    if(cOp && !cNew){ if(!calcApply()) return; } else cAcc=cDisp.toDouble();
    cOp=k[0]; cNew=true;
  } else {
    if(cNew){ if(k==".") cDisp="0."; else cDisp=k; cNew=false; }
    else if(cDisp.length()<12){
      if(k=="."){ if(cDisp.indexOf('.')<0) cDisp+="."; }
      else if(cDisp=="0") cDisp=k;
      else cDisp+=k;
    }
  }
}
void drawCalcDisplay(){
  tft.fillRoundRect(10,58,220,54,12,L_CARD);
  if(cDisp.length()<=8) txt(220,64,cDisp.c_str(),L_TEXT,F_XL,2);
  else txt(220,72,cDisp.c_str(),L_TEXT,F_L,2);
}
void drawCalc(){
  appBar("Calculator");
  drawCalcDisplay();
  for(int i=0;i<20;i++){
    int x=10+(i%4)*57, y=122+(i/4)*36;
    bool op=(i%4==3);
    uint32_t bg = op ? accent() : (i<3 ? L_LINE : L_CARD);
    tft.fillRoundRect(x,y,51,32,10,bg);
    txtc(x+25,y+16,CK[i],op?C_WHITE:L_TEXT,F_M);
  }
}

void drawDialNumber(){
  tft.fillRect(10,58,180,36,L_BG);
  if(dial.length()) txtc(100,76,dial.c_str(),L_TEXT,F_L);
  else txtc(100,76,"Enter number",L_LINE,F_M);
}
void drawDialer(){
  appBar("Phone");
  drawDialNumber();
  txtc(214,76,"Del",accent(),F_S);
  drawKeypad(118,KEYS_DIAL,false,0);
  tft.fillRoundRect(70,272,100,26,13,C_GREEN);
  txtc(120,285,"Call",C_WHITE,F_M);
}
void drawMessages(){
  appBar("Messages");
  static const char* const who[3]={"PocketOS","System","Developer"};
  static const char* const msg[3]={"Welcome to your pocket system!","Network not set up yet","Build 0.6 - swipe + games"};
  uint32_t cols[3]={accent(),0x0A84FF,C_GREEN};
  for(int i=0;i<3;i++){
    int y=64+i*58;
    tft.fillRoundRect(10,y,220,52,12,L_CARD);
    tft.fillCircle(34,y+26,16,cols[i]);
    char ini[2]={who[i][0],0};
    txtc(34,y+26,ini,C_WHITE,F_M);
    txt(60,y+8,who[i],L_TEXT,F_M);
    txt(60,y+30,msg[i],L_MUTED,F_S);
  }
}
void drawCamera(){
  appBar("Camera");
  tft.fillRoundRect(10,60,220,170,10,C_BLACK);
  tft.drawFastHLine(26,76,16,C_WHITE);  tft.drawFastVLine(26,76,16,C_WHITE);
  tft.drawFastHLine(198,76,16,C_WHITE); tft.drawFastVLine(213,76,16,C_WHITE);
  tft.drawFastHLine(26,213,16,C_WHITE); tft.drawFastVLine(26,198,16,C_WHITE);
  tft.drawFastHLine(198,213,16,C_WHITE);tft.drawFastVLine(213,198,16,C_WHITE);
  txtc(120,145,"No camera module",L_MUTED,F_S);
  tft.fillCircle(120,268,24,L_TEXT);
  tft.fillCircle(120,268,21,L_BG);
  tft.fillCircle(120,268,18,L_TEXT);
}
void refreshSDEntries(){
  sdEntryCount = 0;
  for(int i=0;i<4;i++){ sdEntryName[i]=""; sdEntryIsDir[i]=false; sdEntrySize[i]=0; }
  if(!sdReady) return;
  File dir = SD.open(sdCurrentPath.c_str());
  if(!dir || !dir.isDirectory()){ if(dir) dir.close(); return; }
  while(sdEntryCount < 4){
    File entry = dir.openNextFile();
    if(!entry) break;
    String name = String(entry.name());
    // Some SD libraries return a full path; show only the final component.
    int slash = name.lastIndexOf('/');
    if(slash >= 0) name = name.substring(slash + 1);
    if(name.length() && name != "." && name != ".."){
      sdEntryName[sdEntryCount] = name;
      sdEntryIsDir[sdEntryCount] = entry.isDirectory();
      sdEntrySize[sdEntryCount] = entry.size();
      sdEntryCount++;
    }
    entry.close();
  }
  dir.close();
}
void drawFiles(){
  appBar("Files");
  if(!sdReady){
    drawIcon(I_FILES,88,78,64);
    txtc(120,166,"SD card not ready",L_TEXT,F_M);
    txtc(120,190,"Insert FAT32 microSD",L_MUTED,F_S);
    txtc(120,210,"and restart PocketOS",L_MUTED,F_S);
    tft.fillRoundRect(36,242,168,34,10,accent());
    txtc(120,259,"Retry SD card",C_WHITE,F_S);
    return;
  }
  txtc(120,58,sdCurrentPath.c_str(),L_MUTED,F_S);
  // First row navigates up, or refreshes when already at root.
  tft.fillRoundRect(10,68,220,35,9,L_CARD);
  txt(20,78,sdCurrentPath=="/"?"Refresh list":"..  Up one folder",L_TEXT,F_M);
  for(int i=0;i<sdEntryCount;i++){
    int y=108+i*43;
    tft.fillRoundRect(10,y,220,38,9,L_CARD);
    txt(18,y+10,sdEntryIsDir[i]?"[DIR]":"[FILE]",accent(),F_S);
    String shown=sdEntryName[i];
    if(shown.length()>17) shown=shown.substring(0,16)+"~";
    txt(68,y+10,shown.c_str(),L_TEXT,F_S);
    if(sdEntryIsDir[i]) chevronR(y);
  }
  if(sdEntryCount==0) txtc(120,158,"No files in this folder",L_MUTED,F_S);
  txtc(120,290,"Tap a folder to open it",L_MUTED,F_S);
}
String sdJoinPath(const String& base,const String& name){
  if(base=="/") return "/"+name;
  return base+"/"+name;
}
void handleFiles(int x,int y){
  if(!sdReady){
    if(hit(x,y,36,242,168,34)){
      sdSPI.begin(SD_SCK_PIN,SD_MISO_PIN,SD_MOSI_PIN,SD_CS_PIN);
      sdReady=SD.begin(SD_CS_PIN,sdSPI,10000000);
      if(sdReady){ sdCurrentPath="/"; refreshSDEntries(); drawFiles(); }
      else toast("SD init failed");
    }
    return;
  }
  if(hit(x,y,10,68,220,35)){
    if(sdCurrentPath!="/"){
      int slash=sdCurrentPath.lastIndexOf('/');
      if(slash<=0) sdCurrentPath="/"; else sdCurrentPath=sdCurrentPath.substring(0,slash);
    }
    refreshSDEntries(); drawFiles(); return;
  }
  for(int i=0;i<sdEntryCount;i++){
    int rowY=108+i*43;
    if(hit(x,y,10,rowY,220,38)){
      String selected=sdEntryName[i];
      if(sdEntryIsDir[i]){
        sdCurrentPath=sdJoinPath(sdCurrentPath,selected);
        refreshSDEntries(); drawFiles();
      } else {
        String message=selected;
        if(message.length()>21) message=message.substring(0,20);
        Serial.printf("SD file: %s (%lu bytes)\n",sdJoinPath(sdCurrentPath,selected).c_str(),(unsigned long)sdEntrySize[i]);
        toast(message.c_str());
      }
      return;
    }
  }
}
void infoPage(const char* title,int icon,const char* a,const char* b,const char* c){
  appBar(title);
  drawIcon(icon,88,70,64);
  txtc(120,160,a,L_TEXT,F_L);
  txtc(120,190,b,L_MUTED,F_S);
  txtc(120,212,c,L_MUTED,F_S);
}
void drawAbout(){
  char a[32]; snprintf(a,sizeof(a),"%u KB free RAM",(unsigned)(ESP.getFreeHeap()/1024));
  infoPage("About",I_INFO,"PocketOS 0.6","ESP32-2432S028 CYD",a);
}

// ---------- network, browser, timer and stopwatch ----------
void startNetwork(){
  if(String(WIFI_SSID)=="YOUR_WIFI_NAME" || String(WIFI_SSID).length()==0){
    Serial.println("Set WIFI_SSID and WIFI_PASSWORD in the sketch.");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long began=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-began<12000){ delay(250); Serial.print("."); }
  Serial.println();
  wifiReady=(WiFi.status()==WL_CONNECTED);
  if(!wifiReady){ Serial.println("Wi-Fi connection failed; network apps unavailable."); return; }
  // Europe/Berlin: automatic CET/CEST daylight-saving changes.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3","pool.ntp.org","time.google.com");
}
String stripHtml(const String& html){
  String out; bool tag=false, space=false;
  for(size_t i=0;i<html.length() && out.length()<420;i++){
    char c=html[i];
    if(c=='<'){ tag=true; continue; }
    if(c=='>'){ tag=false; space=true; continue; }
    if(tag) continue;
    if(c=='&'){
      if(html.substring(i,i+4)=="&lt;"){out+='<';i+=3;continue;}
      if(html.substring(i,i+4)=="&gt;"){out+='>';i+=3;continue;}
      if(html.substring(i,i+5)=="&amp;"){out+='&';i+=4;continue;}
      if(html.substring(i,i+6)=="&nbsp;"){out+=' ';i+=5;continue;}
    }
    if(c=='\r'||c=='\n'||c=='\t') c=' ';
    if(c==' '){ if(!space) out+=' '; space=true; }
    else { out+=c; space=false; }
  }
  out.trim(); return out;
}
void browserLoad(){
  if(!wifiReady || WiFi.status()!=WL_CONNECTED){ browserText="Wi-Fi not connected. Check SSID/password in sketch."; return; }
  if(!browserUrl.startsWith("http://")){ browserText="This lightweight browser supports HTTP URLs only. Use http://..."; return; }
  HTTPClient http;
  http.setTimeout(5000);
  if(!http.begin(browserUrl)){ browserText="Could not open URL."; return; }
  int code=http.GET();
  if(code>0){
    String body=http.getString();
    browserText="HTTP "+String(code)+"\n"+stripHtml(body);
    if(browserText.length()>360) browserText.remove(360);
    browserLinkCount=0;
    int pos=0;
    while(browserLinkCount<3){
      int href=body.indexOf("href=",pos); if(href<0) href=body.indexOf("HREF=",pos);
      if(href<0) break;
      int q=href+5;
      while(q<(int)body.length() && body[q]==' ') q++;
      if(q>=(int)body.length()) break;
      char quote=body[q];
      if(body[q]==39 || body[q]==34) q++;
      else quote=' ';
      int e=q;
      while(e<(int)body.length() && ((quote==' ' && body[e]!=' ' && body[e]!='>') || (quote!=' ' && body[e]!=quote))) e++;
      String link=body.substring(q,e); pos=e+1;
      if(link.startsWith("http://")) browserLinks[browserLinkCount++]=link;
      else if(link.startsWith("/")) {
        int hostEnd=browserUrl.indexOf('/',7);
        if(hostEnd<0) hostEnd=browserUrl.length();
        browserLinks[browserLinkCount++]=browserUrl.substring(0,hostEnd)+link;
      }
    }
  } else browserText="Request failed: "+http.errorToString(code);
  http.end();
}
void drawBrowser(){
  appBar("Browser");
  tft.fillRoundRect(8,53,224,29,7,L_CARD); tft.drawRoundRect(8,53,224,29,7,L_LINE);
  String shown=browserUrl; if(shown.length()>29) shown=shown.substring(shown.length()-29);
  txt(13,60,shown.c_str(),L_TEXT,F_S);
  tft.fillRoundRect(174,85,58,25,7,accent()); txtc(203,97,browserEditing?"Done":"Load",C_WHITE,F_S);
  if(browserEditing){
    static const char keys[]="abcdefghijklmnopqrstuvwxyz0123456789./:-_?";
    for(int i=0;i<42;i++){
      int x=5+(i%7)*33, y=116+(i/7)*25;
      tft.fillRoundRect(x,y,29,21,4,L_CARD);
      char label[2]={keys[i],0}; txtc(x+14,y+10,label,L_TEXT,F_S);
    }
    tft.fillRoundRect(8,270,104,25,7,L_CARD); txtc(60,282,"Backspace",L_TEXT,F_S);
    tft.fillRoundRect(120,270,112,25,7,L_CARD); txtc(176,282,"Clear URL",L_TEXT,F_S);
  } else {
    tft.fillRoundRect(8,85,158,25,7,L_CARD); txtc(87,97,"Tap address to edit",L_MUTED,F_S);
    tft.fillRoundRect(8,115,224,140,8,L_CARD);
    String text=browserText; int y=122;
    while(text.length() && y<247){
      int cut=text.indexOf('\n'); if(cut<0) cut=text.length();
      String line=text.substring(0,min(cut,34)); line.trim();
      if(line.length()) { txt(14,y,line.c_str(),L_TEXT,F_S); y+=15; }
      if(cut>=(int)text.length()) break;
      text.remove(0,cut+1);
    }
    if(browserLinkCount){
      for(int i=0;i<browserLinkCount;i++){
        int ly=258+i*13;
        String link=browserLinks[i]; if(link.length()>31) link=link.substring(0,31);
        txt(10,ly,link.c_str(),accent(),F_S);
      }
    } else txtc(120,270,"No simple HTTP links found",L_MUTED,F_S);
    txtc(120,296,"HTTP text only - no JavaScript",L_MUTED,F_S);
  }
}
String timeFmt(unsigned long ms){
  unsigned long sec=ms/1000UL;
  char b[20]; snprintf(b,sizeof(b),"%02lu:%02lu:%02lu.%1lu",(sec/3600)%100,(sec/60)%60,sec%60,(ms%1000)/100);
  return String(b);
}
unsigned long timerLeft(){
  if(!timerRunning) return timerRemainingMs;
  unsigned long elapsed=millis()-timerStartedAt;
  if(elapsed>=timerRemainingMs){ timerRemainingMs=0; timerRunning=false; return 0; }
  return timerRemainingMs-elapsed;
}
void drawTimer(){
  appBar("Countdown timer");
  unsigned long left=timerLeft();
  txtc(120,92,timeFmt(left).c_str(),L_TEXT,F_XL);
  txtc(120,122,"Default: 1 minute",L_MUTED,F_S);
  tft.fillRoundRect(10,151,66,38,10,L_CARD); txtc(43,170,"-10s",L_TEXT,F_M);
  tft.fillRoundRect(87,151,66,38,10,L_CARD); txtc(120,170,"+10s",L_TEXT,F_M);
  tft.fillRoundRect(164,151,66,38,10,L_CARD); txtc(197,170,"Reset",L_TEXT,F_M);
  tft.fillRoundRect(28,211,184,44,13,accent());
  txtc(120,233,timerRunning?"Pause":"Start",C_WHITE,F_L);
  if(left==0) txtc(120,276,"Timer finished",C_RED,F_M);
}
void drawStopwatch(){
  appBar("Stopwatch");
  unsigned long ms=stopwatchAccumulatedMs+(stopwatchRunning?(millis()-stopwatchStartedAt):0);
  txtc(120,103,timeFmt(ms).c_str(),L_TEXT,F_XL);
  tft.fillRoundRect(20,177,90,44,12,accent()); txtc(65,199,stopwatchRunning?"Pause":"Start",C_WHITE,F_M);
  tft.fillRoundRect(130,177,90,44,12,L_CARD); txtc(175,199,"Reset",L_TEXT,F_M);
  txtc(120,252,"Uses millis() elapsed time",L_MUTED,F_S);
}
void handleBrowser(int x,int y){
  if(hit(x,y,8,53,224,29)){ browserEditing=true; drawBrowser(); return; }
  if(hit(x,y,174,85,58,25)){
    if(browserEditing) browserEditing=false;
    else browserLoad();
    drawBrowser(); return;
  }
  if(!browserEditing && browserLinkCount && y>=255 && y<294){
    int li=(y-255)/13;
    if(li>=0 && li<browserLinkCount){ browserUrl=browserLinks[li]; browserLoad(); drawBrowser(); return; }
  }
  if(browserEditing){
    if(hit(x,y,8,270,104,25)){
      if(browserUrl.length()) browserUrl.remove(browserUrl.length()-1);
      drawBrowser(); return;
    }
    if(hit(x,y,120,270,112,25)){ browserUrl=""; drawBrowser(); return; }
    static const char keys[]="abcdefghijklmnopqrstuvwxyz0123456789./:-_?";
    if(y>=116 && y<266){
      int row=(y-116)/25, col=(x-5)/33, idx=row*7+col;
      if(x>=5 && col>=0 && col<7 && idx>=0 && idx<42 && browserUrl.length()<100){
        browserUrl+=keys[idx]; drawBrowser();
      }
    }
  }
}
void handleTimer(int x,int y){
  if(hit(x,y,10,151,66,38)){
    unsigned long left=timerLeft(); timerRemainingMs=(left>10000?left-10000:10000); if(timerRunning) timerStartedAt=millis();
  } else if(hit(x,y,87,151,66,38)){
    unsigned long left=timerLeft(); timerRemainingMs=min(3600000UL,left+10000UL); if(timerRunning) timerStartedAt=millis();
  } else if(hit(x,y,164,151,66,38)){ timerRunning=false; timerRemainingMs=60000UL; }
  else if(hit(x,y,28,211,184,44)){
    if(timerRunning){ timerRemainingMs=timerLeft(); timerRunning=false; }
    else if(timerRemainingMs>0){ timerStartedAt=millis(); timerRunning=true; }
  }
  drawTimer();
}
void handleStopwatch(int x,int y){
  if(hit(x,y,20,177,90,44)){
    if(stopwatchRunning){ stopwatchAccumulatedMs+=millis()-stopwatchStartedAt; stopwatchRunning=false; }
    else { stopwatchStartedAt=millis(); stopwatchRunning=true; }
  } else if(hit(x,y,130,177,90,44)){ stopwatchRunning=false; stopwatchAccumulatedMs=0; }
  drawStopwatch();
}

// ---------- GAME: Flappy ----------
struct Pipe { int x; int gap; bool scored; };
static Pipe pipes[3];
static float fy=150, fvy=0;
static int fScore=0, fState=0, fOldY=150;   // fState: 0 ready, 1 playing, 2 over
static unsigned long fLast=0;
static const uint32_t SKY=0x9BE4FF;
static const int PW=36, GAPH=78, TOPY=56, GROUND=290, BX=60;

void flappyScore(){
  tft.fillRect(110,26,122,26,L_BG);
  char b[28]; snprintf(b,sizeof(b),"%d   Best %d",fScore,fBest);
  txt(230,30,b,L_TEXT,F_M,2);
}
void flappyReset(){
  fy=150; fvy=0; fScore=0; fState=0; fOldY=150;
  for(int i=0;i<3;i++){
    pipes[i].x=260+i*130;
    pipes[i].gap=random(TOPY+22,GROUND-GAPH-22);
    pipes[i].scored=false;
  }
}
void flappyDrawPipes(){
  for(int i=0;i<3;i++){
    int x=pipes[i].x, g=pipes[i].gap;
    if(x>240 || x+PW+4<0) continue;
    tft.fillRect(x,TOPY,PW,g-TOPY,0x34C759);
    tft.fillRect(x-2,g-10,PW+4,10,0x248A3D);
    tft.fillRect(x,g+GAPH,PW,GROUND-(g+GAPH),0x34C759);
    tft.fillRect(x-2,g+GAPH,PW+4,10,0x248A3D);
  }
}
void flappyDrawBird(int y){
  tft.fillCircle(BX,y,9,0xFFD60A);
  tft.fillCircle(BX-3,y+2,4,0xFFB300);
  tft.fillCircle(BX+3,y-3,3,C_WHITE);
  tft.fillCircle(BX+4,y-3,1,C_BLACK);
  tft.fillTriangle(BX+8,y-1,BX+15,y+1,BX+8,y+3,0xFF9500);
}
void drawFlappy(){
  appBar("Flappy");
  flappyReset();
  tft.fillRect(0,TOPY,240,GROUND-TOPY,SKY);
  tft.fillRect(0,GROUND,240,18,0xDED895);
  tft.fillRect(0,GROUND,240,4,0x7CB342);
  flappyScore();
  flappyDrawPipes();
  flappyDrawBird((int)fy);
  txtc(120,170,"Tap to start",L_TEXT,F_L);
}
void flappyTap(){
  if(fState==0){
    fState=1; fvy=-5.5f; fLast=millis();
    tft.fillRect(40,150,160,40,SKY);
  } else if(fState==1) fvy=-5.5f;
  else drawFlappy();
}
void flappyTick(unsigned long now){
  if(fState!=1) return;
  if(now-fLast<33) return;
  fLast=now;
  tft.fillRect(BX-14,fOldY-13,32,27,SKY);          // erase old bird
  fvy+=0.55f; if(fvy>9) fvy=9;
  fy+=fvy;
  bool dead=false;
  for(int i=0;i<3;i++){
    pipes[i].x-=3;
    tft.fillRect(pipes[i].x+PW+2,TOPY,3,GROUND-TOPY,SKY);   // erase trailing strip
    if(pipes[i].x+PW+2<0){
      pipes[i].x+=390;
      pipes[i].gap=random(TOPY+22,GROUND-GAPH-22);
      pipes[i].scored=false;
    }
    if(!pipes[i].scored && pipes[i].x+PW<BX-8){
      pipes[i].scored=true; fScore++; flappyScore();
    }
    if(BX+8>pipes[i].x-2 && BX-8<pipes[i].x+PW+2 && (fy-8<pipes[i].gap || fy+8>pipes[i].gap+GAPH)) dead=true;
  }
  if(fy>GROUND-9){ fy=GROUND-9; dead=true; }
  if(fy<TOPY+10){ fy=TOPY+10; fvy=0; }
  flappyDrawPipes();
  flappyDrawBird((int)fy);
  fOldY=(int)fy;
  if(dead){
    fState=2;
    if(fScore>fBest){
      fBest=fScore;
      prefs.begin("pocketos",false); prefs.putInt("fbest",fBest); prefs.end();
      flappyScore();
    }
    tft.fillRoundRect(40,120,160,92,14,L_CARD);
    tft.drawRoundRect(40,120,160,92,14,L_LINE);
    txtc(120,142,"Game over",L_TEXT,F_L);
    char b[24]; snprintf(b,sizeof(b),"Score %d",fScore);
    txtc(120,170,b,accent(),F_M);
    txtc(120,196,"Tap to retry",L_MUTED,F_S);
  }
}

// ---------- GAME: Tic-Tac-Toe (you = X, CPU = O) ----------
static char tb[9];
static bool tOver=false;
static int tTurn=0, tW=0, tL=0, tD=0;       // tTurn: 0 you, 1 CPU
static unsigned long tCpuAt=0;
static const int LN[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
static const int TBX=24, TBY=84;

int tWinLine(char p){
  for(int i=0;i<8;i++) if(tb[LN[i][0]]==p && tb[LN[i][1]]==p && tb[LN[i][2]]==p) return i;
  return -1;
}
bool tFull(){ for(int i=0;i<9;i++) if(tb[i]==' ') return false; return true; }
void tStatus(const char* s,uint32_t c){ tft.fillRect(0,56,240,24,L_BG); txtc(120,67,s,c,F_M); }
void tScore(){
  char b[40]; snprintf(b,sizeof(b),"You %d    CPU %d    Draw %d",tW,tL,tD);
  tft.fillRect(0,282,240,18,L_BG);
  txtc(120,291,b,L_MUTED,F_S);
}
void tDrawMark(int i){
  int cx=TBX+(i%3)*64+32, cy=TBY+(i/3)*64+32;
  tft.fillRect(cx-28,cy-28,56,56,L_CARD);
  if(tb[i]=='X'){
    tft.drawWideLine(cx-16,cy-16,cx+16,cy+16,3.0f,accent());
    tft.drawWideLine(cx-16,cy+16,cx+16,cy-16,3.0f,accent());
  } else if(tb[i]=='O'){
    tft.fillCircle(cx,cy,19,C_RED);
    tft.fillCircle(cx,cy,14,L_CARD);
  }
}
void drawTTTBoard(){
  tft.fillRoundRect(TBX-6,TBY-6,204,204,14,L_CARD);
  tft.fillRect(TBX+63,TBY+8,2,176,L_LINE);
  tft.fillRect(TBX+127,TBY+8,2,176,L_LINE);
  tft.fillRect(TBX+8,TBY+63,176,2,L_LINE);
  tft.fillRect(TBX+8,TBY+127,176,2,L_LINE);
  for(int i=0;i<9;i++) tDrawMark(i);
}
void tReset(){
  for(int i=0;i<9;i++) tb[i]=' ';
  tOver=false; tTurn=0; tCpuAt=0;
}
void drawTTT(){
  appBar("Tic-Tac-Toe");
  tReset();
  drawTTTBoard();
  tStatus("Your turn (X)",L_TEXT);
  tScore();
}
void tFinish(int winner){      // winner: 1 you, 2 CPU, 0 draw
  tOver=true;
  if(winner==1){ tW++; tStatus("You win! Tap to play again",C_GREEN); }
  else if(winner==2){ tL++; tStatus("CPU wins. Tap to play again",C_RED); }
  else { tD++; tStatus("Draw. Tap to play again",L_MUTED); }
  char p = winner==1?'X':'O';
  if(winner){
    int l=tWinLine(p);
    int a=LN[l][0], c=LN[l][2];
    tft.drawWideLine(TBX+(a%3)*64+32,TBY+(a/3)*64+32,TBX+(c%3)*64+32,TBY+(c/3)*64+32,3.0f,C_GREEN);
  }
  tScore();
}
int tLineMove(char p){        // empty cell that completes two-in-line for p, or -1
  for(int i=0;i<8;i++){
    int cnt=0, empty=-1;
    for(int k=0;k<3;k++){
      char v=tb[LN[i][k]];
      if(v==p) cnt++; else if(v==' ') empty=LN[i][k];
    }
    if(cnt==2 && empty>=0) return empty;
  }
  return -1;
}
int tCpuMove(){
  int m=tLineMove('O'); if(m>=0) return m;
  m=tLineMove('X'); if(m>=0) return m;
  if(tb[4]==' ') return 4;
  int cs[4]={0,2,6,8}, opts[4], n=0;
  for(int i=0;i<4;i++) if(tb[cs[i]]==' ') opts[n++]=cs[i];
  if(n) return opts[random(n)];
  int any[9], k=0;
  for(int i=0;i<9;i++) if(tb[i]==' ') any[k++]=i;
  return any[random(k)];
}
void tttTap(int x,int y){
  if(tOver){ drawTTT(); return; }
  if(tTurn!=0) return;
  if(x<TBX || y<TBY || x>=TBX+192 || y>=TBY+192) return;
  int i=((y-TBY)/64)*3+(x-TBX)/64;
  if(tb[i]!=' ') return;
  tb[i]='X'; tDrawMark(i);
  if(tWinLine('X')>=0){ tFinish(1); return; }
  if(tFull()){ tFinish(0); return; }
  tTurn=1; tCpuAt=millis()+450;
  tStatus("CPU thinking...",L_MUTED);
}
void tttTick(unsigned long now){
  if(tTurn==1 && !tOver && now>=tCpuAt){
    int m=tCpuMove();
    tb[m]='O'; tDrawMark(m);
    if(tWinLine('O')>=0){ tFinish(2); return; }
    if(tFull()){ tFinish(0); return; }
    tTurn=0;
    tStatus("Your turn (X)",L_TEXT);
  }
}

// ===================== MINECRAFT APP (landscape, isometric block builder) =====================
// ============================================================================
//  Minecraft.h - Minecraft-style block builder for PocketOS (fan-made, not
//  affiliated with Mojang). Landscape 320x240, isometric 3D blocks, main menu
//  with Play / Settings / Quit, 8 block types, rotate view, place / break.
//
//  Include it from the main sketch AFTER drawIcon(), hit(), mix(), txt(),
//  txtc() are defined and BEFORE openPage() is defined.
//  Needs: tft (LovyanGFX), prefs (Preferences), F_S/F_M/F_L/F_XL font macros.
// ============================================================================

// ---------- world ----------
static const int MC_WX=8, MC_WY=8, MC_WZ=5;
static uint8_t mcWorld[MC_WX][MC_WY][MC_WZ];      // 0 = air, 1..8 = block types

// ---------- state ----------
static int  mcState=0;                 // 0 main menu, 1 settings, 2 game
static int  mcRot=0, mcSel=1, mcMode=0, mcSky=0;   // mcMode: 0 place, 1 break; mcSky: 0 day, 1 sunset, 2 night
static bool mcOut=true, mcTex=true, mcFlip=false, mcAuto=true, mcLoaded=false;
static unsigned long mcResetArm=0, mcDoneAt=0;

// ---------- isometric geometry (half width, half height, cube height) ----------
static const int MC_TW2=14, MC_TH2=7, MC_CH=14, MC_OX=112, MC_OY=70;

// ---------- block look ----------
static const uint32_t MC_TOP[9]  ={0,0x5DA83A,0x8B5A2B,0x8E8E8E,0xB8864B,0xA0763C,0xE6D690,0xB04A3A,0x2F7D2B};
static const uint32_t MC_SIDE[9] ={0,0x8B5A2B,0x7A4E26,0x808080,0xA97A42,0x5C4126,0xD8C880,0xA04030,0x2A7226};
// 1 Grass, 2 Dirt, 3 Stone, 4 Planks, 5 Log, 6 Sand, 7 Brick, 8 Leaves

static uint32_t mcHash(uint32_t a){ a=a*2654435761u+0x9E3779B9u; a^=a<<13; a^=a>>17; a^=a<<5; return a; }
static uint32_t mcSkyColor(){ static const uint32_t c[3]={0x8CC8FF,0xFFA46B,0x0B1030}; return c[mcSky]; }
static uint32_t mcShade(uint32_t c){
  if(mcSky==2) return mix(c,0x0A1024,120);
  if(mcSky==1) return mix(c,0xFF7A3C,30);
  return c;
}

// ---------- settings / world storage ----------
static void mcLoadCfg(){
  prefs.begin("pocketos",true);
  mcSky=constrain(prefs.getInt("mcsky",0),0,2);
  mcOut=prefs.getBool("mcout",true); mcTex=prefs.getBool("mctex",true);
  mcFlip=prefs.getBool("mcflip",false); mcAuto=prefs.getBool("mcauto",true);
  prefs.end();
}
static void mcSaveCfg(){
  prefs.begin("pocketos",false);
  prefs.putInt("mcsky",mcSky);
  prefs.putBool("mcout",mcOut); prefs.putBool("mctex",mcTex);
  prefs.putBool("mcflip",mcFlip); prefs.putBool("mcauto",mcAuto);
  prefs.end();
}
static void mcSaveWorld(){
  prefs.begin("pocketos",false);
  prefs.putBytes("mcw",mcWorld,sizeof(mcWorld));
  prefs.end();
}
static bool mcLoadWorld(){
  prefs.begin("pocketos",true);
  size_t n=prefs.getBytesLength("mcw");
  bool ok=(n==sizeof(mcWorld));
  if(ok) prefs.getBytes("mcw",mcWorld,n);
  prefs.end();
  return ok;
}
static void mcGen(){                    // flat grass world with a small tree
  memset(mcWorld,0,sizeof(mcWorld));
  for(int x=0;x<MC_WX;x++) for(int y=0;y<MC_WY;y++) mcWorld[x][y][0]=1;
  mcWorld[5][2][1]=5; mcWorld[5][2][2]=5;
  mcWorld[5][2][3]=8; mcWorld[4][2][3]=8; mcWorld[6][2][3]=8; mcWorld[5][1][3]=8; mcWorld[5][3][3]=8;
  mcWorld[5][2][4]=8;
}

// ---------- view <-> world (4 rotations; world is square) ----------
static void mcToWorld(int vx,int vy,int &wx,int &wy){
  switch(mcRot&3){
    case 0: wx=vx;           wy=vy;           break;
    case 1: wx=vy;           wy=MC_WX-1-vx;   break;
    case 2: wx=MC_WX-1-vx;   wy=MC_WY-1-vy;   break;
    default: wx=MC_WY-1-vy;  wy=vx;           break;
  }
}
static bool mcInside(int vx,int vy,int z){ return vx>=0 && vy>=0 && z>=0 && vx<MC_WX && vy<MC_WY && z<MC_WZ; }
static uint8_t mcGet(int vx,int vy,int z){
  if(!mcInside(vx,vy,z)) return 0;
  int wx,wy; mcToWorld(vx,vy,wx,wy);
  return mcWorld[wx][wy][z];
}
static void mcSet(int vx,int vy,int z,uint8_t v){
  if(!mcInside(vx,vy,z)) return;
  int wx,wy; mcToWorld(vx,vy,wx,wy);
  mcWorld[wx][wy][z]=v;
}
static inline int mcCX(int vx,int vy){ return MC_OX+(vx-vy)*MC_TW2; }
static inline int mcCY(int vx,int vy,int z){ return MC_OY+(vx+vy)*MC_TH2-z*MC_CH; }

// ---------- drawing ----------
static void mcBtn(int x,int y,int w,int h,const char* s){      // Minecraft-style stone button
  tft.fillRect(x,y,w,h,0x000000);
  tft.fillRect(x+1,y+1,w-2,h-2,0x6F6F6F);
  tft.fillRect(x+1,y+1,w-2,2,0xA9A9A9);   tft.fillRect(x+1,y+1,2,h-2,0xA9A9A9);
  tft.fillRect(x+1,y+h-3,w-2,2,0x3D3D3D); tft.fillRect(x+w-3,y+1,2,h-2,0x3D3D3D);
  txtc(x+w/2+1,y+h/2+1,s,0x3A3A3A,F_M);
  txtc(x+w/2,y+h/2,s,0xFFFFFF,F_M);
}

static void mcSpeck(int ox,int oy,int v1x,int v1y,int v2x,int v2y,uint32_t seed,uint32_t base,int n){
  for(int i=0;i<n;i++){
    uint32_t h=mcHash(seed+(uint32_t)i*7919u);
    int a=1+(int)(h%14), b=1+(int)((h>>8)%14);
    int px=ox+(a*v1x+b*v2x)/16, py=oy+(a*v1y+b*v2y)/16;
    tft.fillRect(px,py,2,2,((h>>16)&1)?mix(base,0xFFFFFF,40):mix(base,0x000000,45));
  }
}

// one cube; (cx,cy) = top vertex of its top face
static void mcCube(int cx,int cy,int tw,int th,int ch,uint8_t t,uint32_t seed,bool outline,bool tex){
  if(t<1 || t>8) return;
  uint32_t ct=mcShade(MC_TOP[t]);
  uint32_t cl=mcShade(mix(MC_SIDE[t],0x000000,40));
  uint32_t cr=mcShade(mix(MC_SIDE[t],0x000000,95));
  int Lx=cx-tw, Rx=cx+tw, Sy=cy+th, Bx=cx, By=cy+2*th;
  tft.fillTriangle(cx,cy,Rx,Sy,Bx,By,ct);        // top
  tft.fillTriangle(cx,cy,Bx,By,Lx,Sy,ct);
  tft.fillTriangle(Lx,Sy,Bx,By,Bx,By+ch,cl);     // left side
  tft.fillTriangle(Lx,Sy,Bx,By+ch,Lx,Sy+ch,cl);
  tft.fillTriangle(Bx,By,Rx,Sy,Rx,Sy+ch,cr);     // right side
  tft.fillTriangle(Bx,By,Rx,Sy+ch,Bx,By+ch,cr);
  if(t==1){                                       // grass edge on the sides
    int bt=ch/4+1;
    uint32_t gl=mcShade(mix(MC_TOP[1],0x000000,40)), gr=mcShade(mix(MC_TOP[1],0x000000,95));
    tft.fillTriangle(Lx,Sy,Bx,By,Bx,By+bt,gl);   tft.fillTriangle(Lx,Sy,Bx,By+bt,Lx,Sy+bt,gl);
    tft.fillTriangle(Bx,By,Rx,Sy,Rx,Sy+bt,gr);   tft.fillTriangle(Bx,By,Rx,Sy+bt,Bx,By+bt,gr);
  }
  if(t==5){                                       // log rings on top
    uint32_t rc=mcShade(0xC49A5A);
    int rw=tw/2, rh=th/2, mid=cy+th;
    tft.fillTriangle(cx,mid-rh,cx+rw,mid,cx,mid+rh,rc);
    tft.fillTriangle(cx,mid-rh,cx,mid+rh,cx-rw,mid,rc);
  }
  if(tex){
    if(t==1||t==2||t==3||t==4||t==6||t==8){
      mcSpeck(cx,cy,tw,th,-tw,th,seed,ct,6);
      mcSpeck(Lx,Sy,tw,th,0,ch,seed+101u,cl,4);
      mcSpeck(Bx,By,tw,-th,0,ch,seed+202u,cr,4);
    }
    if(t==7){                                     // brick mortar lines
      uint32_t ml=mcShade(mix(0xE0D8C8,0x000000,40)), mr=mcShade(mix(0xE0D8C8,0x000000,95));
      for(int k=1;k<3;k++){
        int yo=ch*k/3;
        tft.drawLine(Lx,Sy+yo,Bx,By+yo,ml);
        tft.drawLine(Bx,By+yo,Rx,Sy+yo,mr);
      }
    }
  }
  if(outline){
    uint32_t o=mcShade(mix(ct,0x000000,170));
    tft.drawLine(cx,cy,Rx,Sy,o); tft.drawLine(Rx,Sy,Bx,By,o); tft.drawLine(Bx,By,Lx,Sy,o); tft.drawLine(Lx,Sy,cx,cy,o);
    tft.drawLine(Lx,Sy,Lx,Sy+ch,o); tft.drawLine(Bx,By,Bx,By+ch,o); tft.drawLine(Rx,Sy,Rx,Sy+ch,o);
    tft.drawLine(Lx,Sy+ch,Bx,By+ch,o); tft.drawLine(Bx,By+ch,Rx,Sy+ch,o);
  }
}

// draw the whole 3D view (or only a clipped rectangle of it)
static void mcDrawWorld(int x0,int y0,int w,int h){
  if(w<=0){ x0=0; y0=0; w=224; h=240; }
  int x1=x0+w, y1=y0+h;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>224) x1=224;
  if(y1>240) y1=240;
  if(x1<=x0 || y1<=y0) return;
  tft.setClipRect(x0,y0,x1-x0,y1-y0);
  tft.fillRect(0,0,224,240,mcSkyColor());
  if(mcSky==2) for(int i=0;i<26;i++){ uint32_t hh=mcHash((uint32_t)i*131u+7u); tft.fillRect((int)(hh%222u),(int)((hh>>9)%180u),2,2,0xFFFFFF); }
  for(int z=0;z<MC_WZ;z++)
    for(int s=0;s<=MC_WX+MC_WY-2;s++)
      for(int vx=(s>MC_WY-1?s-(MC_WY-1):0); vx<=(s<MC_WX-1?s:MC_WX-1); vx++){
        int vy=s-vx;
        uint8_t t=mcGet(vx,vy,z);
        if(!t) continue;
        if(mcGet(vx+1,vy,z) && mcGet(vx,vy+1,z) && mcGet(vx,vy,z+1)) continue;   // fully hidden
        int cx=mcCX(vx,vy), cy=mcCY(vx,vy,z);
        if(cx+MC_TW2<x0 || cx-MC_TW2>x1 || cy+2*MC_TH2+MC_CH<y0 || cy>y1) continue;
        int wx,wy; mcToWorld(vx,vy,wx,wy);
        mcCube(cx,cy,MC_TW2,MC_TH2,MC_CH,t,(uint32_t)(wx*31+wy*17+z*7),mcOut,mcTex);
      }
  tft.clearClipRect();
}
static void mcRedrawCell(int vx,int vy,int z){
  int cx=mcCX(vx,vy), cy=mcCY(vx,vy,z);
  mcDrawWorld(cx-MC_TW2-1,cy-1,2*MC_TW2+3,2*MC_TH2+MC_CH+3);
}

// pick the block under a touch point (front-to-back). face: 0 top, 1 left(+y), 2 right(+x)
static bool mcPick(int px,int py,int &ox,int &oy,int &oz,int &face){
  for(int z=MC_WZ-1;z>=0;z--)
    for(int s=MC_WX+MC_WY-2;s>=0;s--)
      for(int vx=(s>MC_WY-1?s-(MC_WY-1):0); vx<=(s<MC_WX-1?s:MC_WX-1); vx++){
        int vy=s-vx;
        if(!mcGet(vx,vy,z)) continue;
        int dx=px-mcCX(vx,vy), dy=py-mcCY(vx,vy,z);
        if(dx<-MC_TW2 || dx>MC_TW2 || dy<0 || dy>2*MC_TH2+MC_CH) continue;
        if(abs(dx)*MC_TH2+abs(dy-MC_TH2)*MC_TW2<=MC_TW2*MC_TH2){ ox=vx; oy=vy; oz=z; face=0; return true; }
        int yt = (dx<0) ? MC_TH2+(dx+MC_TW2)*MC_TH2/MC_TW2 : 2*MC_TH2-dx*MC_TH2/MC_TW2;
        if(dy>=yt && dy<=yt+MC_CH){ ox=vx; oy=vy; oz=z; face=(dx<0)?1:2; return true; }
      }
  return false;
}

static void mcSlot(int i){
  int t=i+1, x=228+(i%2)*46, y=62+(i/2)*38;
  tft.fillRect(x,y,42,36,(mcSel==t)?0xFFFFFF:0x5A5A5A);
  tft.fillRect(x+2,y+2,38,32,0x1A1A1A);
  mcCube(x+21,y+8,11,6,9,(uint8_t)t,0,false,false);
}
static void mcDrawPanel(){
  tft.fillRect(224,0,96,240,0x2B2B2B);
  tft.drawFastVLine(224,0,240,0x000000);
  mcBtn(228,4,88,24,mcMode?"Break":"Place");
  mcBtn(228,32,88,24,"Turn");
  for(int i=0;i<8;i++) mcSlot(i);
  mcBtn(228,216,88,22,"Menu");
}
static void mcDrawGame(){
  tft.fillScreen(0x1A1A1A);
  mcDrawWorld(0,0,0,0);
  mcDrawPanel();
}

// ---------- main menu (stone buttons, big stone title, splash text) ----------
static void mcDrawMenu(){
  for(int y=0;y<240;y++) tft.drawFastHLine(0,y,320,mix(0x6FA8F0,0xD4E8FA,y*255/239));
  for(int i=0;i<40;i++){
    int h=160+(int)(sinf(i*0.33f)*16+sinf(i*0.85f)*7);
    tft.fillRect(i*8,h,8,240-h,0x4F9A34);
    tft.fillRect(i*8,h,8,5,0x6CBE48);
  }
  tft.fillRect(0,200,120,40,0x3E5FCF);
  const char* T="MINECRAFT";
  tft.setFont(F_XL);
  int x=160-tft.textWidth(T)/2, y=20;
  for(int d=5;d>=1;d--) txt(x+d,y+d,T,0x2A2A2A,F_XL);
  txt(x,y,T,0xB9B9B9,F_XL);
  txtc(262,76,"PocketOS!",0x3F3F00,F_M);                 // yellow splash (shadow first)
  txtc(261,75,"PocketOS!",0xFFFF00,F_M);
  mcBtn(60,104,200,28,"Play");
  mcBtn(60,140,200,28,"Settings");
  mcBtn(60,176,200,28,"Quit Game");
  txtc(161,227,"Fan-made - not affiliated with Mojang",0x2A2A2A,F_S);
  txtc(160,226,"Fan-made - not affiliated with Mojang",0xFFFFFF,F_S);
}

// ---------- settings screen (dirt background, 2 columns) ----------
static void mcDrawSettings(){
  tft.fillRect(0,0,320,240,0x3B2C1E);
  for(int i=0;i<260;i++){
    uint32_t h=mcHash((uint32_t)i*977u+5u);
    tft.fillRect((int)(h%80u)*4,(int)((h>>8)%60u)*4,4,4,((h>>16)&1)?0x4A3826:0x2E2218);
  }
  txtc(161,25,"Settings",0x000000,F_L);
  txtc(160,24,"Settings",0xFFFFFF,F_L);
  static const char* const skyN[3]={"Day","Sunset","Night"};
  char b[28];
  for(int i=0;i<6;i++){
    int bx=14+(i%2)*150, by=52+(i/2)*36;
    switch(i){
      case 0: snprintf(b,sizeof(b),"Sky: %s",skyN[mcSky]); break;
      case 1: snprintf(b,sizeof(b),"Outlines: %s",mcOut?"ON":"OFF"); break;
      case 2: snprintf(b,sizeof(b),"Textures: %s",mcTex?"ON":"OFF"); break;
      case 3: snprintf(b,sizeof(b),"Flip screen: %s",mcFlip?"ON":"OFF"); break;
      case 4: snprintf(b,sizeof(b),"Auto-save: %s",mcAuto?"ON":"OFF"); break;
      default:
        if(millis()-mcDoneAt<1500) snprintf(b,sizeof(b),"World reset!");
        else if(mcResetArm && millis()-mcResetArm<3000) snprintf(b,sizeof(b),"Tap again!");
        else snprintf(b,sizeof(b),"Reset world");
    }
    mcBtn(bx,by,142,28,b);
  }
  mcBtn(60,176,200,28,"Done");
}

// ---------- app entry / exit ----------
static void mcOpen(){
  mcLoadCfg();
  mcState=0; mcLoaded=false; mcRot=0; mcMode=0; mcResetArm=0;
  tft.setRotation(mcFlip?3:1);                       // landscape
  mcDrawMenu();
}
static void mcExit(){
  tft.setRotation(0);                                // back to portrait phone UI
  openPage("home");
}
static void mcStartGame(){
  if(!mcLoaded){ if(!mcLoadWorld()) mcGen(); mcLoaded=true; }
  mcState=2;
  mcDrawGame();
}

// ---------- touch ----------
static void mcSettingsPress(int x,int y){
  for(int i=0;i<6;i++){
    int bx=14+(i%2)*150, by=52+(i/2)*36;
    if(!hit(x,y,bx,by,142,28)) continue;
    switch(i){
      case 0: mcSky=(mcSky+1)%3; break;
      case 1: mcOut=!mcOut; break;
      case 2: mcTex=!mcTex; break;
      case 3: mcFlip=!mcFlip; tft.setRotation(mcFlip?3:1); break;
      case 4: mcAuto=!mcAuto; break;
      default:
        if(mcResetArm && millis()-mcResetArm<3000){
          mcGen(); mcLoaded=true; mcResetArm=0; mcDoneAt=millis();
          prefs.begin("pocketos",false); prefs.remove("mcw"); prefs.end();
        } else mcResetArm=millis();
    }
    mcSaveCfg();
    mcDrawSettings();
    return;
  }
  if(hit(x,y,60,176,200,28)){ mcSaveCfg(); mcState=0; mcDrawMenu(); }
}

static void mcGamePress(int x,int y){
  if(x>=224){
    if(hit(x,y,228,4,88,24)){ mcMode^=1; mcBtn(228,4,88,24,mcMode?"Break":"Place"); return; }
    if(hit(x,y,228,32,88,24)){ mcRot=(mcRot+1)&3; mcDrawWorld(0,0,0,0); return; }
    for(int i=0;i<8;i++){
      if(hit(x,y,228+(i%2)*46,62+(i/2)*38,42,36)){
        int old=mcSel-1;
        mcSel=i+1; mcSlot(old); mcSlot(i);
        if(mcMode){ mcMode=0; mcBtn(228,4,88,24,"Place"); }
        return;
      }
    }
    if(hit(x,y,228,216,88,22)){ if(mcAuto) mcSaveWorld(); mcState=0; mcDrawMenu(); }
    return;
  }
  int vx,vy,vz,face;
  if(!mcPick(x,y,vx,vy,vz,face)) return;
  if(mcMode==1){                                    // break
    mcSet(vx,vy,vz,0);
    mcRedrawCell(vx,vy,vz);
  } else {                                          // place on the touched face
    int tx=vx, ty=vy, tz=vz;
    if(face==0) tz++; else if(face==1) ty++; else tx++;
    if(mcInside(tx,ty,tz) && mcGet(tx,ty,tz)==0){
      mcSet(tx,ty,tz,(uint8_t)mcSel);
      mcRedrawCell(tx,ty,tz);
    }
  }
}

static void mcPress(int x,int y){
  if(mcState==0){
    if(hit(x,y,60,104,200,28)) mcStartGame();
    else if(hit(x,y,60,140,200,28)){ mcState=1; mcDrawSettings(); }
    else if(hit(x,y,60,176,200,28)) mcExit();
  }
  else if(mcState==1) mcSettingsPress(x,y);
  else mcGamePress(x,y);
}

// ===================== end Minecraft app =====================

void openPage(String p){
  page=p;
  if(p=="home") homeLockUntil=millis()+1000;   // 1 s cooldown after closing an app
  if(p=="lock") drawLock();
  else if(p=="home") drawHome();
  else if(p=="settings") drawSettings();
  else if(p=="pin") drawPinChange();
  else if(p=="wall") drawWallpaperPage();
  else if(p=="clock") drawClock();
  else if(p=="calc") drawCalc();
  else if(p=="phone") drawDialer();
  else if(p=="messages") drawMessages();
  else if(p=="camera") drawCamera();
  else if(p=="files") drawFiles();
  else if(p=="weather") infoPage("Weather",I_WEATHER,"22 C  Sunny","Demo data","No network yet");
  else if(p=="music") infoPage("Music",I_MUSIC,"No music yet","Demo screen","Add a player later");
  else if(p=="term") infoPage("Terminal",I_TERM,"PocketOS shell","Serial: 115200 baud","Demo screen");
  else if(p=="browser") drawBrowser();
  else if(p=="timer") drawTimer();
  else if(p=="stopwatch") drawStopwatch();
  else if(p=="flappy") drawFlappy();
  else if(p=="ttt") drawTTT();
  else if(p=="mc") mcOpen();
  else drawAbout();
}

// ---------- input handlers ----------
void padDigit(int k,int dotsY,bool onWall){
  if(entered.length()<4) entered += (char)(k==10 ? '0' : '1'+k);
  drawDots(dotsY,entered.length(),onWall,onWall?C_WHITE:accent());
}
void checkUnlock(){
  if(entered==pinCode){
    entered="";
    if(firstRun){ firstRun=false; save(); }
    openPage("home");
  } else {
    drawDots(126,4,true,C_RED);
    delay(350);
    entered="";
    drawDots(126,0,true,C_WHITE);
  }
}
void handleLock(int x,int y){
  if(!pinEnabled){ openPage("home"); return; }
  int k=keyAt(x,y,162);
  if(k<0) return;
  if(k==9){ entered=""; drawDots(126,0,true,C_WHITE); }
  else if(k==11) checkUnlock();
  else { padDigit(k,126,true); if(entered.length()==4) checkUnlock(); }
}
void handlePin(int x,int y){
  int k=keyAt(x,y,138);
  if(k<0) return;
  if(k==9){ entered=""; drawDots(100,0,false,accent()); }
  else if(k==11){
    if(entered.length()==4){
      pinCode=entered; save(); entered="";
      page="settings";
      toast("PIN saved");
    }
  } else padDigit(k,100,false);
}
void handleDialer(int x,int y){
  if(hit(x,y,190,58,50,36)){
    if(dial.length()){ dial.remove(dial.length()-1); drawDialNumber(); }
    return;
  }
  if(hit(x,y,70,272,100,26)){ if(dial.length()) toast("No cellular module"); return; }
  int k=keyAt(x,y,118);
  if(k>=0 && dial.length()<11){ dial += KEYS_DIAL[k]; drawDialNumber(); }
}
void handleHome(int x,int y){          // called on a tap (release without swiping)
  int n=(homePage==0)?9:6;
  for(int i=0;i<n;i++){
    int cx=40+(i%3)*80, top=34+(i/3)*72;
    if(hit(x,y,cx-36,top-4,72,70)){
      if(homePage==0){ if(i==8) entered=""; openPage(GRID_PAGE[i]); }
      else openPage(P2_PAGE[i]);
      return;
    }
  }
  for(int i=0;i<3;i++) if(hit(x,y,50+i*70-30,258,60,54)){ openPage(DOCK_PAGE[i]); return; }
}
void handleSettings(int x,int y){
  if(hit(x,y,176,24,32,28)){ if(setScroll>0){ setScroll--; drawSettings(); } return; }
  if(hit(x,y,204,24,32,28)){ if(setScroll+5<NSET){ setScroll++; drawSettings(); } return; }
  if(y<62) return;
  int r=(y-62)/48;
  if((y-62)%48>=42 || r>4) return;
  int idx=setScroll+r;
  if(idx>=NSET) return;
  switch(idx){
    case 0: openPage("wall"); return;
    case 1: accentIdx=(accentIdx+1)%5; break;
    case 2: brIdx=(brIdx+1)%4; tft.setBrightness(BR_V[brIdx]); break;
    case 3: pinEnabled=!pinEnabled; break;
    case 4: entered=""; openPage("pin"); return;
    case 5: labelsOn=!labelsOn; break;
    case 6: clockOn=!clockOn; break;
    case 7: entered=""; openPage("lock"); return;
    case 8: openPage("about"); return;
    case 9:
      if(millis()-resetArm<3000){
        prefs.begin("pocketos",false); prefs.clear(); prefs.end();
        wall=0; accentIdx=0; brIdx=3; pinEnabled=true; labelsOn=true; clockOn=true;
        firstRun=true; pinCode="1234"; setScroll=0; resetArm=0; fBest=0;
        tft.setBrightness(BR_V[brIdx]);
        toast("Settings reset");
      } else {
        resetArm=millis();
        toast("Tap again to confirm");
      }
      return;
  }
  save();
  drawSettings();
}
void handleCalc(int x,int y){
  for(int i=0;i<20;i++){
    int bx=10+(i%4)*57, by=122+(i/4)*36;
    if(hit(x,y,bx,by,51,32)){ calcKey(CK[i]); drawCalcDisplay(); return; }
  }
}

// ---------- setup / loop ----------
void setup(){
  Serial.begin(115200);
  load();
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(BR_V[brIdx]);
  Serial.println("Display started");
  randomSeed(micros());
  // SD uses the common CYD TF-slot pins. If your board revision differs,
  // SD init will fail without affecting the rest of PocketOS.
  sdSPI.begin(SD_SCK_PIN,SD_MISO_PIN,SD_MOSI_PIN,SD_CS_PIN);
  sdReady = SD.begin(SD_CS_PIN,sdSPI,10000000);
  Serial.println(sdReady ? "SD card mounted" : "SD card not mounted");
  if(sdReady) refreshSDEntries();
  startNetwork();
  lastMin=millis()/60000;
  openPage(pinEnabled?"lock":"home");
}

void onPress(int x,int y){             // pressed on any page except home
  if(page=="mc"){ mcPress(x,y); return; }   // Minecraft handles its own landscape UI
  if(page!="lock"){
    if(y>=302){ openPage("home"); return; }
    if(x<64 && y>=22 && y<56){
      if(page=="pin"||page=="wall"||page=="about"){ entered=""; openPage("settings"); }
      else openPage("home");
      return;
    }
  }
  if(page=="lock") handleLock(x,y);
  else if(page=="settings") handleSettings(x,y);
  else if(page=="pin") handlePin(x,y);
  else if(page=="phone") handleDialer(x,y);
  else if(page=="calc") handleCalc(x,y);
  else if(page=="browser") handleBrowser(x,y);
  else if(page=="timer") handleTimer(x,y);
  else if(page=="stopwatch") handleStopwatch(x,y);
  else if(page=="flappy"){ if(y>=TOPY) flappyTap(); }
  else if(page=="ttt") tttTap(x,y);
  else if(page=="wall"){
    for(int i=0;i<4;i++){
      if(hit(x,y,10+(i%2)*116,62+(i/2)*122,104,110)){ wall=i; save(); drawWallpaperPage(); return; }
    }
  }
  else if(page=="camera"){
    int dx=x-120, dy=y-268;
    if(dx*dx+dy*dy<=30*30) toast("No camera module");
  }
  else if(page=="files") handleFiles(x,y);
}

void loop(){
  unsigned long now=millis();

  if(now/60000!=lastMin){                       // once a minute
    lastMin=now/60000;
    if(page=="home") drawStatusBar(true);
    else if(page=="lock"){ drawStatusBar(true); drawLockClock(); }
    else if(page!="mc") drawStatusBar(false);
  }
  if(page=="clock" && now-lastClock>=1000){ lastClock=now; drawClockFace(); }
  if(page=="timer" && now-lastFeatureRedraw>=200){ lastFeatureRedraw=now; drawTimer(); }
  if(page=="stopwatch" && now-lastFeatureRedraw>=100){ lastFeatureRedraw=now; drawStopwatch(); }
  if(page=="flappy") flappyTick(now);
  if(page=="ttt") tttTick(now);

  // ----- touch: press / drag / release (swipe on home) -----
  static bool touching=false;
  static String pressPage="";
  static unsigned long lastTouchMs=0;
  static int sx=0, sy=0, lx=0, ly=0;
  int32_t tx=0, ty=0;
  bool t=tft.getTouch(&tx,&ty);
  if(t && (tx<0 || tx>=tft.width() || ty<0 || ty>=tft.height())) t=false;

  if(t){
    lastTouchMs=now;
    if(!touching){
      touching=true; sx=lx=tx; sy=ly=ty;
      pressPage=page;                              // remember where the press started
      if(page!="home" && now-lastTap>=80){ lastTap=now; onPress(tx,ty); }
    } else { lx=tx; ly=ty; }
    return;
  }
  if(touching && now-lastTouchMs>60){              // finger lifted
    touching=false;
    if(page=="home" && pressPage=="home" && now>=homeLockUntil){
      int dx=lx-sx, dy=ly-sy;
      if(abs(dx)>=40 && abs(dx)>abs(dy)){          // swipe
        if(dx<0 && homePage<HOME_PAGES-1){ homePage++; drawHome(); }
        else if(dx>0 && homePage>0){ homePage--; drawHome(); }
      } else if(abs(dx)<15 && abs(dy)<15){         // tap
        handleHome(sx,sy);
      }
    }
  }
}
