#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// ============================================================
// SETTINGS
// ============================================================

const bool COMMON_ANODE = false;
const uint8_t BRIGHTNESS = 180;

const uint8_t LED_PINS[4][3] = {
  {25, 26, 27},
  {14, 13, 32},
  {33, 18, 19},
  {21, 22, 23}
};

const uint8_t BUTTON_PINS[3] = {4, 16, 17};

const uint8_t CHANNEL_GAIN[4][3] = {
  {255, 255, 255},
  {255, 255, 255},
  {255, 255, 255},
  {255, 255, 255}
};

// Wind-up light: keep your working connections.
const uint8_t WIND_LED_PIN = 12;
const uint8_t WIND_ENCODER_A = 35;
const uint8_t WIND_ENCODER_B = 39;
const int WIND_DIRECTION = 1;  // Set to -1 to reverse
const int WIND_TRANSITIONS_PER_STEP = 4;
const int WIND_BOOST = 20;
const uint32_t WIND_FADE_WAIT_MS = 600;
const uint32_t WIND_FADE_INTERVAL_MS = 25;

// Catch the stars.
const uint8_t OLED_SDA = 5;
const uint8_t OLED_SCL = 15;
const uint8_t POT_PIN = 34;

#define OLED_IS_SH1106 1

#if OLED_IS_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#endif

const bool POT_REVERSED = false;
const int POT_MIN = 0;
const int POT_MAX = 4095;
const int BASKET_WIDTH = 28;
const int BASKET_Y = 55;
const float STAR_SPEED = 10.0f;  // Pixels per second
const uint32_t SCREEN_INTERVAL_MS = 50;

// ============================================================
// WIND-UP LIGHT
// ============================================================

portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint8_t encoderPreviousAB = 0;
volatile int encoderPartial = 0;
volatile int encoderPendingSteps = 0;

// Stored in RAM for use inside the interrupt handler.
DRAM_ATTR int8_t encoderTransitions[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

int windBrightness = 0;
int windPreviousBrightness = -1;
uint32_t windLastTurn = 0;
uint32_t windLastFade = 0;

void ARDUINO_ISR_ATTR encoderISR() {
  portENTER_CRITICAL_ISR(&encoderMux);

  uint8_t current =
      (digitalRead(WIND_ENCODER_A) << 1) |
       digitalRead(WIND_ENCODER_B);

  if ((current ^ encoderPreviousAB) == 3) {
    encoderPartial = 0;
  } else {
    encoderPartial += WIND_DIRECTION *
        encoderTransitions[(encoderPreviousAB << 2) | current];

    if (encoderPartial >= WIND_TRANSITIONS_PER_STEP) {
      encoderPendingSteps++;
      encoderPartial = 0;
    } else if (encoderPartial <= -WIND_TRANSITIONS_PER_STEP) {
      encoderPendingSteps--;
      encoderPartial = 0;
    }
  }

  encoderPreviousAB = current;
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void setupWindLight() {
  // External pull-ups are required on these input-only pins.
  pinMode(WIND_ENCODER_A, INPUT);
  pinMode(WIND_ENCODER_B, INPUT);

  digitalWrite(WIND_LED_PIN, LOW);
  pinMode(WIND_LED_PIN, OUTPUT);
  analogWrite(WIND_LED_PIN, 0);

  encoderPreviousAB =
      (digitalRead(WIND_ENCODER_A) << 1) |
       digitalRead(WIND_ENCODER_B);

  attachInterrupt(WIND_ENCODER_A, encoderISR, CHANGE);
  attachInterrupt(WIND_ENCODER_B, encoderISR, CHANGE);

  windLastTurn = millis();
  windLastFade = windLastTurn;
}

void updateWindLight() {
  uint32_t now = millis();

  portENTER_CRITICAL(&encoderMux);
  int steps = encoderPendingSteps;
  encoderPendingSteps = 0;
  portEXIT_CRITICAL(&encoderMux);

  if (steps != 0) {
    windBrightness =
        constrain(windBrightness + steps * WIND_BOOST, 0, 255);
    windLastTurn = now;
    windLastFade = now;
  }

  if (now - windLastTurn < WIND_FADE_WAIT_MS) {
    windLastFade = now;
  } else if (now - windLastFade >= WIND_FADE_INTERVAL_MS) {
    uint32_t fadeSteps =
        (now - windLastFade) / WIND_FADE_INTERVAL_MS;

    windLastFade += fadeSteps * WIND_FADE_INTERVAL_MS;
    windBrightness = fadeSteps >= (uint32_t)windBrightness
        ? 0 : windBrightness - (int)fadeSteps;
  }

  if (windBrightness != windPreviousBrightness) {
    analogWrite(WIND_LED_PIN, windBrightness);
    windPreviousBrightness = windBrightness;
  }
}

// ============================================================
// COLOR MATCHING
// ============================================================

struct Color {
  uint8_t r, g, b;
  const char* name;
};

const Color PALETTE[] = {
  {255,  45,   0, "Tangerine"},
  {255, 150,   0, "Golden yellow"},
  {100, 255,   0, "Lime"},
  {  0, 255,  35, "Emerald"},
  {  0, 210, 180, "Turquoise"},
  {  0,  45, 255, "Ocean blue"},
  {130,   0, 255, "Violet"},
  {255,   0,  75, "Hot pink"}
};

const uint8_t COLOR_COUNT = sizeof(PALETTE) / sizeof(PALETTE[0]);

uint8_t answerColors[3];
uint8_t correctButton = 0;
int previousTarget = -1;
unsigned long matches = 0;

const uint32_t DEBOUNCE_MS = 35;
uint8_t previousMask = 0;
uint32_t maskChangedAt = 0;
bool readyForPress = false;

enum ColorState {
  COLOR_FADE,
  COLOR_PLAY,
  COLOR_CELEBRATE_WAIT,
  COLOR_CELEBRATE_ON,
  COLOR_CELEBRATE_GAP,
  COLOR_CELEBRATE_END
};

ColorState colorState = COLOR_FADE;
uint32_t colorStateAt = 0;
uint8_t celebrationLED = 0;
int previousFadeLevel = -1;

void writeColor(uint8_t led, uint8_t index, uint8_t level) {
  const Color& c = PALETTE[index];
  const uint8_t values[3] = {c.r, c.g, c.b};

  for (uint8_t channel = 0; channel < 3; channel++) {
    uint32_t value = values[channel];
    value = value * CHANNEL_GAIN[led][channel] / 255;
    value = value * BRIGHTNESS / 255;
    value = value * level / 255;

    if (COMMON_ANODE) value = 255 - value;
    analogWrite(LED_PINS[led][channel], value);
  }
}

void showRound(uint8_t level) {
  writeColor(0, answerColors[correctButton], level);
  for (uint8_t i = 0; i < 3; i++) {
    writeColor(i + 1, answerColors[i], level);
  }
}

uint8_t readButtonMask() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (digitalRead(BUTTON_PINS[i]) == LOW) mask |= (1 << i);
  }
  return mask;
}

void resetButtonDetection() {
  readyForPress = false;
  previousMask = readButtonMask();
  maskChangedAt = millis();
}

void newRound() {
  uint8_t base = random(COLOR_COUNT);
  answerColors[0] = base;
  answerColors[1] = (base + 3) % COLOR_COUNT;
  answerColors[2] = (base + 5) % COLOR_COUNT;

  for (int i = 2; i > 0; i--) {
    int j = random(i + 1);
    uint8_t temp = answerColors[i];
    answerColors[i] = answerColors[j];
    answerColors[j] = temp;
  }

  do {
    correctButton = random(3);
  } while (answerColors[correctButton] == previousTarget);

  previousTarget = answerColors[correctButton];

  Serial.printf("\nTarget: %s\n",
                PALETTE[answerColors[correctButton]].name);
  for (uint8_t i = 0; i < 3; i++) {
    Serial.printf("Button %u: %s\n", i + 1,
                  PALETTE[answerColors[i]].name);
  }

  colorState = COLOR_FADE;
  colorStateAt = millis();
  previousFadeLevel = -1;
  showRound(0);
  resetButtonDetection();
}

void updateColorGame() {
  uint32_t now = millis();
  uint32_t elapsed = now - colorStateAt;
  uint8_t matched = answerColors[correctButton];

  switch (colorState) {
    case COLOR_FADE: {
      float p = min(1.0f, elapsed / 320.0f);
      int level = int(255 * p * p * (3.0f - 2.0f * p));

      if (level != previousFadeLevel) {
        showRound(level);
        previousFadeLevel = level;
      }

      if (elapsed >= 320) {
        colorState = COLOR_PLAY;
        resetButtonDetection();
      }
      return;
    }

    case COLOR_CELEBRATE_WAIT:
      if (elapsed >= 180) {
        celebrationLED = 0;
        writeColor(celebrationLED, matched, 255);
        colorState = COLOR_CELEBRATE_ON;
        colorStateAt = now;
      }
      return;

    case COLOR_CELEBRATE_ON:
      if (elapsed >= 220) {
        writeColor(celebrationLED, matched, 0);
        colorState = COLOR_CELEBRATE_GAP;
        colorStateAt = now;
      }
      return;

    case COLOR_CELEBRATE_GAP:
      if (elapsed >= 70) {
        celebrationLED++;
        if (celebrationLED < 4) {
          writeColor(celebrationLED, matched, 255);
          colorState = COLOR_CELEBRATE_ON;
        } else {
          colorState = COLOR_CELEBRATE_END;
        }
        colorStateAt = now;
      }
      return;

    case COLOR_CELEBRATE_END:
      if (elapsed >= 150) newRound();
      return;

    case COLOR_PLAY:
      break;
  }

  uint8_t mask = readButtonMask();
  if (mask != previousMask) {
    previousMask = mask;
    maskChangedAt = now;
  }

  if (now - maskChangedAt < DEBOUNCE_MS) return;

  if (mask == 0) {
    readyForPress = true;
    return;
  }

  if (!readyForPress) return;
  readyForPress = false;

  if ((mask & (mask - 1)) != 0) {
    Serial.println("Press one button at a time.");
    return;
  }

  uint8_t pressed = mask == 1 ? 0 : mask == 2 ? 1 : 2;

  if (pressed == correctButton) {
    matches++;
    Serial.printf("MATCH! Total: %lu\n", matches);
    showRound(0);
    colorState = COLOR_CELEBRATE_WAIT;
    colorStateAt = now;
  } else {
    Serial.println("Try again.");
  }
}

// ============================================================
// CATCH THE STARS
// ============================================================

bool oledReady = false;
float filteredPot = 0;
int basketX = 50;
int starX = 64;
float starY = 18;
unsigned long starsCaught = 0;

uint32_t lastPotAt = 0;
uint32_t lastScreenAt = 0;
uint32_t lastPhysicsAt = 0;
uint32_t sparkleAt = 0;
bool sparkling = false;
int sparkleX = 64;

void spawnStar() {
  starX = random(5, 123);
  starY = 18;
}

void drawStar(int x, int y) {
  oled.drawLine(x - 3, y, x + 3, y);
  oled.drawLine(x, y - 3, x, y + 3);
  oled.drawLine(x - 2, y - 2, x + 2, y + 2);
  oled.drawLine(x - 2, y + 2, x + 2, y - 2);
}

void setupStars() {
  pinMode(POT_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(POT_PIN, ADC_11db);
  filteredPot = analogRead(POT_PIN);

  // Do not use default I2C pins 21/22: your RGB LEDs use them.
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setTimeOut(25);

  uint8_t address = 0;
  for (uint8_t candidate = 0x3C; candidate <= 0x3D; candidate++) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) {
      address = candidate;
      break;
    }
  }

  if (address == 0) {
    Serial.println("OLED not found at 0x3C or 0x3D.");
    Serial.println("Other two games will still run.");
    return;
  }

  // U8g2 takes the address shifted left by one bit.
  oled.setI2CAddress(address << 1);
  oled.setBusClock(400000);
  oled.begin();
  Wire.setTimeOut(25);
  oled.setContrast(100);
  oledReady = true;

  Serial.printf("OLED detected at 0x%02X\n", address);

  spawnStar();
  lastPhysicsAt = millis();
  lastScreenAt = millis();
}

void updateStars() {
  if (!oledReady) return;

  uint32_t now = millis();

  // Smooth the pot reading to reduce basket jitter.
  if (now - lastPotAt >= 5) {
    lastPotAt = now;
    filteredPot += 0.18f * (analogRead(POT_PIN) - filteredPot);

    int reading = constrain((int)filteredPot, POT_MIN, POT_MAX);
    int x = map(reading, POT_MIN, POT_MAX, 0, 128 - BASKET_WIDTH);
    basketX = POT_REVERSED ? 128 - BASKET_WIDTH - x : x;
  }

  if (now - lastScreenAt < SCREEN_INTERVAL_MS) return;
  lastScreenAt = now;

  float dt = min((now - lastPhysicsAt) / 1000.0f, 0.15f);
  lastPhysicsAt = now;

  float oldY = starY;
  starY += STAR_SPEED * dt;

  // Test only when the star crosses the basket's top edge.
  if (oldY + 3 < BASKET_Y && starY + 3 >= BASKET_Y) {
    if (starX >= basketX - 2 &&
        starX <= basketX + BASKET_WIDTH + 2) {
      starsCaught++;
      sparkleAt = now;
      sparkling = true;
      sparkleX = basketX + BASKET_WIDTH / 2;
      spawnStar();
    }
  }

  // Missed stars simply restart. No penalties.
  if (starY > 68) spawnStar();
  if (sparkling && now - sparkleAt >= 450) sparkling = false;

  oled.clearBuffer();
  oled.setFont(u8g2_font_5x7_tf);

  char heading[28];
  snprintf(heading, sizeof(heading), "STARS: %lu", starsCaught);
  oled.drawStr(2, 8, heading);
  oled.drawHLine(0, 11, 128);

  if (starY < 67) drawStar(starX, (int)starY);

  // Open-topped basket.
  oled.drawLine(basketX, BASKET_Y,
                basketX + 3, 62);
  oled.drawLine(basketX + 3, 62,
                basketX + BASKET_WIDTH - 4, 62);
  oled.drawLine(basketX + BASKET_WIDTH - 4, 62,
                basketX + BASKET_WIDTH - 1, BASKET_Y);

  if (sparkling) {
    int radius = 4 + (now - sparkleAt) / 45;
    oled.drawCircle(sparkleX, 48, radius);
    oled.drawStr(49, 29, "CAUGHT!");
  }

  oled.sendBuffer();
}

// ============================================================
// SETUP AND LOOP
// ============================================================

void setup() {
  Serial.begin(115200);

  for (uint8_t led = 0; led < 4; led++) {
    for (uint8_t channel = 0; channel < 3; channel++) {
      uint8_t pin = LED_PINS[led][channel];
      digitalWrite(pin, COMMON_ANODE ? HIGH : LOW);
      pinMode(pin, OUTPUT);
      analogWrite(pin, COMMON_ANODE ? 255 : 0);
    }
  }

  for (uint8_t i = 0; i < 3; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
  }

  randomSeed(micros());
  setupWindLight();
  setupStars();
  newRound();

  Serial.println("Activity board ready.");
}

void loop() {
  updateWindLight();
  updateColorGame();
  updateStars();
}