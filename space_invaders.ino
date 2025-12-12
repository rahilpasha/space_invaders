#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <NimBLEDevice.h>
#include <Preferences.h> // NEW: Include Preferences library for NVS storage

// LCD Pins
const int LCD_SDA = 35;
const int LCD_SCK = 36;
const int LCD_CS = 40;
const int LCD_DC = 41;
const int LCD_RESET = 42;

// Input Pins
const int startPin = 7;
const int knobA = 4;
const int knobB = 5;
const int knobPin = 15;

// Create display object
Adafruit_ST7735 tft = Adafruit_ST7735(LCD_CS, LCD_DC, LCD_RESET);

// Game constants
#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 128
#define PLAYER_WIDTH 11
#define PLAYER_HEIGHT 8
#define ALIEN_WIDTH 11
#define ALIEN_HEIGHT 8
#define ALIEN_ROWS 3  // Back to 3 rows
#define ALIEN_COLS 8
#define ALIEN_ROWS_COOP 5  // More aliens for co-op
#define ALIEN_COLS_COOP 10
#define BULLET_SIZE 2

// Sprite data (1 = pixel on, 0 = pixel off)
// Classic Space Invaders alien sprite 1 (11x8) - Top row
const uint16_t alienSprite1[8] = {
  0b00100000100,
  0b00010001000,
  0b00111111100,
  0b01101110110,
  0b11111111111,
  0b10111111101,
  0b10100000101,
  0b00011011000
};

// Alien sprite 2 (11x8) - Second row
const uint16_t alienSprite2[8] = {
  0b00001110000,
  0b00111111100,
  0b01111111110,
  0b11101110111,
  0b11111111111,
  0b01111111110,
  0b00100000100,
  0b01000000010
};

// Alien sprite 3 (11x8) - Third row
const uint16_t alienSprite3[8] = {
  0b00011111000,
  0b01111111110,
  0b11111111111,
  0b11101110111,
  0b11111111111,
  0b00111111100,
  0b00100000100,
  0b01000000010
};

// Alien sprite 4 (11x8) - Fourth row
const uint16_t alienSprite4[8] = {
  0b00000100000,
  0b00001110000,
  0b00011111000,
  0b01111111110,
  0b11111111111,
  0b01111111110,
  0b00100000100,
  0b01000000010
};

// Player ship sprite (11x8)
const uint16_t playerSprite[8] = {
  0b00000100000,
  0b00001110000,
  0b00001110000,
  0b01111111110,
  0b11111111111,
  0b11111111111,
  0b11111111111,
  0b00000000000
};

// Game modes
enum GameMode {
  MENU,
  SINGLE_PLAYER,
  TWO_PLAYER
};

GameMode currentMode = MENU;
int menuSelection = 0;

// Player 1 (Rotary encoder)
int player1X = SCREEN_WIDTH / 4;
int player1Y = SCREEN_HEIGHT - 15;
volatile int encoderPos = 25;
volatile int lastEncoded = 0;
const int MIN_POS = 0;
const int MAX_POS = 50;
int player1Score = 0;
int player1Lives = 3;

// Player 2 (Xbox controller)
int player2X = 3 * SCREEN_WIDTH / 4;
int player2Y = SCREEN_HEIGHT - 15;
int player2Score = 0;
int player2Lives = 3;

// Bullets
struct Bullet {
  int x, y;
  bool active;
  int owner; // 1 or 2
};
Bullet player1Bullet = {0, 0, false, 1};
Bullet player2Bullet = {0, 0, false, 2};
Bullet alienBullets[3];

// Aliens
struct Alien {
  int x, y;
  bool alive;
  int lives;  // For super aliens
  int type;   // 0-3 for different sprite types
};
Alien aliens[ALIEN_ROWS_COOP][ALIEN_COLS_COOP];  // Use max size for both modes
int alienDirection = 1;
unsigned long lastAlienMove = 0;
int alienMoveDelay = 500;
int activeAlienRows = ALIEN_ROWS;  // Track how many rows are active
int activeAlienCols = ALIEN_COLS;
bool aliensShouldDescend = false;  // Track if aliens should move down
int alienBounces = 0;  // Count how many times aliens bounced

// Defensive fortifications
struct Fort {
  int x, y;
  bool blocks[8][12];  // 12x8 pixel fort
  bool alive;
};
Fort forts[4];  // 4 bunkers now

// Super alien
struct SuperAlien {
  int x, y;
  bool active;
  int lives;
};
// Super Alien now has 1 life
SuperAlien superAlien = {0, 0, false, 1}; 
unsigned long lastSuperSpawn = 0;
unsigned long lastSuperMove = 0;

// Rapid fire powerup
bool rapidFireActive = false;
unsigned long rapidFireEnd = 0;

// Explosion effect
struct Explosion {
  int x, y;
  bool active;
  unsigned long startTime;
};
Explosion explosion = {0, 0, false, 0};

// Game state
bool gameOver = false;
volatile bool firePressed = false;
volatile bool menuPressed = false;
unsigned long lastFireTime = 0;
const int FIRE_COOLDOWN = 300;  // 300ms cooldown between shots

// Xbox Controller BLE
NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pInputCharacteristic = nullptr;
bool controllerConnected = false;
int16_t xboxLeftStickX = 0;
bool xboxAButton = false;
bool xboxAButtonPrev = false;

// --- High Score and Preferences (NVS) ---
Preferences preferences;
int currentHighScore = 0;
const char* HIGH_SCORE_KEY = "high_score";
const char* NVS_NAMESPACE = "invaders_nvs";
// ------------------------------------------

// BLE Callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) {
    controllerConnected = true;
    Serial.println("Xbox Controller Connected!");
  }

  void onDisconnect(NimBLEClient* pclient) {
    controllerConnected = false;
    Serial.println("Xbox Controller Disconnected");
  }
};

static void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  // Parse Xbox controller input report
  if (length >= 8) {
    // Left stick X axis (bytes 2-3, signed 16-bit)
    xboxLeftStickX = (int16_t)((pData[3] << 8) | pData[2]);
    
    // A button (byte 6, bit 0)
    xboxAButton = (pData[6] & 0x01) != 0;
  }
}

void setupBLE() {
  Serial.println("Initializing BLE...");
  NimBLEDevice::init("ESP32_SpaceInvaders");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  
  // Start scanning for Xbox controller
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  Serial.println("Scanning for Xbox controller...");
}

bool connectToXboxController() {
  Serial.println("Starting BLE scan...");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  
  Serial.println("Scanning for 15 seconds...");
  pScan->start(15, false);  // Scan for 15 seconds
  
  NimBLEScanResults scanResults = pScan->getResults();
  Serial.print("Found ");
  Serial.print(scanResults.getCount());
  Serial.println(" devices");
  
  for (int i = 0; i < scanResults.getCount(); i++) {
    const NimBLEAdvertisedDevice* device = scanResults.getDevice(i);
    
    std::string deviceName = device->getName();
    Serial.print("Device ");
    Serial.print(i);
    Serial.print(": ");
    Serial.println(deviceName.c_str());
    
    // Look for Xbox controller (more flexible matching)
    if (deviceName.find("Xbox") != std::string::npos ||
        deviceName.find("Controller") != std::string::npos ||
        deviceName.find("Wireless") != std::string::npos ||
        device->haveServiceUUID() && device->getServiceUUID().equals(NimBLEUUID((uint16_t)0x1812))) {
      
      Serial.print("Found potential controller: ");
      Serial.println(deviceName.c_str());
      
      pClient = NimBLEDevice::createClient();
      pClient->setClientCallbacks(new ClientCallbacks());
      
      Serial.println("Attempting to connect...");
      if (pClient->connect(device)) {
        Serial.println("Connected to controller!");
        
        // Get HID service (0x1812)
        NimBLERemoteService* pService = pClient->getService(NimBLEUUID((uint16_t)0x1812));
        if (pService) {
          Serial.println("Found HID service");
          // Get Report characteristic (0x2A4D)
          pInputCharacteristic = pService->getCharacteristic(NimBLEUUID((uint16_t)0x2A4D));
          if (pInputCharacteristic && pInputCharacteristic->canNotify()) {
            pInputCharacteristic->subscribe(true, notifyCallback);
            Serial.println("Subscribed to notifications");
            return true;
          } else {
            Serial.println("Could not subscribe to characteristic");
          }
        } else {
          Serial.println("HID service not found");
        }
      } else {
        Serial.println("Connection failed");
      }
    }
  }
  Serial.println("No controller found");
  return false;
}

// Interrupt handlers
void IRAM_ATTR startButtonISR() {
  static unsigned long lastPress = 0;
  unsigned long now = millis();
  if (now - lastPress > 200) {
    menuPressed = true;
    lastPress = now;
  }
}

void IRAM_ATTR fireButtonISR() {
  static unsigned long lastPress = 0;
  unsigned long now = millis();
  if (now - lastPress > 200) {
    firePressed = true;
    lastPress = now;
  }
}

void IRAM_ATTR updateEncoder() {
  int MSB = digitalRead(knobA);
  int LSB = digitalRead(knobB);
  
  int encoded = (MSB << 1) | LSB;
  int sum = (lastEncoded << 2) | encoded;
  
  if(sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) {
    encoderPos++;
    if (encoderPos > MAX_POS) encoderPos = MAX_POS;
  }
  else if(sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) {
    encoderPos--;
    if (encoderPos < MIN_POS) encoderPos = MIN_POS;
  }
  
  lastEncoded = encoded;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize Preferences (NEW)
  preferences.begin(NVS_NAMESPACE, false);
  currentHighScore = preferences.getInt(HIGH_SCORE_KEY, 0); // Load high score, default 0
  
  // Initialize SPI
  SPI.begin(LCD_SCK, -1, LCD_SDA, LCD_CS);
  
  // Initialize display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  
  // Setup inputs
  pinMode(startPin, INPUT_PULLDOWN);
  pinMode(knobPin, INPUT_PULLDOWN);
  pinMode(knobA, INPUT_PULLUP);
  pinMode(knobB, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(startPin), startButtonISR, RISING);
  attachInterrupt(digitalPinToInterrupt(knobPin), fireButtonISR, RISING);
  attachInterrupt(digitalPinToInterrupt(knobA), updateEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(knobB), updateEncoder, CHANGE);
  
  // Setup BLE
  setupBLE();
  
  drawMenu();
  
  Serial.println("Space Invaders Ready!");
}

void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(15, 20);
  tft.println("SPACE");
  tft.setCursor(5, 40);
  tft.println("INVADERS");
  
  // Display High Score (Updated text)
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_RED);
  tft.setCursor(30, 65);
  tft.print("HIGH-SCORE: "); 
  tft.println(currentHighScore);
  
  tft.setTextColor(ST77XX_WHITE);
  
  // Menu options (Adjusted Y positions)
  tft.setCursor(20, 85);
  if (menuSelection == 0) tft.print(">");
  tft.println(" 1 Player vs AI");
  
  tft.setCursor(20, 100);
  if (menuSelection == 1) tft.print(">");
  tft.println(" 2 Player Co-op");
  
  tft.setCursor(10, 115);
  tft.setTextColor(ST77XX_CYAN);
  tft.println("Turn & press start");
}

void initSinglePlayer() {
  activeAlienRows = ALIEN_ROWS;
  activeAlienCols = ALIEN_COLS;
  
  // Initialize aliens with more spacing and different types per row
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      aliens[row][col].x = 10 + col * 18;
      aliens[row][col].y = 25 + row * 14;
      aliens[row][col].alive = true;
      aliens[row][col].lives = 1;
      aliens[row][col].type = row;  // Each row gets a different type (0-2)
    }
  }
  
  for (int i = 0; i < 3; i++) {
    alienBullets[i].active = false;
  }
  
  alienDirection = 1;
  alienMoveDelay = 400;
  alienBounces = 0;
  aliensShouldDescend = false;
  player1Bullet.active = false;
  player1X = SCREEN_WIDTH / 2;
  player1Score = 0;
  player1Lives = 3;
  gameOver = false;
  superAlien.active = false;
  superAlien.lives = 1; // UPDATED: 1 life for Super Alien
  rapidFireActive = false;
  lastSuperSpawn = millis();
  
  // Initialize forts only on first game start
  initForts();
}

void resetAliensOnly() {
  // Reset only aliens, keep everything else (forts, lives, etc.)
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      aliens[row][col].x = 10 + col * 18;
      aliens[row][col].y = 25 + row * 14;
      aliens[row][col].alive = true;
      aliens[row][col].lives = 1;
      aliens[row][col].type = row;
    }
  }
  
  alienDirection = 1;
  alienBounces = 0;
  aliensShouldDescend = false;
  superAlien.active = false;
  superAlien.lives = 1; // UPDATED: 1 life for Super Alien
  rapidFireActive = false;
  lastSuperSpawn = millis();
}

void initTwoPlayer() {
  activeAlienRows = ALIEN_ROWS_COOP;
  activeAlienCols = ALIEN_COLS_COOP;
  
  // Initialize MORE aliens for co-op challenge
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      aliens[row][col].x = 5 + col * 14;
      aliens[row][col].y = 12 + row * 10;
      aliens[row][col].alive = true;
      aliens[row][col].lives = 1;
      aliens[row][col].type = row % 4;  // Cycle through 4 types
    }
  }
  
  for (int i = 0; i < 3; i++) {
    alienBullets[i].active = false;
  }
  
  alienDirection = 1;
  alienMoveDelay = 400;
  alienBounces = 0;
  aliensShouldDescend = false;
  
  player1X = SCREEN_WIDTH / 4;
  player1Y = SCREEN_HEIGHT - 15;
  player2X = 3 * SCREEN_WIDTH / 4;
  player2Y = SCREEN_HEIGHT - 15;
  
  player1Bullet.active = false;
  player2Bullet.active = false;
  
  player1Score = 0;
  player2Score = 0;
  player1Lives = 3;
  player2Lives = 3;
  gameOver = false;
  superAlien.active = false;
  superAlien.lives = 1; // Consistency update
  rapidFireActive = false;
  
  // Initialize forts
  initForts();
}

void drawPlayer(int x, int y, uint16_t color) {
  for (int row = 0; row < PLAYER_HEIGHT; row++) {
    for (int col = 0; col < PLAYER_WIDTH; col++) {
      if (playerSprite[row] & (1 << (PLAYER_WIDTH - 1 - col))) {
        tft.drawPixel(x + col, y + row, color);
      }
    }
  }
}

void erasePlayer(int x, int y) {
  tft.fillRect(x, y, PLAYER_WIDTH, PLAYER_HEIGHT, ST77XX_BLACK);
}

void drawAlien(int x, int y) {
  for (int row = 0; row < ALIEN_HEIGHT; row++) {
    for (int col = 0; col < ALIEN_WIDTH; col++) {
      if (alienSprite1[row] & (1 << (ALIEN_WIDTH - 1 - col))) {
        tft.drawPixel(x + col, y + row, ST77XX_WHITE);
      }
    }
  }
}

void drawAlienType(int x, int y, int type) {
  const uint16_t* sprite;
  switch(type) {
    case 0: sprite = alienSprite1; break;
    case 1: sprite = alienSprite2; break;
    case 2: sprite = alienSprite3; break;
    case 3: sprite = alienSprite4; break;
    default: sprite = alienSprite1; break;
  }
  
  for (int row = 0; row < ALIEN_HEIGHT; row++) {
    for (int col = 0; col < ALIEN_WIDTH; col++) {
      if (sprite[row] & (1 << (ALIEN_WIDTH - 1 - col))) {
        tft.drawPixel(x + col, y + row, ST77XX_WHITE);
      }
    }
  }
}

void drawSuperAlien(int x, int y) {
  // Draw same size as regular alien but in red
  for (int row = 0; row < ALIEN_HEIGHT; row++) {
    for (int col = 0; col < ALIEN_WIDTH; col++) {
      if (alienSprite1[row] & (1 << (ALIEN_WIDTH - 1 - col))) {
        tft.drawPixel(x + col, y + row, ST77XX_RED);
      }
    }
  }
}

void eraseSuperAlien(int x, int y) {
  // Erase same size as regular alien
  tft.fillRect(x, y, ALIEN_WIDTH, ALIEN_HEIGHT, ST77XX_BLACK);
}

void initForts() {
  // Fort pattern (classic space invaders style)
  bool fortPattern[8][12] = {
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,0,0,0,0,1,1,1,1},
    {1,1,1,1,0,0,0,0,1,1,1,1},
    {1,1,1,0,0,0,0,0,0,1,1,1},
    {1,1,1,0,0,0,0,0,0,1,1,1}
  };
  
  // Initialize 4 forts
  int fortY = SCREEN_HEIGHT - 35;
  int spacing = SCREEN_WIDTH / 5;  // Divide screen into 5 sections for 4 forts
  
  for (int f = 0; f < 4; f++) {
    forts[f].x = spacing * (f + 1) - 6;  // Center each fort in its section
    forts[f].y = fortY;
    forts[f].alive = true;
    
    // Copy pattern
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 12; col++) {
        forts[f].blocks[row][col] = fortPattern[row][col];
      }
    }
  }
}

void drawFort(int fortIndex) {
  if (!forts[fortIndex].alive) return;
  
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 12; col++) {
      if (forts[fortIndex].blocks[row][col]) {
        tft.drawPixel(forts[fortIndex].x + col, forts[fortIndex].y + row, ST77XX_GREEN);
      }
    }
  }
}

void drawAllForts() {
  for (int i = 0; i < 4; i++) {
    drawFort(i);
  }
}

bool checkFortCollision(int bulletX, int bulletY, bool isPlayerBullet) {
  for (int f = 0; f < 4; f++) {
    if (!forts[f].alive) continue;
    
    // Check if bullet is within fort bounds
    if (bulletX >= forts[f].x && bulletX < forts[f].x + 12 &&
        bulletY >= forts[f].y && bulletY < forts[f].y + 8) {
      
      int col = bulletX - forts[f].x;
      int row = bulletY - forts[f].y;
      
      if (forts[f].blocks[row][col]) {
        // Destroy 3x3 area around impact
        for (int dr = -1; dr <= 1; dr++) {
          for (int dc = -1; dc <= 1; dc++) {
            int r = row + dr;
            int c = col + dc;
            if (r >= 0 && r < 8 && c >= 0 && c < 12) {
              if (forts[f].blocks[r][c]) {
                forts[f].blocks[r][c] = false;
                tft.drawPixel(forts[f].x + c, forts[f].y + r, ST77XX_BLACK);
              }
            }
          }
        }
        
        // Check if entire fort is destroyed
        bool anyBlocksLeft = false;
        for (int r = 0; r < 8; r++) {
          for (int c = 0; c < 12; c++) {
            if (forts[f].blocks[r][c]) {
              anyBlocksLeft = true;
              break;
            }
          }
          if (anyBlocksLeft) break;
        }
        
        if (!anyBlocksLeft) {
          forts[f].alive = false;
        }
        
        return true;
      }
    }
  }
  return false;
}

bool checkAllFortsDestroyed() {
  for (int i = 0; i < 4; i++) {
    if (forts[i].alive) return false;
  }
  return true;
}

void drawExplosion(int x, int y, int frame) {
  // Multi-frame explosion animation
  uint16_t colors[] = {ST77XX_YELLOW, ST77XX_ORANGE, ST77XX_RED};
  uint16_t color = colors[frame % 3];
  
  if (frame == 0) {
    // Frame 1: Small burst
    tft.fillCircle(x, y, 3, color);
  } else if (frame == 1) {
    // Frame 2: Larger with particles
    tft.fillCircle(x, y, 5, color);
    tft.fillCircle(x - 4, y - 2, 2, ST77XX_YELLOW);
    tft.fillCircle(x + 4, y - 2, 2, ST77XX_YELLOW);
    tft.fillCircle(x, y + 4, 2, ST77XX_YELLOW);
  } else if (frame == 2) {
    // Frame 3: Dissipating
    tft.fillCircle(x, y, 6, ST77XX_RED);
    tft.fillCircle(x - 5, y - 3, 1, ST77XX_ORANGE);
    tft.fillCircle(x + 5, y - 3, 1, ST77XX_ORANGE);
    tft.fillCircle(x - 3, y + 4, 1, ST77XX_ORANGE);
    tft.fillCircle(x + 3, y + 4, 1, ST77XX_ORANGE);
  }
}

void eraseExplosion(int x, int y) {
  tft.fillRect(x - 8, y - 8, 16, 16, ST77XX_BLACK);
}

void updateExplosion() {
  if (explosion.active) {
    unsigned long elapsed = millis() - explosion.startTime;
    int frame = elapsed / 100;  // 100ms per frame
    
    if (frame < 3) {
      // Clear previous frame
      if (frame > 0) {
        eraseExplosion(explosion.x, explosion.y);
      }
      // Draw current frame
      drawExplosion(explosion.x, explosion.y, frame);
    } else {
      // Animation done
      eraseExplosion(explosion.x, explosion.y);
      explosion.active = false;

      // FIX: Redraw player ships if still alive after explosion ends
      if (currentMode == SINGLE_PLAYER && player1Lives > 0) {
        drawPlayer(player1X, player1Y, ST77XX_GREEN);
      } else if (currentMode == TWO_PLAYER) {
        if (player1Lives > 0) drawPlayer(player1X, player1Y, ST77XX_GREEN);
        if (player2Lives > 0) drawPlayer(player2X, player2Y, ST77XX_CYAN);
      }
    }
  }
}

void triggerExplosion(int x, int y) {
  explosion.x = x;
  explosion.y = y;
  explosion.active = true;
  explosion.startTime = millis();
}

void eraseAlien(int x, int y) {
  tft.fillRect(x, y, ALIEN_WIDTH, ALIEN_HEIGHT, ST77XX_BLACK);
}

void drawBullet(int x, int y, uint16_t color) {
  tft.fillRect(x, y, BULLET_SIZE, BULLET_SIZE * 2, color);
}

void eraseBullet(int x, int y) {
  tft.fillRect(x, y, BULLET_SIZE, BULLET_SIZE * 2, ST77XX_BLACK);
}

void updateHUDSinglePlayer() {
  tft.fillRect(0, 0, SCREEN_WIDTH, 10, ST77XX_BLACK);
  tft.setCursor(2, 2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.print("Score:");
  tft.print(player1Score);
  
  // Show rapid fire indicator (moved position for lives icons)
  if (rapidFireActive) {
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(65, 2); 
    tft.print("RAPID!");
  }

  // Draw player 1 life icons (P1 is GREEN)
  // Max 3 icons: (3 * 11 width) + (2 * 1 spacing) = 35 pixels wide. Start at 160 - 35 = 125
  int iconStartX = SCREEN_WIDTH - (3 * PLAYER_WIDTH + 2); // 125
  
  for (int i = 0; i < player1Lives; i++) {
      int iconX = iconStartX + i * (PLAYER_WIDTH + 1); // 11 width + 1 spacing
      
      // Draw 11x8 player sprite icon directly in the HUD area (Y=1 to center in 10px height)
      for (int row = 0; row < PLAYER_HEIGHT; row++) {
          for (int col = 0; col < PLAYER_WIDTH; col++) {
              if (playerSprite[row] & (1 << (PLAYER_WIDTH - 1 - col))) {
                  tft.drawPixel(iconX + col, 1 + row, ST77XX_GREEN);
              }
          }
      }
  }
}

void updateHUDTwoPlayer() {
  tft.fillRect(0, 0, SCREEN_WIDTH, 10, ST77XX_BLACK);
  tft.setTextSize(1);
  
  // P1 Score (Left)
  tft.setCursor(2, 2);
  tft.setTextColor(ST77XX_GREEN);
  tft.print("P1:");
  tft.print(player1Score);
  
  // Draw P1 life icons (11x8 sprite, P1 is GREEN)
  // Max 3 icons: (3 * 11 width) + (2 * 1 spacing) = 35 pixels wide. Start at 160/2 - 40 = 40.
  int p1IconStartX = SCREEN_WIDTH / 2 - 40; 
  
  for (int i = 0; i < player1Lives; i++) {
      int iconX = p1IconStartX + i * (PLAYER_WIDTH + 1); 
      for (int row = 0; row < PLAYER_HEIGHT; row++) {
          for (int col = 0; col < PLAYER_WIDTH; col++) {
              if (playerSprite[row] & (1 << (PLAYER_WIDTH - 1 - col))) {
                  tft.drawPixel(iconX + col, 1 + row, ST77XX_GREEN);
              }
          }
      }
  }
  
  // P2 Score (Right)
  tft.setCursor(SCREEN_WIDTH / 2 + 2, 2);
  tft.setTextColor(ST77XX_CYAN);
  tft.print("P2:");
  tft.print(player2Score);

  // Draw P2 life icons (11x8 sprite, P2 is CYAN)
  // Max 3 icons: Start at 160 - 35 = 125
  int p2IconStartX = SCREEN_WIDTH - 35; 
  
  for (int i = 0; i < player2Lives; i++) {
      int iconX = p2IconStartX + i * (PLAYER_WIDTH + 1); 
      for (int row = 0; row < PLAYER_HEIGHT; row++) {
          for (int col = 0; col < PLAYER_WIDTH; col++) {
              if (playerSprite[row] & (1 << (PLAYER_WIDTH - 1 - col))) {
                  tft.drawPixel(iconX + col, 1 + row, ST77XX_CYAN);
              }
          }
      }
  }
}

void updatePlayer1() {
  int newX = map(encoderPos, MIN_POS, MAX_POS, 0, SCREEN_WIDTH - PLAYER_WIDTH);
  // Ensure we only draw if the player is alive AND not currently exploding
  if (!explosion.active) {
    if (newX != player1X) {
      erasePlayer(player1X, player1Y);
      player1X = newX;
      if (player1Lives > 0) {
        drawPlayer(player1X, player1Y, ST77XX_GREEN);
      }
    } else if (player1Lives > 0) {
      // Redraw if player didn't move but needs to reappear (handled by updateExplosion too, but safe here)
      drawPlayer(player1X, player1Y, ST77XX_GREEN);
    }
  }
  player1X = newX;
}

void updatePlayer2() {
  if (!controllerConnected) return;
  
  // Map joystick (-32768 to 32767) to screen width
  int newX = map(xboxLeftStickX, -32768, 32767, 0, SCREEN_WIDTH - PLAYER_WIDTH);
  newX = constrain(newX, 0, SCREEN_WIDTH - PLAYER_WIDTH);
  
  // Ensure we only draw if the player is alive AND not currently exploding
  if (!explosion.active) {
    if (newX != player2X) {
      erasePlayer(player2X, player2Y);
      player2X = newX;
      if (player2Lives > 0) {
        drawPlayer(player2X, player2Y, ST77XX_CYAN);
      }
    } else if (player2Lives > 0) {
      // Redraw if player didn't move but needs to reappear
      drawPlayer(player2X, player2Y, ST77XX_CYAN);
    }
  }
  player2X = newX;
  
  // Handle A button for firing
  if (xboxAButton && !xboxAButtonPrev && !player2Bullet.active) {
    player2Bullet.x = player2X + PLAYER_WIDTH/2;
    player2Bullet.y = player2Y - 2;
    player2Bullet.active = true;
  }
  xboxAButtonPrev = xboxAButton;
}

void updateBulletsSinglePlayer() {
  // Player bullet
  if (player1Bullet.active) {
    eraseBullet(player1Bullet.x, player1Bullet.y);
    player1Bullet.y -= 4;
    
    // Check fort collision
    if (checkFortCollision(player1Bullet.x, player1Bullet.y, true)) {
      player1Bullet.active = false;
      if (checkAllFortsDestroyed()) {
        gameOver = true;
      }
    } else if (player1Bullet.y < 10) {
      player1Bullet.active = false;
    } else {
      drawBullet(player1Bullet.x, player1Bullet.y, ST77XX_YELLOW);
    }
  }
  
  // Alien bullets
  for (int i = 0; i < 3; i++) {
    if (alienBullets[i].active) {
      eraseBullet(alienBullets[i].x, alienBullets[i].y);
      alienBullets[i].y += 3;
      
      // Check fort collision
      if (checkFortCollision(alienBullets[i].x, alienBullets[i].y, false)) {
        alienBullets[i].active = false;
        if (checkAllFortsDestroyed()) {
          gameOver = true;
        }
      } else if (alienBullets[i].y > SCREEN_HEIGHT) {
        alienBullets[i].active = false;
      } else {
        drawBullet(alienBullets[i].x, alienBullets[i].y, ST77XX_RED);
        
        if (alienBullets[i].y >= player1Y && 
            alienBullets[i].x >= player1X && 
            alienBullets[i].x <= player1X + PLAYER_WIDTH) {
          alienBullets[i].active = false;
          
          erasePlayer(player1X, player1Y); // Erase player on hit for explosion sequence
          // Trigger explosion at player position
          triggerExplosion(player1X + PLAYER_WIDTH/2, player1Y + PLAYER_HEIGHT/2);
          
          player1Lives--;
          updateHUDSinglePlayer();
          if (player1Lives <= 0) {
            gameOver = true;
          }
        }
      }
    }
  }
}

void updateBulletsTwoPlayer() {
  // Player 1 bullet
  if (player1Bullet.active) {
    eraseBullet(player1Bullet.x, player1Bullet.y);
    player1Bullet.y -= 4;
    
    // Check fort collision
    if (checkFortCollision(player1Bullet.x, player1Bullet.y, true)) {
      player1Bullet.active = false;
      if (checkAllFortsDestroyed()) {
        gameOver = true;
      }
    } else if (player1Bullet.y < 10) {
      player1Bullet.active = false;
    } else {
      drawBullet(player1Bullet.x, player1Bullet.y, ST77XX_YELLOW);
    }
  }
  
  // Player 2 bullet
  if (player2Bullet.active) {
    eraseBullet(player2Bullet.x, player2Bullet.y);
    player2Bullet.y -= 4;
    
    // Check fort collision
    if (checkFortCollision(player2Bullet.x, player2Bullet.y, true)) {
      player2Bullet.active = false;
      if (checkAllFortsDestroyed()) {
        gameOver = true;
      }
    } else if (player2Bullet.y < 10) {
      player2Bullet.active = false;
    } else {
      drawBullet(player2Bullet.x, player2Bullet.y, ST77XX_MAGENTA);
    }
  }
  
  // Alien bullets hit either player or forts
  for (int i = 0; i < 3; i++) {
    if (alienBullets[i].active) {
      eraseBullet(alienBullets[i].x, alienBullets[i].y);
      alienBullets[i].y += 3;
      
      // Check fort collision
      if (checkFortCollision(alienBullets[i].x, alienBullets[i].y, false)) {
        alienBullets[i].active = false;
        if (checkAllFortsDestroyed()) {
          gameOver = true;
        }
      } else if (alienBullets[i].y > SCREEN_HEIGHT) {
        alienBullets[i].active = false;
      } else {
        drawBullet(alienBullets[i].x, alienBullets[i].y, ST77XX_RED);
        
        // Check collision with player 1
        if (alienBullets[i].y >= player1Y && 
            alienBullets[i].x >= player1X && 
            alienBullets[i].x <= player1X + PLAYER_WIDTH) {
          alienBullets[i].active = false;
          
          erasePlayer(player1X, player1Y); // Erase player on hit for explosion sequence
          
          // Trigger explosion at player 1 position
          triggerExplosion(player1X + PLAYER_WIDTH/2, player1Y + PLAYER_HEIGHT/2);
          
          player1Lives--;
          updateHUDTwoPlayer();
          if (player1Lives <= 0 && player2Lives <= 0) {
            gameOver = true;
          }
        }
        
        // Check collision with player 2
        if (alienBullets[i].y >= player2Y && 
            alienBullets[i].x >= player2X && 
            alienBullets[i].x <= player2X + PLAYER_WIDTH) {
          alienBullets[i].active = false;
          
          erasePlayer(player2X, player2Y); // Erase player on hit for explosion sequence
          
          // Trigger explosion at player 2 position
          triggerExplosion(player2X + PLAYER_WIDTH/2, player2Y + PLAYER_HEIGHT/2);
          
          player2Lives--;
          updateHUDTwoPlayer();
          if (player1Lives <= 0 && player2Lives <= 0) {
            gameOver = true;
          }
        }
      }
    }
  }
}

void updateAliens() {
  if (millis() - lastAlienMove < alienMoveDelay) return;
  lastAlienMove = millis();
  
  bool hitEdge = false;
  
  // Check regular aliens
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      if (aliens[row][col].alive) {
        if ((aliens[row][col].x <= 0 && alienDirection < 0) ||
            (aliens[row][col].x >= SCREEN_WIDTH - ALIEN_WIDTH && alienDirection > 0)) {
          hitEdge = true;
          break;
        }
      }
    }
    if (hitEdge) break;
  }
  
  // Handle bouncing - need to bounce twice before descending
  if (hitEdge) {
    alienDirection = -alienDirection;
    alienBounces++;
    if (alienBounces >= 2) {
      aliensShouldDescend = true;
      alienBounces = 0;
    }
  }
  
  // Move regular aliens
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      if (aliens[row][col].alive) {
        eraseAlien(aliens[row][col].x, aliens[row][col].y);
        aliens[row][col].x += alienDirection * 2;
        if (aliensShouldDescend) aliens[row][col].y += 4;
        drawAlienType(aliens[row][col].x, aliens[row][col].y, aliens[row][col].type);
        
        if (aliens[row][col].y >= player1Y - ALIEN_HEIGHT) {
          gameOver = true;
        }
      }
    }
  }
  
  if (aliensShouldDescend) {
    aliensShouldDescend = false;
  }
  
  // Move super alien faster (independent movement)
  if (superAlien.active && millis() - lastSuperMove > 20) {
    lastSuperMove = millis();
    eraseSuperAlien(superAlien.x, superAlien.y);
    superAlien.x += 3;
    if (superAlien.x > SCREEN_WIDTH) {
      superAlien.active = false;
    } else {
      drawSuperAlien(superAlien.x, superAlien.y);
    }
  }
  
  // Only the lowest alien in each column can shoot
  int shootChance = (currentMode == TWO_PLAYER) ? 25 : 35;
  if (random(100) < shootChance) {
    for (int i = 0; i < 3; i++) {
      if (!alienBullets[i].active) {
        // Pick random column
        int col = random(activeAlienCols);
        
        // Find lowest alive alien in that column
        int lowestRow = -1;
        for (int row = activeAlienRows - 1; row >= 0; row--) {
          if (aliens[row][col].alive) {
            lowestRow = row;
            break;
          }
        }
        
        if (lowestRow >= 0) {
          alienBullets[i].x = aliens[lowestRow][col].x + ALIEN_WIDTH/2;
          alienBullets[i].y = aliens[lowestRow][col].y + ALIEN_HEIGHT;
          alienBullets[i].active = true;
          break;
        }
      }
    }
  }
  
  // Spawn super alien occasionally in single player
  if (currentMode == SINGLE_PLAYER && !superAlien.active && 
      millis() - lastSuperSpawn > 15000 && random(100) < 5) {
    superAlien.x = 0;
    superAlien.y = 12;
    superAlien.active = true;
    superAlien.lives = 1; // Consistency update
    lastSuperSpawn = millis();
    lastSuperMove = millis();
  }
}

void checkCollisions() {
  // Check rapid fire timeout
  if (rapidFireActive && millis() > rapidFireEnd) {
    rapidFireActive = false;
    updateHUDSinglePlayer();
  }
  
  // Check player 1 bullet vs super alien (same hitbox as regular alien now)
  if (player1Bullet.active && superAlien.active) {
    if (player1Bullet.x >= superAlien.x &&
        player1Bullet.x <= superAlien.x + ALIEN_WIDTH &&
        player1Bullet.y <= superAlien.y + ALIEN_HEIGHT &&
        player1Bullet.y >= superAlien.y) {
      
      eraseBullet(player1Bullet.x, player1Bullet.y);
      player1Bullet.active = false;
      
      // Trigger explosion on super alien hit
      triggerExplosion(superAlien.x + ALIEN_WIDTH/2, superAlien.y + ALIEN_HEIGHT/2);
      
      superAlien.lives--;
      if (superAlien.lives <= 0) {
        eraseSuperAlien(superAlien.x, superAlien.y);
        superAlien.active = false;
        player1Score += 50;
        
        // Activate rapid fire!
        rapidFireActive = true;
        rapidFireEnd = millis() + 5000;  // 5 seconds of rapid fire
        
        if (currentMode == SINGLE_PLAYER) {
          updateHUDSinglePlayer();
        }
      }
      return;
    }
  }
  
  // Check player 1 bullet
  if (player1Bullet.active) {
    for (int row = 0; row < activeAlienRows; row++) {
      for (int col = 0; col < activeAlienCols; col++) {
        if (aliens[row][col].alive) {
          if (player1Bullet.x >= aliens[row][col].x &&
              player1Bullet.x <= aliens[row][col].x + ALIEN_WIDTH &&
              player1Bullet.y <= aliens[row][col].y + ALIEN_HEIGHT &&
              player1Bullet.y >= aliens[row][col].y) {
            
            // Erase bullet before marking alien as dead
            eraseBullet(player1Bullet.x, player1Bullet.y);
            player1Bullet.active = false;
            
            aliens[row][col].alive = false;
            eraseAlien(aliens[row][col].x, aliens[row][col].y);
            player1Score += 10;
            
            if (currentMode == SINGLE_PLAYER) {
              updateHUDSinglePlayer();
              if (alienMoveDelay > 100) alienMoveDelay -= 10;
            } else {
              updateHUDTwoPlayer();
            }
            return;
          }
        }
      }
    }
  }
  
  // Check player 2 bullet (co-op mode)
  if (currentMode == TWO_PLAYER && player2Bullet.active) {
    for (int row = 0; row < activeAlienRows; row++) {
      for (int col = 0; col < activeAlienCols; col++) {
        if (aliens[row][col].alive) {
          if (player2Bullet.x >= aliens[row][col].x &&
              player2Bullet.x <= aliens[row][col].x + ALIEN_WIDTH &&
              player2Bullet.y <= aliens[row][col].y + ALIEN_HEIGHT &&
              player2Bullet.y >= aliens[row][col].y) {
            
            // Erase bullet before marking alien as dead
            eraseBullet(player2Bullet.x, player2Bullet.y);
            player2Bullet.active = false;
            
            aliens[row][col].alive = false;
            eraseAlien(aliens[row][col].x, aliens[row][col].y);
            player2Score += 10;
            updateHUDTwoPlayer();
            return;
          }
        }
      }
    }
  }
}

void checkWin() {
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      if (aliens[row][col].alive) return;
    }
  }
  
  // All aliens defeated! Give only 1 life back (not full reset)
  if (currentMode == SINGLE_PLAYER) {
    if (player1Lives < 3) {
      player1Lives++;  // Add 1 life, max 3
    }
    resetAliensOnly();  // Only reset aliens, keep forts
    alienMoveDelay = max(100, alienMoveDelay - 50);
    tft.fillScreen(ST77XX_BLACK);
    drawPlayer(player1X, player1Y, ST77XX_GREEN);
    drawAllForts();  // Redraw existing forts (damaged state preserved)
    updateHUDSinglePlayer();
  } else {
    if (player1Lives < 3) player1Lives++;
    if (player2Lives < 3) player2Lives++;
    
    // Reset aliens for co-op
    for (int row = 0; row < activeAlienRows; row++) {
      for (int col = 0; col < activeAlienCols; col++) {
        aliens[row][col].x = 5 + col * 14;
        aliens[row][col].y = 12 + row * 10;
        aliens[row][col].alive = true;
        aliens[row][col].lives = 1;
        aliens[row][col].type = row % 4;
      }
    }
    
    alienDirection = 1;
    alienBounces = 0;
    aliensShouldDescend = false;
    superAlien.active = false;
    superAlien.lives = 1; // Consistency update
    rapidFireActive = false;
    
    alienMoveDelay = max(100, alienMoveDelay - 50);
    tft.fillScreen(ST77XX_BLACK);
    drawPlayer(player1X, player1Y, ST77XX_GREEN);
    drawPlayer(player2X, player2Y, ST77XX_CYAN);
    drawAllForts();  // Redraw existing forts (damaged state preserved)
    updateHUDTwoPlayer();
  }
}

void handleMenu() {
  // --- MENU SENSITIVITY FIX (WITH THRESHOLD) ---
  // Uses a threshold of 4 steps to ensure the menu only switches on deliberate turns.
  static int lastRawEncoderPos = encoderPos; 
  const int MENU_SENSITIVITY_THRESHOLD = 4;

  if (abs(encoderPos - lastRawEncoderPos) >= MENU_SENSITIVITY_THRESHOLD) {
      if (encoderPos > lastRawEncoderPos) {
          // Moved down (Clockwise)
          menuSelection = (menuSelection + 1) % 2;
      } else {
          // Moved up (Counter-Clockwise)
          menuSelection = (menuSelection - 1 + 2) % 2;
      }
      // Reset the reference position to the current position to start tracking the next movement
      lastRawEncoderPos = encoderPos; 
      drawMenu();
  }
  // --- END MENU SENSITIVITY FIX ---
  
  // Start button selects menu option
  if (menuPressed) {
    menuPressed = false;
    tft.fillScreen(ST77XX_BLACK);
    
    if (menuSelection == 0) {
      currentMode = SINGLE_PLAYER;
      initSinglePlayer();
      drawPlayer(player1X, player1Y, ST77XX_GREEN);
      drawAllForts();
      updateHUDSinglePlayer();
    } else {
      currentMode = TWO_PLAYER;
      
      // Try to connect to Xbox controller
      tft.fillScreen(ST77XX_BLACK);
      tft.setCursor(10, 40);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.println("Scanning for Xbox");
      tft.println("  controller...");
      tft.println("");
      tft.println("Hold Xbox + Sync");
      tft.println("buttons on controller");
      
      if (connectToXboxController()) {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(20, 50);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(2);
        tft.println("CONNECTED!");
        tft.setTextSize(1);
        tft.setCursor(15, 80);
        tft.setTextColor(ST77XX_WHITE);
        tft.println("Controller ready");
        delay(2000);
      } else {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(15, 50);
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(1);
        tft.println("Controller not found");
        tft.setCursor(10, 70);
        tft.setTextColor(ST77XX_WHITE);
        tft.println("Playing without P2");
        delay(2000);
      }
      
      tft.fillScreen(ST77XX_BLACK);
      initTwoPlayer();
      drawPlayer(player1X, player1Y, ST77XX_GREEN);
      drawPlayer(player2X, player2Y, ST77XX_CYAN);
      drawAllForts();
      updateHUDTwoPlayer();
    }
  }
}

void handleGameOver() {
  int finalScore = 0;
  if (currentMode == SINGLE_PLAYER) {
    finalScore = player1Score;
  } else {
    // In co-op, use the combined score for high score tracking
    finalScore = player1Score + player2Score;
  }

  // Check and save new High Score (NEW LOGIC)
  if (finalScore > currentHighScore) {
    currentHighScore = finalScore;
    preferences.putInt(HIGH_SCORE_KEY, currentHighScore);
  }
  
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(30, 30); // Move title up
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.println("GAME OVER");
  
  tft.setTextSize(1);
  tft.setCursor(20, 60);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print("HIGH-SCORE: "); 
  tft.println(currentHighScore);
  
  tft.setTextColor(ST77XX_WHITE);
  
  if (currentMode == SINGLE_PLAYER) {
    tft.setCursor(20, 80);
    tft.print("Score: ");
    tft.println(player1Score);
  } else {
    tft.setCursor(20, 80);
    tft.setTextColor(ST77XX_GREEN);
    tft.print("P1 Score: ");
    tft.println(player1Score);
    tft.setCursor(20, 95);
    tft.setTextColor(ST77XX_CYAN);
    tft.print("P2 Score: ");
    tft.println(player2Score);
    tft.setCursor(20, 110);
    tft.setTextColor(ST77XX_WHITE);
    tft.print("Total: ");
    tft.println(finalScore);
  }
  
  tft.setCursor(15, 120);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("Press start button");
  
  while (!menuPressed && !xboxAButton) {
    delay(10);
  }
  menuPressed = false;
  xboxAButtonPrev = false;
  
  currentMode = MENU;
  menuSelection = 0;
  encoderPos = 0;
  drawMenu();
}

void loop() {
  
  if (currentMode == MENU) {
    handleMenu();
    
    // Removed the periodic redraw logic that was likely causing flicker.
    // Menu redraw relies solely on handleMenu() when encoder threshold is met.
    
    delay(20);
    return;
  }
  
  if (gameOver) {
    handleGameOver();
    return;
  }
  
  // 1. Explosion/Redraw Logic must run first
  updateExplosion();
  
  if (currentMode == SINGLE_PLAYER) {
    // Single player mode
    // Regular fire - press to shoot with cooldown
    if (firePressed && !player1Bullet.active && millis() - lastFireTime >= FIRE_COOLDOWN) {
      player1Bullet.x = player1X + PLAYER_WIDTH/2;
      player1Bullet.y = player1Y - 2;
      player1Bullet.active = true;
      lastFireTime = millis();
      firePressed = false;
    }
    
    // Rapid fire mode - hold fire button for auto-fire
    if (rapidFireActive && firePressed && !player1Bullet.active && millis() - lastFireTime >= 150) {
      player1Bullet.x = player1X + PLAYER_WIDTH/2;
      player1Bullet.y = player1Y - 2;
      player1Bullet.active = true;
      lastFireTime = millis();
      // Don't clear firePressed - let it stay pressed for rapid fire
    }
    
    updatePlayer1();
    updateBulletsSinglePlayer();
    updateAliens();
    checkCollisions();
    checkWin();
    
  } else if (currentMode == TWO_PLAYER) {
    // Two player CO-OP mode
    if (firePressed && !player1Bullet.active && millis() - lastFireTime >= FIRE_COOLDOWN) {
      player1Bullet.x = player1X + PLAYER_WIDTH/2;
      player1Bullet.y = player1Y - 2;
      player1Bullet.active = true;
      lastFireTime = millis();
      firePressed = false;
    }
    
    updatePlayer1();
    updatePlayer2();
    updateBulletsTwoPlayer();
    updateAliens();
    checkCollisions();
    checkWin();
    updateHUDTwoPlayer();
  }
  
  delay(20);
}