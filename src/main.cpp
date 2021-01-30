#include <Arduino.h>

#include <THiNXLib.h>
THiNX thx;

#define TESTING

//

// SSD1306

#include <Wire.h>  // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306.h" // alias for `#include "SSD1306Wire.h"`
#include "OLEDDisplayUi.h"
#include "images.h"

SSD1306  display(0x3c, D5, D6);
OLEDDisplayUi ui     ( &display );

int LAST_EAV = 0; // latest measured value
int graph[128] = {0};

#define MODE_MEASURE 0
#define MODE_STIMULATE 1

#define BUTTON_PIN D4
#define SIGOUT_PIN D2

int mode = MODE_MEASURE;
long debounce = 0; // millis until button will be debounced
bool perform_graph_reset = false; // trigger for graph reset
long max_result = 0; // measured maximum
long max_result_time = 0; // time from measured maximum
long max_result_interval = 0; // interval from measured maximum
int graph_loop_counter = 0;

void measurementOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->setFont(ArialMT_Plain_16);
  display->drawString(128, 0, String(LAST_EAV));
}

void stateOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);

  if (mode == MODE_MEASURE) {
    if (max_result_interval) {
      // show max_result when interval falls...? over-enginnered prototype
      display->drawString(0, 0, String(max_result_interval/1000) + String("s -") + String(max_result-LAST_EAV) + String("%"));
    } else {
      display->drawString(0, 0, F("MEASURE"));
    }
  } else {
    display->drawString(0, 0, F("STIMULATE"));
  }
}

void drawFrame1(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

  // Draw line [graph]
  for (int gx = 0; gx < 128; gx++) {

    if ( (gx % 4) == 0 ) {
      display->setPixel (gx + x, 15);
      display->setPixel (gx + x, 31);
      display->setPixel (gx + x, 63);
    }

    int gy = 64 - (graph[gx] - 25); // 100% - 64 = 36 / 2 = 18 margin - 8 to ignore bottom levels
    display->setPixel (gx + x, gy + y);

    // Draw black pixel divider from old history line
    display->setColor (BLACK);
    display->setPixel (gx + x + 1, gy + y);
    display->setColor (WHITE);

  }
}

void drawFrame2(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

  return;

  // draw an xbm image.
  // Please note that everything that should be transitioned
  // needs to be drawn relative to x and y

  display->drawXbm(x + 34, y + 14, WiFi_Logo_width, WiFi_Logo_height, WiFi_Logo_bits);

  // void setPixel(int16_t x, int16_t y);

  /*
  // Demonstrates the 3 included default sizes. The fonts come from SSD1306Fonts.h file
  // Besides the default fonts there will be a program to convert TrueType fonts into this format
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(0 + x, 10 + y, "Arial 10");

  display->setFont(ArialMT_Plain_16);
  display->drawString(0 + x, 20 + y, "Arial 16");

  display->setFont(ArialMT_Plain_24);
  display->drawString(0 + x, 34 + y, "Arial 24");
  */
}

void drawFrame3(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  // Text alignment demo
  display->setFont(ArialMT_Plain_10);

  // The coordinates define the left starting point of the text
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0 + x, 11 + y, "Left aligned (0,10)");

  // The coordinates define the center of the text
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64 + x, 22 + y, "Center aligned (64,22)");

  // The coordinates define the right end of the text
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(128 + x, 33 + y, "Right aligned (128,33)");
}

void drawFrame4(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  // Demo for drawStringMaxWidth:
  // with the third parameter you can define the width after which words will be wrapped.
  // Currently only spaces and "-" are allowed for wrapping
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawStringMaxWidth(0 + x, 10 + y, 128, "Lorem ipsum\n dolor sit amet, consetetur sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore.");
}

void drawFrame5(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

}

// SSD1306

// This array keeps function pointers to all frames
// frames are the single views that slide in
FrameCallback frames[] = { drawFrame1, drawFrame2, drawFrame3, drawFrame4, drawFrame5 };

// how many frames are there?
int frameCount = 1;

// Overlays are statically drawn on top of a frame eg. a clock
OverlayCallback overlays[] = { stateOverlay, measurementOverlay };
int overlaysCount = 2;

// Print out value on A0 only.
// Minimum EAV without any resistors, works only with Wemos D1 mini pro.
// Human connects between GND and A0 and that's it.

void setup() {

#ifdef TESTING
  Serial.begin(230400);
  Serial.setTimeout(2000);
  Serial.flush();

  // Wait for serial to initialize.
  Serial.println("\nI'm awake.");
  THiNX::logging = true;
#endif

  // Works, but should use WiFiManager
  WiFi.mode(WIFI_STA);
  WiFi.begin("THiNX-IoT", "<enter-your-ssid-password>");
  delay(2000); // wait for DHCP

  // TODO: Encrypt those values using DevSec
  const char *api_key = "dc534a02cc9b7677282f9cc2d2d514677c703eca0fef258c93f56ce0f08a23c2";
  const char *owner_id = "886d515f173e4698f15140366113b7c98c678401b815a592d88c866d13bf5445";

  thx = THiNX(api_key, owner_id);

  Serial.println("Configuring hardware...");

  pinMode(A0, INPUT);
  pinMode(BUTTON_PIN, INPUT);

  /*
#ifdef TESTING
  Serial.println("Going into deep sleep for 5 seconds");
  Serial.println(millis());
  ESP.deepSleep(5e6); // 5e6 is 5000000 microseconds
#endif
  */

  // The ESP is capable of rendering 60fps in 80Mhz mode
	// but that won't give you much time for anything else
	// run it in 160Mhz mode or just set it to 30 fps
  ui.setTargetFPS(30);
  //ui.setActiveSymbol(activeSymbol);
  //ui.setInactiveSymbol(inactiveSymbol);
  //ui.setIndicatorPosition(BOTTOM); // TOP, LEFT, BOTTOM, RIGHT
  //ui.setIndicatorDirection(LEFT_RIGHT);
  //ui.setFrameAnimation(SLIDE_LEFT); // SLIDE_LEFT, SLIDE_RIGHT, SLIDE_UP, SLIDE_DOWN
  ui.setFrames(frames, frameCount);
  ui.setOverlays(overlays, overlaysCount);
  ui.disableAutoTransition();
  ui.disableAllIndicators();
  ui.disableIndicator();

  // Initialising the UI will init the display too.
  ui.init();
  display.flipScreenVertically();

#ifdef TESTING
  Serial.println("Main loop started...");
#endif
}

/*
 * ESP8266 technical notes

- wakes up when RST goes to GND and HIGH (rising side); therefore button cannot be evaluated after reset
- timer resets as well

*/

/*

CZ pro Vlčka:

Při vývoji software jsem bohužel narazil na nějaké komplikace. ESP8266 se probouzí vzestupnou hranou z LOW na HIGH. Pokud není připojeno, je HIGH.
Takže po vypršení časovače se nezapne, dokud se nepropojí D0 s RST.
Na sondě by tedy musely být neustále 3,3V, aby se při dotyku zařízení probralo.
Zároveň by se ale při měření musela sonda od signálu RST nějak odpojit.

Nejjednodušším řešením bude asi vypínač.

Zkusil jsem tedy navrhnout řešení, kdy se použije dvojité tlačítko. Jeden pár spojuje RST a D0,
takže se zařízení po libovolném time-outu (nejdéle po hodině) probudí až po stisknutí tlačítka.
Po startu se pak na D0 nastaví výstup 0, aby stisk tlačítka nezpůsobil reset během měření.
Druhý pár tlačítka se pak použije při měření pro krok na další bod.

Usnulo by to samo po neaktivitě.

---

Dotknu se sondou a pustim. Uzemní se RST a zase se odzemní, tím se to zapne. Do sondy se tím pádem musí pustit 3,3V až po zapnutí.

ESP8266 se probouzí samo nejdéle po 71 minutách (dané maximální možnou hodnotou pro nastavení časovače).
Tím pádem je nutné rozpoznat, zda bylo probuzeno časovačem (výstup pinu D0), nebo tlačítkem připojeném na RST.
Pin D0 musí být připojen na RST a ESP8266 se probudí na vzestupné hraně LOW/HIGH signálu na resetu.

Pokud ale věc spí a RST je propojen se zemí přes tlačítko, stisknutím se probudí po libovolné době (akorát ESP asi do nějaké míry naběhne) .

PROBUZENÍ TLAČÍTKEM + SONDOU
Nejsnadnější technické řešení probuzení je takové, že uživatel se dotkne hrotem sondy a stiskne tlačítko.
Při puštění tlačítka se systém probudí, změří hodnotu na vstupu a pokud bude hodnota vyšší než malá,
zapne se. V opačném případě jde o probuzení časovačem nebo omylem a zařízení se opět uvede do spánku.

Problém je, že tlačítko potřebujeme (i) na jiném pinu než RST a pokud se používá jako ovladač, nesmí být připojeno na RST.
Tedy smí být připojeno na RST, jenom když procesor spí.

PROBUZENÍ JEN TLAČÍTKEM
V důsledku toho lze sice probudit tlačítkem, ale není snadné poznat, že to bylo právě tlačítkem a ne časovačem.
Poznámka: Pokud je obvod zapnutý a RST je spojeno s GND (LOW), není možné nahrát firmware.

*/

int measure = 0;

int MAX_INT = 1024;
int MIN_IN = 10; // (5 is one human noise, 10 is two)
int MIN_AP = 110;

int transform(int measure) {
  if (measure > 1000) return MAX_INT;
  int result = measure / 10;
  return result;
}

/* Format result as string for output */
String format(int measure) {
  if (measure == MAX_INT) {
    return F("SHORT");
  } else if (measure > 100) {
    return String(measure) + " (AP)";
  } else {
    return String(measure);
  }
}


void reset_graph() {
  for (int i = 0; i < 128; i++) {
    graph[i] = 0;
  }
  graph_loop_counter = 0;
  max_result = 0;
  max_result_time = 0;
  perform_graph_reset = false;
}


void loop() {

  measure = analogRead(A0);

  // Enable reset when signal falls to zero
  if (measure < LAST_EAV) {
    if (measure < MIN_IN) {
      perform_graph_reset = true;
    }
  }

  int result = transform(measure);

  if (perform_graph_reset && (result - 10 > MIN_IN) ) {
    reset_graph();
  }

  LAST_EAV = result;

  if (result > MIN_IN) {

    Serial.print(result);
    Serial.print(" > ");
    Serial.println(format(result));

    if (result > max_result) {
      max_result = result;
      max_result_time = millis();
    }

    max_result_interval = millis() - max_result_time;

    // increase and save graph value
    graph_loop_counter++;
    if (graph_loop_counter > 128) {
      graph_loop_counter = 0;
    }
    graph[graph_loop_counter] = result;
  }

  thx.loop();

   int remainingTimeBudget = ui.update();
   if (remainingTimeBudget > 0) {

     // You can do some work here
     // Don't do stuff if you are below your
     // time budget.

     // Use button to switch mode with 500ms debouce
     int button_state = digitalRead(BUTTON_PIN);
     if (button_state == LOW) {
       if (millis() < debounce) {
         // still pressed
       } else {
         mode = !(bool)mode;
         if (mode == MODE_STIMULATE) {
           reset_graph();
           analogWrite(D2, 1000); // start stimulator
         } else {
           analogWrite(D2, 0); // stop stimulator
         }
         debounce = millis() + 500;
       }
     }
     delay(remainingTimeBudget);
   }
}
