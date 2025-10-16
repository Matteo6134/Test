#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <CST816S.h>
#include <Preferences.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "qrcode.h"
#include <cmath> // Aggiunto per le funzioni matematiche (cos, sin, round)

// =======================================================================
//  COSTANTI DI CONFIGURAZIONE GLOBALE (NO "MAGIC NUMBERS")
// =======================================================================
// --- Colori ---
const uint16_t COLOR_BACKGROUND              = TFT_BLACK;
const uint16_t COLOR_PROGRESS_BAR_BG         = 0x2104;
const uint16_t COLOR_PROGRESS_BAR_FG         = TFT_GREEN;
const uint16_t COLOR_TEXT_PRIMARY            = TFT_WHITE;
const uint16_t COLOR_TEXT_SECONDARY          = 0x7BEF;
const uint16_t COLOR_TEXT_TERTIARY           = 0x4208;
const uint16_t COLOR_MENU_ITEM_PRESSED       = TFT_GREEN;
const uint16_t COLOR_WIFI_QR_TEXT            = TFT_CYAN;
const uint16_t COLOR_MENU_SEPARATOR          = 0x2104;

// --- Timing e Animazioni ---
const unsigned long ANIMATION_DURATION_SET   = 600; // ms
const unsigned long DEBOUNCE_DELAY           = 500; // ms 
const unsigned long SLEEP_MODE_TIMEOUT       = 600000; // 10 minuti

// --- Layout ---
const int PADDING_HORIZONTAL                 = 20;
const int SCREEN_W                           = 240;
const int SCREEN_H                           = 240;

// --- Limiti Dati ---
const size_t MAX_EXERCISE_NAME_LEN    = 30;
const size_t MAX_DAY_NAME_LEN         = 50;
const size_t MAX_MUSCLE_GROUP_LEN     = 50;


// --- Configurazione LovyanGFX ---
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX(void) {
    { auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST; cfg.freq_write = 40000000;
      cfg.pin_sclk = 6; cfg.pin_mosi = 7; cfg.pin_miso = -1; cfg.pin_dc = 2;
      _bus_instance.config(cfg); _panel_instance.setBus(&_bus_instance);
    } { auto cfg = _panel_instance.config();
      cfg.pin_cs = 10; cfg.pin_rst = -1;
      cfg.panel_width = SCREEN_W; cfg.panel_height = SCREEN_H;
      cfg.invert = true; cfg.rgb_order = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

// --- Configurazione Touch ---
#define TOUCH_SDA 4
#define TOUCH_SCL 5
#define TOUCH_RST 1
#define TOUCH_INT 0
CST816S touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

// --- Struttura Dati Globale ---
struct Esercizio { char nome[MAX_EXERCISE_NAME_LEN]; int serie; int ripetizioni; };
struct GiornoAllenamento { char nomeGiorno[MAX_DAY_NAME_LEN]; char gruppiMuscolari[MAX_MUSCLE_GROUP_LEN]; Esercizio esercizi[10]; int numeroEsercizi; };
GiornoAllenamento miaScheda[7];
int numeroGiorniTotali = 0;
int giornoCorrente = 0;

// --- Variabili Globali di Sistema ---
LGFX tft;
Preferences preferences;
DNSServer dnsServer;
AsyncWebServer server(80);
const char* const ssid_ap = "GymBuddy-Setup";

// =======================================================================
//  ARCHITETTURA A OGGETTI PER LE SCHERMATE
// =======================================================================

class Screen;

// --- NUOVO: Enumeratore per il tipo di transizione ---
enum TransitionType { HORIZONTAL, VERTICAL };

// --- Gestore di Schermate e Transizioni ---
Screen* currentScreen = nullptr;
Screen* transitionToScreen = nullptr;
bool isTransitioning = false;
float transitionProgress = 0.0f;
int transitionDirection = 1;
TransitionType currentTransitionType = HORIZONTAL; // Memorizza il tipo di transizione corrente
LGFX_Sprite bufA(&tft), bufB(&tft);
unsigned long ignoreTouchUntilMillis = 0;
unsigned long ultimaAttivitaMillis = 0;
bool needsRedraw = true; // Flag per ottimizzare il ciclo di disegno

// --- Prototipi e Dichiarazioni Anticipate ---
// MODIFICA: Aggiornata la firma della funzione per accettare il tipo di transizione
void changeScreen(Screen* newScreen, int direction, TransitionType type = HORIZONTAL);
void performTransitionFrame();
void drawQrCode(LGFX_Sprite* canvas, int x_offset, int y_offset, QRCode* qrcode, int scale);
void drawSeriesDotsOnCanvas(LGFX_Sprite* canvas, int completed, int total, bool animating, unsigned long animStart, unsigned long animDur);
void deserializeWorkout(String data);
void saveWorkoutToMemory();
void loadWorkoutFromMemory();
void loadDefaultWorkout();

// --- Classe Base per tutte le schermate ---
class Screen {
public:
    virtual ~Screen() {}
    virtual void onEnter() {}
    virtual void onExit() {}
    virtual void update() {}
    virtual void handleInput(CST816S &touch_dev) {}
    virtual void draw(LGFX_Sprite* canvas) = 0;
    virtual bool isAnimating() const { return false; }
};

// Istanza unica per ogni schermata
Screen* menuScreen;
Screen* workoutScreen;
Screen* wifiConfigScreen;
Screen* sleepScreen;
Screen* completionScreen; 


// --- Definizione della Classe MenuScreen ---
class MenuScreen : public Screen {
private:
    int menuItemPressed = -1;
public:
    void onEnter() override { menuItemPressed = -1; }

    void handleInput(CST816S &touch_dev) override {
        if (touch_dev.data.gestureID == SWIPE_UP) {
            // MODIFICA: Specifica una transizione VERTICALE
            changeScreen(wifiConfigScreen, 1, VERTICAL);
            return;
        }
        if (touch_dev.data.gestureID == SWIPE_RIGHT) {
            // MODIFICA: Specifica una transizione ORIZZONTALE
            changeScreen(workoutScreen, 1, HORIZONTAL);
            return;
        }

        int touchY = touch_dev.data.y;
        int itemHeight = 60, startY = 20;
        for (int i = 0; i < numeroGiorniTotali; i++) {
            if (touchY > startY + (i * itemHeight) && touchY < startY + (i * itemHeight) + (itemHeight - 10)) {
                menuItemPressed = i;
                giornoCorrente = i;
                // MODIFICA: Specifica una transizione ORIZZONTALE
                changeScreen(workoutScreen, 1, HORIZONTAL);
                return;
            }
        }
    }

    void draw(LGFX_Sprite* canvas) override {
        canvas->fillScreen(COLOR_BACKGROUND);
        canvas->setTextDatum(MC_DATUM);
        int centroX = SCREEN_W / 2, itemHeight = 60, startY = 20;
        for (int i = 0; i < numeroGiorniTotali; i++) {
            int itemY = startY + (i * itemHeight);
            uint16_t colorGiorno = (i == menuItemPressed) ? COLOR_MENU_ITEM_PRESSED : COLOR_TEXT_PRIMARY;
            uint16_t colorMuscoli = (i == menuItemPressed) ? COLOR_MENU_ITEM_PRESSED : COLOR_TEXT_SECONDARY;
            
            canvas->setFont(&fonts::Font4);
            canvas->setTextSize(1.5);
            canvas->setTextColor(colorGiorno);
            canvas->drawString(miaScheda[i].nomeGiorno, centroX, itemY + 18);
            
            canvas->setTextSize(1);
            canvas->setFont(&fonts::Font2);
            canvas->setTextColor(colorMuscoli);
            canvas->drawString(miaScheda[i].gruppiMuscolari, centroX, itemY + 38);
            
            if (i < numeroGiorniTotali - 1) {
                canvas->drawLine(PADDING_HORIZONTAL, itemY + 55, SCREEN_W - PADDING_HORIZONTAL, itemY + 55, COLOR_MENU_SEPARATOR);
            }
        }
    }
};

// --- Definizione della Classe WorkoutScreen ---
class WorkoutScreen : public Screen {
private:
    bool animating = false;
    unsigned long animStartTime = 0;
    unsigned long animationDuration = ANIMATION_DURATION_SET;
    int scrollOffset = 0;
    unsigned long lastScrollTime = 0;
    const int scrollSpeed = 100;
    int esercizioCorrente = 0;
    int completedSets = 0;

public:
    void onEnter() override {
        esercizioCorrente = 0;
        completedSets = 0;
        animating = false;
        needsRedraw = true;
    }

    bool isAnimating() const override { return animating; }

    void handleInput(CST816S &touch_dev) override {
        if (touch_dev.data.gestureID == SWIPE_RIGHT || touch_dev.data.gestureID == SWIPE_DOWN) {
            changeScreen(menuScreen, -1, HORIZONTAL);
            return;
        }
        if (!animating && (millis() - animStartTime > 300)) {
            animStartTime = millis();
            animating = true;
        }
    }

    void update() override {
        if (animating) {
            unsigned long elapsed = millis() - animStartTime;
            if (elapsed >= animationDuration) {
                animating = false;
                completedSets++;
                Esercizio& ex = miaScheda[giornoCorrente].esercizi[esercizioCorrente];
                if (completedSets >= ex.serie) {
                    completedSets = 0;
                    esercizioCorrente++;
                    if (esercizioCorrente >= miaScheda[giornoCorrente].numeroEsercizi) {
                        giornoCorrente = (giornoCorrente + 1) % max(1, numeroGiorniTotali);
                        changeScreen(completionScreen, 1, HORIZONTAL); 
                    }
                }
            }
            needsRedraw = true;
        }
    }

    void draw(LGFX_Sprite* canvas) override {
        canvas->fillScreen(COLOR_BACKGROUND);
        Esercizio& ex = miaScheda[giornoCorrente].esercizi[esercizioCorrente];
        int centroX = SCREEN_W / 2, centroY = SCREEN_H / 2;
        int raggio = min(SCREEN_W, SCREEN_H) / 2 - 3, spessore = 12;

        float targetProgress = (float)completedSets / ex.serie;
        if (animating) {
            unsigned long elapsed = millis() - animStartTime;
            float t = (float)elapsed / animationDuration;
            if (t > 1.0f) t = 1.0f;
            float easedT = 1 - powf(1 - t, 3);
            float startProgress = (float)completedSets / ex.serie;
            float endProgress = (float)(completedSets + 1) / ex.serie;
            targetProgress = startProgress + (endProgress - startProgress) * easedT;
        }
        
        canvas->fillArc(centroX, centroY, raggio, raggio - spessore, 0, 360, COLOR_PROGRESS_BAR_BG);

        if (targetProgress > 0.001f) {
            int start_angle = 270;
            int end_angle = 270 + (int)(360 * targetProgress);
            canvas->fillArc(centroX, centroY, raggio, raggio - spessore, start_angle, end_angle, COLOR_PROGRESS_BAR_FG);
            
            float mid_radius = raggio - spessore / 2.0f;
            float cap_radius = spessore / 2.0f;

            float start_angle_rad = start_angle * PI / 180.0f;
            int start_cap_x = round(centroX + mid_radius * cos(start_angle_rad));
            int start_cap_y = round(centroY + mid_radius * sin(start_angle_rad));
            canvas->fillCircle(start_cap_x, start_cap_y, cap_radius, COLOR_PROGRESS_BAR_FG);

            float end_angle_rad = end_angle * PI / 180.0f;
            int end_cap_x = round(centroX + mid_radius * cos(end_angle_rad));
            int end_cap_y = round(centroY + mid_radius * sin(end_angle_rad));
            canvas->fillCircle(end_cap_x, end_cap_y, cap_radius, COLOR_PROGRESS_BAR_FG);
        }

        canvas->setTextColor(COLOR_TEXT_PRIMARY);
        canvas->setTextDatum(MC_DATUM);
        canvas->setFont(&fonts::Font4);
        String nomeEsercizio = String(ex.nome);
        if (canvas->textWidth(nomeEsercizio) > SCREEN_W - 40) {
            if (millis() - lastScrollTime > scrollSpeed) {
                scrollOffset++;
                if (scrollOffset > canvas->textWidth(nomeEsercizio) + 50) scrollOffset = 0;
                lastScrollTime = millis();
            }
            canvas->drawString(nomeEsercizio + "   " + nomeEsercizio, centroX - scrollOffset, centroY - 10);
        } else {
            canvas->drawString(nomeEsercizio, centroX, centroY - 10);
            scrollOffset = 0;
        }

        canvas->setFont(&fonts::Font4);
        canvas->setTextColor(COLOR_TEXT_SECONDARY);
        char bufferRep[20];
        sprintf(bufferRep, "%d reps", ex.ripetizioni);
        canvas->drawString(bufferRep, centroX, centroY + 25);
        drawSeriesDotsOnCanvas(canvas, completedSets, ex.serie, animating, animStartTime, animationDuration);
    }
};

// --- Definizione della Classe WifiConfigScreen ---
class WifiConfigScreen : public Screen {
public:
    void onEnter() override {
        WiFi.softAP(ssid_ap);
        dnsServer.start(53, "*", WiFi.softAPIP());
        server.begin();
    }
    void onExit() override {
        server.end();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
    }
    void update() override { dnsServer.processNextRequest(); }
    void handleInput(CST816S &touch_dev) override {
        if (touch_dev.data.gestureID == SWIPE_DOWN) {
            changeScreen(menuScreen, -1, VERTICAL);
        }
    }
    void draw(LGFX_Sprite* canvas) override {
        canvas->fillScreen(COLOR_BACKGROUND);
        canvas->setTextDatum(MC_DATUM);

        const char* qr_str = "WIFI:T:nopass;S:GymBuddy-Setup;;";
        QRCode qrcode;
        uint8_t qrcodeData[qrcode_getBufferSize(3)];
        qrcode_initText(&qrcode, qrcodeData, 3, 0, qr_str);

        int qr_pixel_size = qrcode.size * 5;
        int x_pos = (SCREEN_W - qr_pixel_size) / 2;
        int total_height = qr_pixel_size + 20;
        int y_pos = (SCREEN_H - total_height) / 2;
        
        drawQrCode(canvas, x_pos, y_pos, &qrcode, 5);

        canvas->setFont(&fonts::Font2);
        canvas->setTextColor(COLOR_WIFI_QR_TEXT);
        canvas->drawString("Inquadra per connetterti", SCREEN_W / 2, y_pos + qr_pixel_size + 15);
    }
};

// --- Definizione della Classe SleepScreen ---
class SleepScreen : public Screen {
public:
    void onEnter() override { tft.setBrightness(10); }
    void onExit() override { tft.setBrightness(255); }
    void handleInput(CST816S &touch_dev) override {
        changeScreen(menuScreen, 1, VERTICAL);
    }
    void draw(LGFX_Sprite* canvas) override {
        canvas->fillScreen(COLOR_BACKGROUND);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawCircle(SCREEN_W/2, SCREEN_H/2 - 20, 30, COLOR_TEXT_TERTIARY);
        canvas->drawCircle(SCREEN_W/2, SCREEN_H/2 - 20, 29, COLOR_TEXT_TERTIARY);
        canvas->setFont(&fonts::Font7);
        canvas->setTextColor(COLOR_TEXT_TERTIARY);
        canvas->drawString("Z", SCREEN_W/2, SCREEN_H/2 - 15);
        canvas->setFont(&fonts::Font2);
        canvas->setTextColor(COLOR_TEXT_SECONDARY);
        canvas->drawString("Tocca per risvegliare", SCREEN_W/2, SCREEN_H/2 + 35);
    }
};

// --- Definizione della Classe CompletionScreen con timer ---
class CompletionScreen : public Screen {
private:
    unsigned long enterTime = 0;
    const unsigned long displayDuration = 3000; // Durata in ms (3 secondi)

public:
    void onEnter() override {
        enterTime = millis();
    }

    void update() override {
        if (millis() - enterTime > displayDuration) {
            changeScreen(menuScreen, -1, HORIZONTAL);
        }
    }

    void handleInput(CST816S &touch_dev) override {
        // Qualsiasi tocco o swipe riporta al menu, anche prima che scada il tempo
        changeScreen(menuScreen, -1, HORIZONTAL);
    }

    void draw(LGFX_Sprite* canvas) override {
        canvas->fillScreen(COLOR_BACKGROUND);
        canvas->setTextDatum(MC_DATUM);

        int centerX = SCREEN_W / 2;
        int centerY = SCREEN_H / 2;

        // Cerchio verde di sfondo
        canvas->fillCircle(centerX, centerY - 30, 45, COLOR_PROGRESS_BAR_FG);
        
        // INIZIO DELLA CORREZIONE: Sostituito fillThickLine con fillTriangle per disegnare il segno di spunta
        // Questo è necessario perché la versione di LovyanGFX in uso non supporta fillThickLine.
        // Costruiamo manualmente due quadrilateri (uno per ogni gamba del segno di spunta)
        // e li disegniamo usando due triangoli ciascuno.
        float thickness = 8.0f;

        // --- Prima (più corta) gamba del segno di spunta ---
        float x1=centerX-22, y1=centerY-30;
        float x2=centerX-5, y2=centerY-10;
        float dx1 = x2 - x1, dy1 = y2 - y1;
        float len1 = sqrt(dx1*dx1 + dy1*dy1);
        float px1 = -dy1 / len1 * (thickness / 2.0f);
        float py1 = dx1 / len1 * (thickness / 2.0f);

        canvas->fillTriangle(round(x1 + px1), round(y1 + py1), 
                             round(x2 + px1), round(y2 + py1), 
                             round(x2 - px1), round(y2 - py1), COLOR_TEXT_PRIMARY);
        canvas->fillTriangle(round(x1 + px1), round(y1 + py1), 
                             round(x2 - px1), round(y2 - py1), 
                             round(x1 - px1), round(y1 - py1), COLOR_TEXT_PRIMARY);

        // --- Seconda (più lunga) gamba del segno di spunta ---
        float x3=centerX+22, y3=centerY-50;
        float dx2 = x3 - x2, dy2 = y3 - y2;
        float len2 = sqrt(dx2*dx2 + dy2*dy2);
        float px2 = -dy2 / len2 * (thickness / 2.0f);
        float py2 = dx2 / len2 * (thickness / 2.0f);

        canvas->fillTriangle(round(x2 + px2), round(y2 + py2), 
                             round(x3 + px2), round(y3 + py2), 
                             round(x3 - px2), round(y3 - py2), COLOR_TEXT_PRIMARY);
        canvas->fillTriangle(round(x2 + px2), round(y2 + py2), 
                             round(x3 - px2), round(y3 - py2), 
                             round(x2 - px2), round(y2 - py2), COLOR_TEXT_PRIMARY);
        // FINE DELLA CORREZIONE

        // Messaggio di completamento
        canvas->setFont(&fonts::Font4);
        canvas->setTextColor(COLOR_TEXT_PRIMARY);
        canvas->drawString("Complimenti!", centerX, centerY + 45);
        
        canvas->setFont(&fonts::Font2);
        canvas->setTextColor(COLOR_TEXT_SECONDARY);
        canvas->drawString("Allenamento completato", centerX, centerY + 70);
    }
};


// --- Funzione di Setup Principale ---
void setup() {
  Serial.begin(115200);
  tft.begin();
  touch.begin();
  pinMode(3, OUTPUT); digitalWrite(3, HIGH);

  bufA.createSprite(SCREEN_W, SCREEN_H);
  bufB.createSprite(SCREEN_W, SCREEN_H);
  bufA.setColorDepth(8);
  bufB.setColorDepth(8);

  if (!SPIFFS.begin(true)) { Serial.println("Errore SPIFFS"); return; }
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ 
    if (SPIFFS.exists("/index.html")) {
        request->send(SPIFFS, "/index.html", "text/html", false);
    } else {
        request->send(404, "text/plain", "File non trovato.");
    }
  });
  server.on("/getWorkout", HTTP_GET, [](AsyncWebServerRequest *request){
    String workoutString = "";
    for (int d = 0; d < numeroGiorniTotali; d++) {
      if (d > 0) workoutString += ";";
      workoutString += String(miaScheda[d].nomeGiorno) + "|" + String(miaScheda[d].gruppiMuscolari) + "|";
      for (int e = 0; e < miaScheda[d].numeroEsercizi; e++) {
        if (e > 0) workoutString += ",";
        workoutString += String(miaScheda[d].esercizi[e].nome) + ":" + String(miaScheda[d].esercizi[e].serie) + ":" + String(miaScheda[d].esercizi[e].ripetizioni);
      }
    }
    request->send(200, "text/plain", workoutString);
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("workoutData", true)) {
      String workout = request->getParam("workoutData", true)->value();
      deserializeWorkout(workout);
      saveWorkoutToMemory();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Dati mancanti");
    }
  });
  server.onNotFound([](AsyncWebServerRequest *request){ request->redirect("/"); });

  preferences.begin("gymbuddy", false);
  if (preferences.isKey("has_data")) { loadWorkoutFromMemory(); } 
  else { loadDefaultWorkout(); saveWorkoutToMemory(); }

  menuScreen = new MenuScreen();
  workoutScreen = new WorkoutScreen();
  wifiConfigScreen = new WifiConfigScreen();
  sleepScreen = new SleepScreen();
  completionScreen = new CompletionScreen(); 

  currentScreen = menuScreen;
  currentScreen->onEnter();
  ultimaAttivitaMillis = millis();
}

// --- Loop Principale OTTIMIZZATO ---
void loop() {
    if (isTransitioning) {
        performTransitionFrame();
        return;
    }

    if (currentScreen) {
        currentScreen->update();

        if (touch.available()) {
            if (millis() < ignoreTouchUntilMillis) return;
            ultimaAttivitaMillis = millis();
            currentScreen->handleInput(touch);
            needsRedraw = true; 
        }
    }

    if (currentScreen != sleepScreen && currentScreen != wifiConfigScreen && (millis() - ultimaAttivitaMillis > SLEEP_MODE_TIMEOUT)) {
        changeScreen(sleepScreen, 1, VERTICAL);
        return; 
    }

    if (needsRedraw || (currentScreen && currentScreen->isAnimating())) {
        if (currentScreen && !isTransitioning) {
            currentScreen->draw(&bufA);
            bufA.pushSprite(0, 0);
            needsRedraw = false;
        }
    }
}

// =======================================================================
//  IMPLEMENTAZIONE FUNZIONI AUSILIARIE
// =======================================================================

void changeScreen(Screen* newScreen, int direction, TransitionType type) {
  if (isTransitioning || !newScreen) return;
  
  ignoreTouchUntilMillis = millis() + DEBOUNCE_DELAY;

  if(currentScreen) currentScreen->onExit();
  
  transitionToScreen = newScreen;
  transitionDirection = direction;
  transitionProgress = 0.0f;
  currentTransitionType = type;

  currentScreen->draw(&bufA);
  transitionToScreen->onEnter(); 
  transitionToScreen->draw(&bufB);
  
  isTransitioning = true;
}

void performTransitionFrame() {
  const float speed = 0.06f;
  transitionProgress += speed;
  if (transitionProgress >= 1.0f) { transitionProgress = 1.0f; }

  float easedProgress = 0.5f - 0.5f * cos(transitionProgress * PI);
  
  int xA = 0, yA = 0, xB = 0, yB = 0;

  if (currentTransitionType == HORIZONTAL) {
    xA = (transitionDirection > 0) ? -(SCREEN_W * easedProgress) : (SCREEN_W * easedProgress);
    xB = (transitionDirection > 0) ? SCREEN_W - (int)(SCREEN_W * easedProgress) : -SCREEN_W + (int)(SCREEN_W * easedProgress);
  } else { // VERTICAL
    yA = (transitionDirection > 0) ? -(SCREEN_H * easedProgress) : (SCREEN_H * easedProgress);
    yB = (transitionDirection > 0) ? SCREEN_H - (int)(SCREEN_H * easedProgress) : -SCREEN_H + (int)(SCREEN_H * easedProgress);
  }

  tft.startWrite();
  bufA.pushSprite(xA, yA);
  bufB.pushSprite(xB, yB);
  tft.endWrite();

  if (transitionProgress >= 1.0f) {
    isTransitioning = false;
    currentScreen = transitionToScreen;
    needsRedraw = true;
  }
}

void drawQrCode(LGFX_Sprite* canvas, int x_offset, int y_offset, QRCode* qrcode, int scale) {
    for (uint8_t y = 0; y < qrcode->size; y++) {
        for (uint8_t x = 0; x < qrcode->size; x++) {
            if (qrcode_getModule(qrcode, x, y)) {
                canvas->fillRect(x_offset + x * scale, y_offset + y * scale, scale, scale, COLOR_TEXT_PRIMARY);
            }
        }
    }
}

void drawSeriesDotsOnCanvas(LGFX_Sprite* canvas, int completed, int total, bool animating, unsigned long animStart, unsigned long animDur) {
  int centroX = SCREEN_W / 2, y = SCREEN_H - 40, radius = 6, spacing = 25;
  int startX = centroX - ((total - 1) * spacing / 2);
  for (int i = 0; i < total; i++) {
    uint16_t color = COLOR_PROGRESS_BAR_BG;
    if (i < completed) {
      color = COLOR_PROGRESS_BAR_FG;
    } else if (i == completed && animating) {
      float t = (float)(millis() - animStart) / animDur;
      if (t > 1.0f) t = 1.0f;
      uint16_t color_bg = COLOR_PROGRESS_BAR_BG;
      uint16_t color_fg = COLOR_PROGRESS_BAR_FG;
      uint8_t r_bg = (color_bg >> 11) & 0x1F;
      uint8_t g_bg = (color_bg >> 5)  & 0x3F;
      uint8_t b_bg = color_bg & 0x1F;
      uint8_t r_fg = (color_fg >> 11) & 0x1F;
      uint8_t g_fg = (color_fg >> 5)  & 0x3F;
      uint8_t b_fg = color_fg & 0x1F;
      uint8_t r = r_bg + (int)((r_fg - r_bg) * t);
      uint8_t g = g_bg + (int)((g_fg - g_bg) * t);
      uint8_t b = b_bg + (int)((b_fg - b_bg) * t);
      color = (r << 11) | (g << 5) | b;
    }
    canvas->fillCircle(startX + (i * spacing), y, radius, color);
  }
}

void deserializeWorkout(String data) {
    numeroGiorniTotali = 0;
    int dayStart = 0;
    while (dayStart < data.length() && numeroGiorniTotali < 7) {
        int dayEnd = data.indexOf(';', dayStart);
        if (dayEnd == -1) dayEnd = data.length();
        String dayData = data.substring(dayStart, dayEnd);
        int pipe1 = dayData.indexOf('|');
        int pipe2 = dayData.indexOf('|', pipe1 + 1);
        if (pipe1 > 0 && pipe2 > pipe1) {
            String nomeGiornoStr = dayData.substring(0, pipe1);
            strncpy(miaScheda[numeroGiorniTotali].nomeGiorno, nomeGiornoStr.c_str(), MAX_DAY_NAME_LEN - 1);
            miaScheda[numeroGiorniTotali].nomeGiorno[MAX_DAY_NAME_LEN - 1] = '\0';

            String gruppiMuscolariStr = dayData.substring(pipe1 + 1, pipe2);
            strncpy(miaScheda[numeroGiorniTotali].gruppiMuscolari, gruppiMuscolariStr.c_str(), MAX_MUSCLE_GROUP_LEN - 1);
            miaScheda[numeroGiorniTotali].gruppiMuscolari[MAX_MUSCLE_GROUP_LEN - 1] = '\0';

            String exercises = dayData.substring(pipe2 + 1);
            miaScheda[numeroGiorniTotali].numeroEsercizi = 0;
            int exStart = 0;
            while (exStart < exercises.length() && miaScheda[numeroGiorniTotali].numeroEsercizi < 10) {
                int exEnd = exercises.indexOf(',', exStart);
                if (exEnd == -1) exEnd = exercises.length();
                String exData = exercises.substring(exStart, exEnd);
                int colon1 = exData.indexOf(':');
                int colon2 = exData.indexOf(':', colon1 + 1);
                if (colon1 > 0 && colon2 > colon1) {
                    int idx = miaScheda[numeroGiorniTotali].numeroEsercizi;
                    
                    String nomeExStr = exData.substring(0, colon1);
                    strncpy(miaScheda[numeroGiorniTotali].esercizi[idx].nome, nomeExStr.c_str(), MAX_EXERCISE_NAME_LEN - 1);
                    miaScheda[numeroGiorniTotali].esercizi[idx].nome[MAX_EXERCISE_NAME_LEN - 1] = '\0';

                    miaScheda[numeroGiorniTotali].esercizi[idx].serie = exData.substring(colon1 + 1, colon2).toInt();
                    miaScheda[numeroGiorniTotali].esercizi[idx].ripetizioni = exData.substring(colon2 + 1).toInt();
                    miaScheda[numeroGiorniTotali].numeroEsercizi++;
                }
                exStart = exEnd + 1;
            }
            numeroGiorniTotali++;
        }
        dayStart = dayEnd + 1;
    }
}


void saveWorkoutToMemory() {
  preferences.clear();
  preferences.putInt("num_days", numeroGiorniTotali);
  for (int d = 0; d < numeroGiorniTotali; d++) {
    String p = "d" + String(d);
    preferences.putString((p + "_n").c_str(), miaScheda[d].nomeGiorno);
    preferences.putString((p + "_gm").c_str(), miaScheda[d].gruppiMuscolari);
    preferences.putInt((p + "_ne").c_str(), miaScheda[d].numeroEsercizi);
    for (int e = 0; e < miaScheda[d].numeroEsercizi; e++) {
      String ep = p + "e" + String(e);
      preferences.putString((ep + "_n").c_str(), miaScheda[d].esercizi[e].nome);
      preferences.putInt((ep + "_s").c_str(), miaScheda[d].esercizi[e].serie);
      preferences.putInt((ep + "_r").c_str(), miaScheda[d].esercizi[e].ripetizioni);
    }
  }
  preferences.putBool("has_data", true);
}

void loadWorkoutFromMemory() {
  numeroGiorniTotali = preferences.getInt("num_days", 0);
  for (int d = 0; d < numeroGiorniTotali; d++) {
    String p = "d" + String(d);
    preferences.getString((p + "_n").c_str(), "").toCharArray(miaScheda[d].nomeGiorno, MAX_DAY_NAME_LEN);
    preferences.getString((p + "_gm").c_str(), "").toCharArray(miaScheda[d].gruppiMuscolari, MAX_MUSCLE_GROUP_LEN);
    miaScheda[d].numeroEsercizi = preferences.getInt((p + "_ne").c_str(), 0);
    for (int e = 0; e < miaScheda[d].numeroEsercizi; e++) {
      String ep = p + "e" + String(e);
      preferences.getString((ep + "_n").c_str(), "").toCharArray(miaScheda[d].esercizi[e].nome, MAX_EXERCISE_NAME_LEN);
      miaScheda[d].esercizi[e].serie = preferences.getInt((ep + "_s").c_str(), 0);
      miaScheda[d].esercizi[e].ripetizioni = preferences.getInt((ep + "_r").c_str(), 0);
    }
  }
}

void loadDefaultWorkout() {
  numeroGiorniTotali = 1;
  strcpy(miaScheda[0].nomeGiorno, "Esempio");
  strcpy(miaScheda[0].gruppiMuscolari, "Petto");
  miaScheda[0].numeroEsercizi = 2;
  strcpy(miaScheda[0].esercizi[0].nome, "Panca Piana");
  miaScheda[0].esercizi[0].serie = 4;
  miaScheda[0].esercizi[0].ripetizioni = 8;
  strcpy(miaScheda[0].esercizi[1].nome, "Spinte Manubri");
  miaScheda[0].esercizi[1].serie = 3;
  miaScheda[0].esercizi[1].ripetizioni = 10;
}

