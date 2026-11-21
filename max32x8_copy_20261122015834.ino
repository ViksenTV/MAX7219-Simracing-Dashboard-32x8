
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <string.h>
#include <stdlib.h>

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

#define DATA_PIN 3
#define CLK_PIN  5
#define CS_PIN   4

#define SERIAL_BAUD 115200
#define DATA_TIMEOUT_MS 1500

#define SHIFT_BLINK_MS 150
#define PIT_BLINK_MS 500

MD_MAX72XX mx(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// Крупные цифры 5x7
const uint8_t largeDigits[10][7] = {
  {14,17,19,21,25,17,14},
  {4,12,4,4,4,4,14},
  {14,17,1,2,4,8,31},
  {30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2},
  {31,16,16,30,1,1,30},
  {14,16,16,30,17,17,14},
  {31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14},
  {14,17,17,15,1,1,14}
};

// Маленькие цифры 3x5
const uint8_t smallDigits[10][5] = {
  {7,5,5,5,7},
  {2,6,2,2,7},
  {7,1,7,4,7},
  {7,1,7,1,7},
  {5,5,7,1,1},
  {7,4,7,1,7},
  {7,4,7,5,7},
  {7,1,2,2,2},
  {7,5,7,5,7},
  {7,5,7,1,7}
};

bool frame[8][32];
bool oldFrame[8][32];

char serialBuffer[96];
uint8_t serialIndex = 0;

int rpmValue = 0;
int speedValue = 0;

// Максимум для масштаба шкалы RPM
int maxRpmValue = 8000;

// Точные обороты, при которых мигает передача
int redlineRpmValue = 7000;

int sessionSeconds = 0;
char gearValue = 'N';

bool pitLimiter = false;
bool gameRunning = false;
bool dataValid = false;
bool clockValid = false;

uint8_t clockHour = 0;
uint8_t clockMinute = 0;

unsigned long lastDataMs = 0;
unsigned long lastClockTickMs = 0;
unsigned long lastShiftBlinkMs = 0;
unsigned long lastPitBlinkMs = 0;

bool shiftBlinkState = true;
bool shiftAlert = false;
bool pitLabelState = true;

// --------------------------------------------------
// Графика
// --------------------------------------------------

void setFramePixel(uint8_t x, uint8_t y, bool on) {
  if (x < 32 && y < 8) {
    frame[y][x] = on;
  }
}

void drawLargeDigit(uint8_t digit, uint8_t x) {
  if (digit > 9) return;

  for (uint8_t y = 0; y < 7; y++) {
    for (uint8_t dx = 0; dx < 5; dx++) {
      bool on = largeDigits[digit][y] & (1 << (4 - dx));
      setFramePixel(x + dx, y, on);
    }
  }
}

void drawLargeLetter(char c, uint8_t x) {
  uint8_t glyph[7] = {0,0,0,0,0,0,0};

  if (c == 'N') {
    const uint8_t n[7] = {17,25,25,21,19,19,17};
    memcpy(glyph, n, sizeof(glyph));
  } else if (c == 'R') {
    const uint8_t r[7] = {30,17,17,30,20,18,17};
    memcpy(glyph, r, sizeof(glyph));
  } else {
    return;
  }

  for (uint8_t y = 0; y < 7; y++) {
    for (uint8_t dx = 0; dx < 5; dx++) {
      bool on = glyph[y] & (1 << (4 - dx));
      setFramePixel(x + dx, y, on);
    }
  }
}

void drawSmallDigit(uint8_t digit, uint8_t x) {
  if (digit > 9) return;

  for (uint8_t y = 0; y < 5; y++) {
    for (uint8_t dx = 0; dx < 3; dx++) {
      bool on = smallDigits[digit][y] & (1 << (2 - dx));
      setFramePixel(x + dx, y + 1, on);
    }
  }
}

void drawSmallLetter(char c, uint8_t x) {
  uint8_t glyph[5] = {0,0,0,0,0};

  if (c == 'P') {
    const uint8_t p[5] = {6,5,6,4,4};
    memcpy(glyph, p, sizeof(glyph));
  } else if (c == 'I') {
    const uint8_t i[5] = {7,2,2,2,7};
    memcpy(glyph, i, sizeof(glyph));
  } else if (c == 'T') {
    const uint8_t t[5] = {7,2,2,2,2};
    memcpy(glyph, t, sizeof(glyph));
  } else {
    return;
  }

  for (uint8_t y = 0; y < 5; y++) {
    for (uint8_t dx = 0; dx < 3; dx++) {
      bool on = glyph[y] & (1 << (2 - dx));
      setFramePixel(x + dx, y + 1, on);
    }
  }
}

void drawGear() {
  if (gearValue >= '1' && gearValue <= '9') {
    drawLargeDigit(gearValue - '0', 1);
  } else if (gearValue == 'R') {
    drawLargeLetter('R', 1);
  } else {
    drawLargeLetter('N', 1);
  }
}

void drawPitLabel() {
  drawSmallLetter('P', 1);
  drawSmallLetter('I', 5);
  drawSmallLetter('T', 9);
}

// --------------------------------------------------
// Правая часть экрана
// --------------------------------------------------

void drawSpeed() {
  int s = constrain(speedValue, 0, 999);

  uint8_t hundreds = s / 100;
  uint8_t tens = (s / 10) % 10;
  uint8_t units = s % 10;

  if (hundreds > 0) drawSmallDigit(hundreds, 13);
  if (hundreds > 0 || tens > 0) drawSmallDigit(tens, 19);
  drawSmallDigit(units, 25);
}

// Время сессии MM:SS
void drawTimeDigits(int totalSeconds) {
  int total = constrain(totalSeconds, 0, 5999);

  uint8_t minutes = total / 60;
  uint8_t seconds = total % 60;

  drawSmallDigit(minutes / 10, 13);
  drawSmallDigit(minutes % 10, 17);

  setFramePixel(21, 2, true);
  setFramePixel(21, 4, true);

  drawSmallDigit(seconds / 10, 23);
  drawSmallDigit(seconds % 10, 27);
}

void drawSessionTime() {
  drawTimeDigits(sessionSeconds);
}

// Часы HH:MM
void drawClockTime() {
  if (!clockValid) return;

  drawSmallDigit(clockHour / 10, 12);
  drawSmallDigit(clockHour % 10, 16);

  setFramePixel(20, 2, true);
  setFramePixel(20, 4, true);

  drawSmallDigit(clockMinute / 10, 22);
  drawSmallDigit(clockMinute % 10, 26);
}

// --------------------------------------------------
// Шкала RPM
// --------------------------------------------------

void drawRPMBar() {
  int limit = maxRpmValue > 0 ? maxRpmValue : 8000;
  int rpm = constrain(rpmValue, 0, limit);

  uint8_t lit = (uint32_t)rpm * 32 / limit;

  for (uint8_t x = 0; x < 32; x++) {
    setFramePixel(x, 7, x < lit);
  }
}

// --------------------------------------------------
// Отрисовка
// --------------------------------------------------

void renderFrame() {
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 32; x++) {
      if (frame[y][x] != oldFrame[y][x]) {
        mx.setPoint(7 - y, x, frame[y][x]);
        oldFrame[y][x] = frame[y][x];
      }
    }
  }
}

void drawDashboard() {
  memset(frame, 0, sizeof(frame));

  // Слева: PIT ↔ передача
  if (pitLimiter) {
    if (pitLabelState) {
      drawPitLabel();
    } else if (!shiftAlert || shiftBlinkState) {
      drawGear();
    }
  } else {
    if (!shiftAlert || shiftBlinkState) {
      drawGear();
    }
  }

  // Справа: время сессии или скорость
  if (pitLimiter || speedValue == 0) {
    drawSessionTime();
  } else {
    drawSpeed();
  }

  drawRPMBar();
  renderFrame();
}

void drawClockDashboard() {
  memset(frame, 0, sizeof(frame));
  drawClockTime();
  renderFrame();
}

void clearDashboard() {
  memset(frame, 0, sizeof(frame));
  renderFrame();
}

// --------------------------------------------------
// Парсинг часов HH:MM
// --------------------------------------------------

bool parseClock(const char *s) {
  if (strlen(s) != 5) return false;

  if (s[0] < '0' || s[0] > '9') return false;
  if (s[1] < '0' || s[1] > '9') return false;
  if (s[2] != ':') return false;
  if (s[3] < '0' || s[3] > '9') return false;
  if (s[4] < '0' || s[4] > '9') return false;

  uint8_t h = (s[0] - '0') * 10 + (s[1] - '0');
  uint8_t m = (s[3] - '0') * 10 + (s[4] - '0');

  if (h > 23 || m > 59) return false;

  bool wasClockValid = clockValid;

  clockHour = h;
  clockMinute = m;
  clockValid = true;

  // Не допускаем скачка локальных часов при первом получении времени
  if (!wasClockValid) {
    lastClockTickMs = millis();
  }

  return true;
}

// --------------------------------------------------
// Формат строки:
// rpm;speed;gear;maxrpm;redlineRpm;sessionSeconds;
// pitLimiter;HH:MM;gameRunning
// Всего 9 полей
// --------------------------------------------------

bool parseLine(char *line) {
  char *fields[9];
  uint8_t count = 1;
  fields[0] = line;

  for (char *p = line; *p; p++) {
    if (*p == ';') {
      *p = '\0';

      if (count >= 9) return false;
      fields[count++] = p + 1;
    }
  }

  if (count != 9) return false;

  int newRpm = atoi(fields[0]);
  int newSpeed = atoi(fields[1]);
  char *g = fields[2];
  int newMaxRpm = atoi(fields[3]);
  int newRedlineRpm = atoi(fields[4]);
  int newSessionSeconds = atoi(fields[5]);
  int newPitLimiter = atoi(fields[6]);

  // GameRunning обычно передаётся как 0/1.
  // Дополнительно поддерживаем True/False.
  int newGameRunning = 0;

  if (strcmp(fields[8], "1") == 0 ||
      strcmp(fields[8], "True") == 0 ||
      strcmp(fields[8], "true") == 0 ||
      strcmp(fields[8], "TRUE") == 0) {
    newGameRunning = 1;
  }

  if (newRpm < 0 || newRpm > 100000) return false;
  if (newSpeed < 0 || newSpeed > 2000) return false;

  char newGear = 'N';

  if (strcmp(g, "R") == 0 ||
      strcmp(g, "r") == 0 ||
      strcmp(g, "-1") == 0) {
    newGear = 'R';
  } else if (strcmp(g, "N") == 0 ||
             strcmp(g, "n") == 0 ||
             strcmp(g, "0") == 0 ||
             g[0] == '\0') {
    newGear = 'N';
  } else if (g[0] >= '1' && g[0] <= '9') {
    newGear = g[0];
  } else {
    return false;
  }

  if (newSessionSeconds < 0) newSessionSeconds = 0;

  // Обновляем время, если SimHub его передал
  parseClock(fields[7]);

  gameRunning = (newGameRunning != 0);

  // Игра закрыта: гоночные данные не используем
  if (!gameRunning) {
    dataValid = false;
    pitLimiter = false;
    shiftAlert = false;
    shiftBlinkState = true;

    drawClockDashboard();
    lastDataMs = millis();
    return true;
  }

  rpmValue = newRpm;
  speedValue = newSpeed;
  gearValue = newGear;
  sessionSeconds = newSessionSeconds;

  // Включённый PIT-лимитер передаёт значение 2
  pitLimiter = (newPitLimiter > 0);

  // Максимум RPM используется только для нижней шкалы
  if (newMaxRpm >= 1000 && newMaxRpm <= 30000) {
    maxRpmValue = newMaxRpm;
  }

  // SpecificRedlineValue — точный порог мигания
  if (newRedlineRpm >= 1000 && newRedlineRpm <= 30000) {
    redlineRpmValue = newRedlineRpm;
  }

  bool wasShiftAlert = shiftAlert;

  // Никаких 90%: сравниваем с точным заданным порогом
  shiftAlert = (rpmValue >= redlineRpmValue);

  if (!shiftAlert) {
    shiftBlinkState = true;
  } else if (!wasShiftAlert) {
    shiftBlinkState = true;
    lastShiftBlinkMs = millis();
  }

  lastDataMs = millis();
  dataValid = true;

  drawDashboard();
  return true;
}

// --------------------------------------------------
// Настройка
// --------------------------------------------------

void setup() {
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 1);
  mx.clear();

  memset(frame, 0, sizeof(frame));
  memset(oldFrame, 0, sizeof(oldFrame));

  Serial.begin(SERIAL_BAUD);
}

// --------------------------------------------------
// Главный цикл
// --------------------------------------------------

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      serialBuffer[serialIndex] = '\0';

      if (serialIndex > 0) {
        parseLine(serialBuffer);
      }

      serialIndex = 0;
    } else if (serialIndex < sizeof(serialBuffer) - 1) {
      serialBuffer[serialIndex++] = c;
    } else {
      serialIndex = 0;
    }
  }

  unsigned long now = millis();

  // Если игра закрыта, показываем часы
  if (!gameRunning) {
    if (dataValid) {
      dataValid = false;
      shiftAlert = false;
      pitLimiter = false;
      shiftBlinkState = true;
      lastClockTickMs = now;
    }

    if (clockValid) {
      drawClockDashboard();
    } else {
      clearDashboard();
    }
  } else {
    // Мигание передачи на точном пороге SpecificRedlineValue
    if (dataValid && shiftAlert &&
        now - lastShiftBlinkMs >= SHIFT_BLINK_MS) {
      lastShiftBlinkMs = now;
      shiftBlinkState = !shiftBlinkState;
      drawDashboard();
    }

    // Чередование PIT и передачи каждые 500 мс
    if (dataValid && pitLimiter &&
        now - lastPitBlinkMs >= PIT_BLINK_MS) {
      lastPitBlinkMs = now;
      pitLabelState = !pitLabelState;
      drawDashboard();
    }

    // Потеря телеметрии более чем на 1,5 секунды
    if (dataValid && now - lastDataMs > DATA_TIMEOUT_MS) {
      dataValid = false;
      shiftAlert = false;
      pitLimiter = false;
      shiftBlinkState = true;
      lastClockTickMs = now;

      if (clockValid) {
        drawClockDashboard();
      } else {
        clearDashboard();
      }
    }
  }

  // Локальные часы идут и при закрытой игре,
  // и при прекращении передачи телеметрии
  if (!dataValid && clockValid &&
      now - lastClockTickMs >= 60000UL) {
    unsigned long elapsedMinutes =
        (now - lastClockTickMs) / 60000UL;

    lastClockTickMs += elapsedMinutes * 60000UL;

    while (elapsedMinutes > 0) {
      clockMinute++;

      if (clockMinute >= 60) {
        clockMinute = 0;
        clockHour++;

        if (clockHour >= 24) {
          clockHour = 0;
        }
      }

      elapsedMinutes--;
    }

    drawClockDashboard();
  }
}
