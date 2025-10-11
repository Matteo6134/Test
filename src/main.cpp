#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <CST816S.h>
#include <Preferences.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

// NOTA: I font personalizzati (es. FreeSans) richiedono l'installazione
// della libreria "Adafruit GFX". Per garantire la compilazione, sono stati
// sostituiti con i font integrati di LovyanGFX.

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
      cfg.panel_width = 240; cfg.panel_height = 240;
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

// --- Struttura Dati ---
struct Esercizio { char nome[30]; int serie; int ripetizioni; };
struct GiornoAllenamento { char nomeGiorno[50]; char gruppiMuscolari[50]; Esercizio esercizi[10]; int numeroEsercizi; };
GiornoAllenamento miaScheda[7];
int numeroGiorniTotali = 0;

// --- Stati e Variabili Globali ---
enum AppState { STATE_WORKOUT, STATE_MENU, STATE_SLEEP, STATE_WIFI_CONFIG };
AppState currentState = STATE_MENU;
static AppState transitionToState; // Stato di destinazione per le transizioni

int giornoCorrente = 0, esercizioCorrente = 0;
int completedSets = 0; // Numero di set completati (0..n-1)
int menuItemPressed = -1; // Indice del menu premuto
unsigned long ultimaAttivitaMillis = 0;
const unsigned long tempoSleepMode = 600000; // 10 minuti

LGFX tft;
Preferences preferences;
DNSServer dnsServer;
AsyncWebServer server(80);
const char* const ssid_ap = "GymBuddy-Setup";

// Variabili per animazione e debounce
unsigned long ultimoToccoMillis = 0;
const int intervalloDebounce = 300;
bool isAnimating = false;
unsigned long animStartTime;
const unsigned long animationDurationDefault = 600;
unsigned long animationDuration = animationDurationDefault;
int scrollOffset = 0;
unsigned long lastScrollTime = 0;
const int scrollSpeed = 100;

// Variabili per transizioni
bool isTransitioning = false;
float transitionProgress = 0.0f; // 0.0 a 1.0
int transitionDirection = 1;     // 1: da destra a sinistra, -1: da sinistra a destra
LGFX_Sprite bufA(&tft), bufB(&tft);
const int SCREEN_W = 240;
const int SCREEN_H = 240;

// Prototipi Funzioni
void drawWorkoutToSprite(LGFX_Sprite* canvas);
void drawMenuToSprite(LGFX_Sprite* canvas);
void drawSleepToSprite(LGFX_Sprite* canvas);
void drawWifiConfigToSprite(LGFX_Sprite* canvas);
void enterWifiConfigMode();
void exitWifiConfigMode();
void enterSleepMode();
void wakeUp();
void deserializeWorkout(String data);
void saveWorkoutToMemory();
void loadWorkoutFromMemory();
void loadDefaultWorkout();
void drawSeriesDotsOnCanvas(LGFX_Sprite* canvas, int completed, int total, bool animating);
void startTransition(int direction, AppState from, AppState to);
void performTransitionFrame();

// --- Setup ---
void setup() {
  Serial.begin(115200);
  tft.begin();
  touch.begin();
  pinMode(3, OUTPUT); digitalWrite(3, HIGH); // Abilita retroilluminazione

  // Crea i buffer per le transizioni
  bufA.createSprite(SCREEN_W, SCREEN_H);
  bufB.createSprite(SCREEN_W, SCREEN_H);
  bufA.setColorDepth(8);
  bufB.setColorDepth(8);

  if (!SPIFFS.begin(true)) { Serial.println("Errore SPIFFS"); return; }
  
  preferences.begin("gymbuddy", false);
  if (preferences.isKey("has_data")) {
    loadWorkoutFromMemory();
  } else {
    loadDefaultWorkout();
    saveWorkoutToMemory();
  }

  ultimaAttivitaMillis = millis();
  giornoCorrente = 0;
  esercizioCorrente = 0;
  completedSets = 0;
  
  // Disegna la schermata iniziale (Menu)
  drawMenuToSprite(&bufA);
  bufA.pushSprite(0, 0);
}

// --- Loop Principale ---
void loop() {
  if (isTransitioning) {
    performTransitionFrame();
    return; // Durante la transizione, non eseguire altro
  }

  if (currentState == STATE_WIFI_CONFIG) {
    dnsServer.processNextRequest();
  }

  // Timeout per la modalità sleep
  if (currentState != STATE_SLEEP && currentState != STATE_WIFI_CONFIG && (millis() - ultimaAttivitaMillis > tempoSleepMode)) {
    enterSleepMode();
  }

  // Gestione del tocco
  if (touch.available()) {
    ultimaAttivitaMillis = millis();
    if (currentState == STATE_SLEEP) {
      wakeUp();
      return;
    }

    // Gestures
    if (touch.data.gestureID == SWIPE_UP && (currentState == STATE_MENU || currentState == STATE_WORKOUT)) {
      enterWifiConfigMode();
      return;
    }
    if (touch.data.gestureID == SWIPE_DOWN && currentState == STATE_WIFI_CONFIG) {
      exitWifiConfigMode();
      return;
    }
    if (touch.data.gestureID == SWIPE_DOWN && currentState == STATE_WORKOUT) {
      startTransition(-1, STATE_WORKOUT, STATE_MENU);
      return;
    }
    if (touch.data.gestureID == SWIPE_RIGHT && currentState == STATE_MENU) {
      startTransition(1, STATE_MENU, STATE_WORKOUT);
      return;
    }
     if (touch.data.gestureID == SWIPE_LEFT && currentState == STATE_WORKOUT) {
      startTransition(-1, STATE_WORKOUT, STATE_MENU);
      return;
    }

    // Tocco singolo
    switch (currentState) {
      case STATE_WORKOUT: {
        if (!isAnimating && (millis() - ultimoToccoMillis > intervalloDebounce)) {
          ultimoToccoMillis = millis();
          animStartTime = millis();
          isAnimating = true;
          animationDuration = animationDurationDefault;
        }
        break;
      }
      case STATE_MENU: {
        int touchY = touch.data.y;
        int itemHeight = 60, startY = 20;
        for (int i = 0; i < numeroGiorniTotali; i++) {
          if (touchY > startY + (i * itemHeight) && touchY < startY + (i * itemHeight) + (itemHeight - 10)) {
            menuItemPressed = i;
            // Redraw per feedback visivo della pressione
            drawMenuToSprite(&bufA);
            bufA.pushSprite(0, 0);
            delay(140);
            menuItemPressed = -1;

            giornoCorrente = i;
            esercizioCorrente = 0;
            completedSets = 0;
            isAnimating = false;
            startTransition(1, STATE_MENU, STATE_WORKOUT);
            return;
          }
        }
        break;
      }
      default: break;
    }
  }

  // Gestione animazione del set completato
  if (isAnimating) {
    drawWorkoutToSprite(&bufA);
    bufA.pushSprite(0, 0);

    unsigned long elapsed = millis() - animStartTime;
    if (elapsed >= animationDuration) {
      isAnimating = false;
      Esercizio& ex = miaScheda[giornoCorrente].esercizi[esercizioCorrente];
      completedSets++;
      if (completedSets >= ex.serie) {
        completedSets = 0;
        esercizioCorrente++;
        if (esercizioCorrente >= miaScheda[giornoCorrente].numeroEsercizi) {
          esercizioCorrente = 0;
          giornoCorrente = (giornoCorrente + 1) % max(1, numeroGiorniTotali);
          startTransition(-1, STATE_WORKOUT, STATE_MENU);
        }
      }
      // Disegna l'ultimo frame con lo stato aggiornato
      drawWorkoutToSprite(&bufA);
      bufA.pushSprite(0, 0);
    }
  }

  // Redraw periodico per lo scroll del testo (solo se necessario)
  if (currentState == STATE_WORKOUT && !isAnimating) {
    static unsigned long lastRedraw = 0;
    if (millis() - lastRedraw > scrollSpeed) {
      lastRedraw = millis();
      drawWorkoutToSprite(&bufA);
      bufA.pushSprite(0, 0);
    }
  }
}

// =======================================================================
//   GESTIONE STATI E MODALITA'
// =======================================================================
void enterSleepMode() {
  currentState = STATE_SLEEP;
  tft.setBrightness(10);
  drawSleepToSprite(&bufA);
  bufA.pushSprite(0, 0);
}

void wakeUp() {
  tft.setBrightness(255);
  // Il prossimo stato dopo il risveglio sarà il menu
  startTransition(1, STATE_SLEEP, STATE_MENU);
}

void enterWifiConfigMode() {
  currentState = STATE_WIFI_CONFIG;
  ultimaAttivitaMillis = millis();
  WiFi.softAP(ssid_ap);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists("/index.html")) {
      request->send(SPIFFS, "/index.html", "text/html", false);
    } else {
      request->send(404, "text/plain", "File non trovato. Caricare il filesystem.");
    }
  });

  server.on("/getWorkout", HTTP_GET, [](AsyncWebServerRequest *request) {
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

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("workoutData", true)) {
      String workout = request->getParam("workoutData", true)->value();
      deserializeWorkout(workout);
      saveWorkoutToMemory();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Dati mancanti");
    }
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });

  server.begin();
  drawWifiConfigToSprite(&bufA);
  bufA.pushSprite(0, 0);
}

void exitWifiConfigMode() {
  server.end();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  startTransition(1, STATE_WIFI_CONFIG, STATE_MENU);
}

// =======================================================================
//   GESTIONE DATI (SALVATAGGIO, CARICAMENTO, PARSING)
// =======================================================================
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
      dayData.substring(0, pipe1).toCharArray(miaScheda[numeroGiorniTotali].nomeGiorno, 50);
      dayData.substring(pipe1 + 1, pipe2).toCharArray(miaScheda[numeroGiorniTotali].gruppiMuscolari, 50);
      String exercises = dayData.substring(pipe2 + 1);
      miaScheda[numeroGiorniTotali].numeroEsercizi = 0;
      int exStart = 0;
      while (exStart < exercises.length() && miaScheda[numeroGiorniTotali].numeroEsercizi < 10) {
        int exEnd = exercises.indexOf(',', exStart);
        if (exEnd == -1) exEnd = exercises.length();
        String exData = exercises.substring(exStart, exEnd);
        int colon1 = exData.indexOf(':');
        int colon2 = exData.indexOf(':', colon1 + 1);
        if (colon1 > 0 && colon2 > 0) {
          int idx = miaScheda[numeroGiorniTotali].numeroEsercizi;
          exData.substring(0, colon1).toCharArray(miaScheda[numeroGiorniTotali].esercizi[idx].nome, 30);
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
    preferences.getString((p + "_n").c_str(), "").toCharArray(miaScheda[d].nomeGiorno, 50);
    preferences.getString((p + "_gm").c_str(), "").toCharArray(miaScheda[d].gruppiMuscolari, 50);
    miaScheda[d].numeroEsercizi = preferences.getInt((p + "_ne").c_str(), 0);
    for (int e = 0; e < miaScheda[d].numeroEsercizi; e++) {
      String ep = p + "e" + String(e);
      preferences.getString((ep + "_n").c_str(), "").toCharArray(miaScheda[d].esercizi[e].nome, 30);
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

// =======================================================================
//   FUNZIONI DI DISEGNO SU SPRITE
// =======================================================================
void drawMenuToSprite(LGFX_Sprite* canvas) {
  canvas->fillScreen(TFT_BLACK);
  canvas->setTextDatum(MC_DATUM);
  int centroX = SCREEN_W / 2, itemHeight = 60, startY = 20;
  for (int i = 0; i < numeroGiorniTotali; i++) {
    int itemY = startY + (i * itemHeight);
    uint16_t colorGiorno = (i == menuItemPressed) ? TFT_GREEN : TFT_WHITE;
    uint16_t colorMuscoli = (i == menuItemPressed) ? TFT_GREEN : 0x7BEF;
    canvas->setFont(&fonts::Font4); // Sostituito FreeSansBold18pt7b
    canvas->setTextSize(1.5);
    canvas->setTextColor(colorGiorno);
    canvas->drawString(miaScheda[i].nomeGiorno, centroX, itemY + 18);
    canvas->setTextSize(1);
    canvas->setFont(&fonts::Font2); // Sostituito FreeSans9pt7b
    canvas->setTextColor(colorMuscoli);
    canvas->drawString(miaScheda[i].gruppiMuscolari, centroX, itemY + 38);
    if (i < numeroGiorniTotali - 1) {
      canvas->drawLine(20, itemY + 55, SCREEN_W - 20, itemY + 55, 0x2104);
    }
  }
}

void drawSleepToSprite(LGFX_Sprite* canvas) {
  canvas->fillScreen(TFT_BLACK);
  int centroX = SCREEN_W / 2, centroY = SCREEN_H / 2;
  canvas->drawCircle(centroX, centroY - 20, 30, 0x4208);
  canvas->drawCircle(centroX, centroY - 20, 29, 0x4208);
  canvas->setTextColor(0x4208);
  canvas->setTextDatum(MC_DATUM);
  canvas->setFont(&fonts::Font7); // Sostituito FreeSansBold18pt7b
  canvas->drawString("Z", centroX, centroY - 15);
  canvas->setFont(&fonts::Font2); // Sostituito FreeSans9pt7b
  canvas->setTextColor(0x7BEF);
  canvas->drawString("Tocca per", centroX, centroY + 30);
  canvas->drawString("risvegliare", centroX, centroY + 50);
}

void drawWorkoutToSprite(LGFX_Sprite* canvas) {
  canvas->fillScreen(TFT_BLACK);
  Esercizio& ex = miaScheda[giornoCorrente].esercizi[esercizioCorrente];
  int centroX = SCREEN_W / 2, centroY = SCREEN_H / 2;
  int raggio = min(SCREEN_W, SCREEN_H) / 2 - 10, spessore = 12;

  float targetProgress = (float)completedSets / ex.serie;
  if (isAnimating) {
    unsigned long elapsed = millis() - animStartTime;
    float t = (float)elapsed / animationDuration;
    if (t > 1.0f) t = 1.0f;
    float easedT = 1 - powf(1 - t, 3); // easeOutCubic
    float startProgress = (float)completedSets / ex.serie;
    float endProgress = (float)(completedSets + 1) / ex.serie;
    targetProgress = startProgress + (endProgress - startProgress) * easedT;
    if (targetProgress > 1.0f) targetProgress = 1.0f;
  }

  // Barra circolare
  canvas->fillArc(centroX, centroY, raggio, raggio - spessore, 0, 360, 0x2104);
  int startAngle = 270;
  int endAngle = startAngle + (int)(360 * targetProgress);
  canvas->fillArc(centroX, centroY, raggio, raggio - spessore, startAngle, endAngle, TFT_GREEN);

  // Testo esercizio (con scrolling se necessario)
  canvas->setTextColor(TFT_WHITE);
  canvas->setTextDatum(MC_DATUM);
  canvas->setFont(&fonts::Font4); // Sostituito FreeSansBold12pt7b
  String nomeEsercizio = String(ex.nome);
  int textWidth = canvas->textWidth(nomeEsercizio);
  int maxWidth = SCREEN_W - 40;

  if (textWidth > maxWidth) {
    String scrollText = nomeEsercizio + "      " + nomeEsercizio;
    if (millis() - lastScrollTime > scrollSpeed) {
      scrollOffset++;
      if (scrollOffset > textWidth + 50) scrollOffset = 0; // 50 = padding
      lastScrollTime = millis();
    }
    LGFX_Sprite textSprite(canvas->getParent());
    textSprite.createSprite(maxWidth, 35);
    textSprite.setColorDepth(8);
    textSprite.fillScreen(TFT_BLACK);
    textSprite.setFont(&fonts::Font4); // Sostituito FreeSansBold12pt7b
    textSprite.setTextColor(TFT_WHITE);
    textSprite.setTextDatum(ML_DATUM);
    textSprite.drawString(scrollText, -scrollOffset, 18);
    textSprite.pushSprite(centroX - (maxWidth / 2), centroY - 25);
    textSprite.deleteSprite();
  } else {
    canvas->drawString(nomeEsercizio, centroX, centroY - 10);
    scrollOffset = 0;
  }

  // Info Ripetizioni
  canvas->setFont(&fonts::Font4); // Sostituito FreeSans12pt7b
  canvas->setTextColor(0x7BEF);
  char bufferRep[20];
  sprintf(bufferRep, "%d reps", ex.ripetizioni);
  canvas->drawString(bufferRep, centroX, centroY + 25);

  // Pallini Serie
  drawSeriesDotsOnCanvas(canvas, completedSets, ex.serie, isAnimating);
}

void drawWifiConfigToSprite(LGFX_Sprite* canvas) {
  canvas->fillScreen(TFT_BLACK);
  canvas->setTextDatum(MC_DATUM);
  canvas->setFont(&fonts::Font4); // Sostituito FreeSansBold12pt7b
  canvas->setTextColor(TFT_WHITE);
  canvas->drawString("Configurazione", SCREEN_W / 2, 30);
  if (WiFi.softAPgetStationNum() > 0) {
    canvas->fillCircle(SCREEN_W / 2, 80, 20, TFT_GREEN);
    canvas->setTextColor(TFT_BLACK);
    canvas->setFont(&fonts::Font4); // Sostituito FreeSansBold12pt7b
    canvas->drawString(">", SCREEN_W / 2, 81);
    canvas->setFont(&fonts::Font2); // Sostituito FreeSans9pt7b
    canvas->setTextColor(TFT_GREEN);
    canvas->drawString("Dispositivo connesso!", SCREEN_W / 2, 130);
    canvas->setTextColor(0x7BEF);
    canvas->drawString("Invia la scheda dal browser.", SCREEN_W / 2, 155);
  } else {
    canvas->setFont(&fonts::Font2); // Sostituito FreeSans9pt7b
    canvas->setTextColor(0x7BEF);
    canvas->drawString("1. Connettiti alla rete:", SCREEN_W / 2, 90);
    canvas->setTextColor(TFT_CYAN);
    canvas->setFont(&fonts::Font4); // Sostituito FreeSansBold9pt7b (usiamo Font4 per maggiore leggibilità)
    canvas->drawString(ssid_ap, SCREEN_W / 2, 115);
    canvas->setFont(&fonts::Font2); // Sostituito FreeSans9pt7b
    canvas->setTextColor(0x7BEF);
    canvas->drawString("2. Si aprira' una pagina", SCREEN_W / 2, 150);
  }
  canvas->setTextColor(0x4208);
  canvas->drawString("Swipe giu per uscire", SCREEN_W / 2, SCREEN_H - 20);
}

void drawSeriesDotsOnCanvas(LGFX_Sprite* canvas, int completed, int total, bool animating) {
  int centroX = SCREEN_W / 2, y = SCREEN_H - 40, radius = 6, spacing = 25;
  int startX = centroX - ((total - 1) * spacing / 2);
  for (int i = 0; i < total; i++) {
    uint16_t color;
    if (i < completed) {
      color = TFT_GREEN;
    } else if (i == completed && animating) {
      unsigned long elapsed = millis() - animStartTime;
      float t = (float)elapsed / animationDuration;
      if (t > 1.0f) t = 1.0f;
      // Anima il colore da grigio a verde
      uint8_t r_start=32, g_start=64, b_start=32; // Colore 0x2104
      uint8_t r_end=0,   g_end=255,  b_end=0;    // Colore TFT_GREEN
      uint8_t r = r_start + (r_end - r_start) * t;
      uint8_t g = g_start + (g_end - g_start) * t;
      uint8_t b = b_start + (b_end - b_start) * t;
      color = canvas->color565(r, g, b);
    } else {
      color = 0x2104; // Grigio scuro
    }
    canvas->fillCircle(startX + (i * spacing), y, radius, color);
  }
}

// =======================================================================
//   FUNZIONI DI TRANSIZIONE
// =======================================================================
void startTransition(int direction, AppState from, AppState to) {
  if (isTransitioning) return;
  isTransitioning = true;
  transitionProgress = 0.0f;
  transitionDirection = direction;
  transitionToState = to;

  // Disegna la schermata di partenza in bufA
  switch (from) {
    case STATE_MENU: drawMenuToSprite(&bufA); break;
    case STATE_WORKOUT: drawWorkoutToSprite(&bufA); break;
    case STATE_SLEEP: drawSleepToSprite(&bufA); break;
    case STATE_WIFI_CONFIG: drawWifiConfigToSprite(&bufA); break;
  }
  // Disegna la schermata di arrivo in bufB
  switch (to) {
    case STATE_MENU: drawMenuToSprite(&bufB); break;
    case STATE_WORKOUT: drawWorkoutToSprite(&bufB); break;
    case STATE_SLEEP: drawSleepToSprite(&bufB); break;
    case STATE_WIFI_CONFIG: drawWifiConfigToSprite(&bufB); break;
  }
}

void performTransitionFrame() {
  const float speed = 0.06f;
  transitionProgress += speed;
  if (transitionProgress >= 1.0f) {
    transitionProgress = 1.0f;
  }

  // Funzione di easing per un movimento più morbido
  float easedProgress = 0.5f - 0.5f * cos(transitionProgress * PI);
  
  int xA, xB;
  if (transitionDirection > 0) { // La nuova schermata (B) arriva da destra
    xA = -(int)(SCREEN_W * easedProgress);
    xB = SCREEN_W - (int)(SCREEN_W * easedProgress);
  } else { // La nuova schermata (B) arriva da sinistra
    xA = (int)(SCREEN_W * easedProgress);
    xB = -SCREEN_W + (int)(SCREEN_W * easedProgress);
  }

  tft.startWrite();
  bufA.pushSprite(xA, 0);
  bufB.pushSprite(xB, 0);
  tft.endWrite();

  if (transitionProgress >= 1.0f) {
    isTransitioning = false;
    currentState = transitionToState;
    // Copia il buffer finale (B) nel buffer principale (A) per coerenza
    bufB.pushSprite(&bufA, 0, 0);
  }
}

