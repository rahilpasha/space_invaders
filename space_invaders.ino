#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <NimBLEDevice.h>

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

// Create display object
Adafruit_ST7735 tft = Adafruit_ST7735(LCD_CS, LCD_DC, LCD_RESET);

// Game constants
#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 128
#define PLAYER_WIDTH 8
#define PLAYER_HEIGHT 6
#define ALIEN_WIDTH 6
#define ALIEN_HEIGHT 5
#define ALIEN_ROWS 3
#define ALIEN_COLS 8
#define ALIEN_ROWS_COOP 5  // More aliens for co-op
#define ALIEN_COLS_COOP 10
#define BULLET_SIZE 2

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
};
Alien aliens[ALIEN_ROWS_COOP][ALIEN_COLS_COOP];  // Use max size for both modes
int alienDirection = 1;
unsigned long lastAlienMove = 0;
int alienMoveDelay = 500;
int activeAlienRows = ALIEN_ROWS;  // Track how many rows are active
int activeAlienCols = ALIEN_COLS;

// Game state
bool gameOver = false;
volatile bool firePressed = false;

// Xbox Controller BLE
NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pInputCharacteristic = nullptr;
bool controllerConnected = false;
int16_t xboxLeftStickX = 0;
bool xboxAButton = false;
bool xboxAButtonPrev = false;

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
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->start(5, false);
  
  NimBLEScanResults scanResults = pScan->getResults();
  
  for (int i = 0; i < scanResults.getCount(); i++) {
    const NimBLEAdvertisedDevice* device = scanResults.getDevice(i);
    
    // Look for Xbox controller (check name or service UUID)
    std::string deviceName = device->getName();
    if (deviceName.find("Xbox") != std::string::npos ||
        deviceName.find("Controller") != std::string::npos) {
      
      Serial.print("Found controller: ");
      Serial.println(deviceName.c_str());
      
      pClient = NimBLEDevice::createClient();
      pClient->setClientCallbacks(new ClientCallbacks());
      
      if (pClient->connect(device)) {
        Serial.println("Connected to controller");
        
        // Get HID service (0x1812)
        NimBLERemoteService* pService = pClient->getService(NimBLEUUID((uint16_t)0x1812));
        if (pService) {
          // Get Report characteristic (0x2A4D)
          pInputCharacteristic = pService->getCharacteristic(NimBLEUUID((uint16_t)0x2A4D));
          if (pInputCharacteristic && pInputCharacteristic->canNotify()) {
            pInputCharacteristic->subscribe(true, notifyCallback);
            return true;
          }
        }
      }
    }
  }
  return false;
}

// Interrupt handlers
void IRAM_ATTR startButtonISR() {
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

  // Initialize SPI
  SPI.begin(LCD_SCK, -1, LCD_SDA, LCD_CS);
  
  // Initialize display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  
  // Setup inputs
  pinMode(startPin, INPUT_PULLDOWN);
  pinMode(knobA, INPUT_PULLUP);
  pinMode(knobB, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(startPin), startButtonISR, RISING);
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
  
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  
  // Menu options
  tft.setCursor(20, 70);
  if (menuSelection == 0) tft.print(">");
  tft.println(" 1 Player vs AI");
  
  tft.setCursor(20, 85);
  if (menuSelection == 1) tft.print(">");
  tft.println(" 2 Player Co-op");
  
  tft.setCursor(10, 110);
  tft.setTextColor(ST77XX_CYAN);
  tft.println("Turn encoder & fire");
}

void initSinglePlayer() {
  activeAlienRows = ALIEN_ROWS;
  activeAlienCols = ALIEN_COLS;
  
  // Initialize aliens
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      aliens[row][col].x = 10 + col * 18;
      aliens[row][col].y = 20 + row * 12;
      aliens[row][col].alive = true;
    }
  }
  
  for (int i = 0; i < 3; i++) {
    alienBullets[i].active = false;
  }
  
  alienDirection = 1;
  alienMoveDelay = 500;
  player1Bullet.active = false;
  player1X = SCREEN_WIDTH / 2;
  player1Score = 0;
  player1Lives = 3;
  gameOver = false;
}

void initTwoPlayer() {
  activeAlienRows = ALIEN_ROWS_COOP;
  activeAlienCols = ALIEN_COLS_COOP;
  
  // Initialize MORE aliens for co-op challenge
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      aliens[row][col].x = 5 + col * 15;
      aliens[row][col].y = 12 + row * 10;
      aliens[row][col].alive = true;
    }
  }
  
  for (int i = 0; i < 3; i++) {
    alienBullets[i].active = false;
  }
  
  alienDirection = 1;
  alienMoveDelay = 400;  // Faster aliens in co-op
  
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
}

void drawPlayer(int x, int y, uint16_t color) {
  tft.fillTriangle(x, y + PLAYER_HEIGHT, 
                    x + PLAYER_WIDTH/2, y,
                    x + PLAYER_WIDTH, y + PLAYER_HEIGHT,
                    color);
}

void erasePlayer(int x, int y) {
  drawPlayer(x, y, ST77XX_BLACK);
}

void drawAlien(int x, int y) {
  tft.fillRect(x, y, ALIEN_WIDTH, ALIEN_HEIGHT, ST77XX_RED);
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
  tft.print(" Lives:");
  tft.print(player1Lives);
}

void updateHUDTwoPlayer() {
  tft.fillRect(0, 0, SCREEN_WIDTH, 10, ST77XX_BLACK);
  tft.setCursor(2, 2);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.print("P1:");
  tft.print(player1Lives);
  tft.print("/");
  tft.print(player1Score);
  tft.setTextColor(ST77XX_CYAN);
  tft.print(" P2:");
  tft.print(player2Lives);
  tft.print("/");
  tft.print(player2Score);
}

void updatePlayer1() {
  int newX = map(encoderPos, MIN_POS, MAX_POS, 0, SCREEN_WIDTH - PLAYER_WIDTH);
  if (newX != player1X) {
    erasePlayer(player1X, player1Y);
    player1X = newX;
    drawPlayer(player1X, player1Y, ST77XX_GREEN);
  }
}

void updatePlayer2() {
  if (!controllerConnected) return;
  
  // Map joystick (-32768 to 32767) to screen width
  int newX = map(xboxLeftStickX, -32768, 32767, 0, SCREEN_WIDTH - PLAYER_WIDTH);
  newX = constrain(newX, 0, SCREEN_WIDTH - PLAYER_WIDTH);
  
  if (newX != player2X) {
    erasePlayer(player2X, player2Y);
    player2X = newX;
    drawPlayer(player2X, player2Y, ST77XX_CYAN);
  }
  
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
    
    if (player1Bullet.y < 10) {
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
      
      if (alienBullets[i].y > SCREEN_HEIGHT) {
        alienBullets[i].active = false;
      } else {
        drawBullet(alienBullets[i].x, alienBullets[i].y, ST77XX_RED);
        
        if (alienBullets[i].y >= player1Y && 
            alienBullets[i].x >= player1X && 
            alienBullets[i].x <= player1X + PLAYER_WIDTH) {
          alienBullets[i].active = false;
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
    
    if (player1Bullet.y < 10) {
      player1Bullet.active = false;
    } else {
      drawBullet(player1Bullet.x, player1Bullet.y, ST77XX_YELLOW);
    }
  }
  
  // Player 2 bullet
  if (player2Bullet.active) {
    eraseBullet(player2Bullet.x, player2Bullet.y);
    player2Bullet.y -= 4;
    
    if (player2Bullet.y < 10) {
      player2Bullet.active = false;
    } else {
      drawBullet(player2Bullet.x, player2Bullet.y, ST77XX_MAGENTA);
    }
  }
  
  // Alien bullets hit either player
  for (int i = 0; i < 3; i++) {
    if (alienBullets[i].active) {
      eraseBullet(alienBullets[i].x, alienBullets[i].y);
      alienBullets[i].y += 3;
      
      if (alienBullets[i].y > SCREEN_HEIGHT) {
        alienBullets[i].active = false;
      } else {
        drawBullet(alienBullets[i].x, alienBullets[i].y, ST77XX_RED);
        
        // Check collision with player 1
        if (alienBullets[i].y >= player1Y && 
            alienBullets[i].x >= player1X && 
            alienBullets[i].x <= player1X + PLAYER_WIDTH) {
          alienBullets[i].active = false;
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
  
  bool moveDown = false;
  
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      if (aliens[row][col].alive) {
        if ((aliens[row][col].x <= 0 && alienDirection < 0) ||
            (aliens[row][col].x >= SCREEN_WIDTH - ALIEN_WIDTH && alienDirection > 0)) {
          moveDown = true;
          alienDirection = -alienDirection;
          break;
        }
      }
    }
    if (moveDown) break;
  }
  
  for (int row = 0; row < activeAlienRows; row++) {
    for (int col = 0; col < activeAlienCols; col++) {
      if (aliens[row][col].alive) {
        eraseAlien(aliens[row][col].x, aliens[row][col].y);
        aliens[row][col].x += alienDirection * 2;
        if (moveDown) aliens[row][col].y += 4;
        drawAlien(aliens[row][col].x, aliens[row][col].y);
        
        if (aliens[row][col].y >= player1Y - ALIEN_HEIGHT) {
          gameOver = true;
        }
      }
    }
  }
  
  // More frequent shooting in co-op mode
  int shootChance = (currentMode == TWO_PLAYER) ? 15 : 10;
  if (random(100) < shootChance) {
    for (int i = 0; i < 3; i++) {
      if (!alienBullets[i].active) {
        int row = random(activeAlienRows);
        int col = random(activeAlienCols);
        if (aliens[row][col].alive) {
          alienBullets[i].x = aliens[row][col].x + ALIEN_WIDTH/2;
          alienBullets[i].y = aliens[row][col].y + ALIEN_HEIGHT;
          alienBullets[i].active = true;
          break;
        }
      }
    }
  }
}

void checkCollisions() {
  // Check player 1 bullet
  if (player1Bullet.active) {
    for (int row = 0; row < activeAlienRows; row++) {
      for (int col = 0; col < activeAlienCols; col++) {
        if (aliens[row][col].alive) {
          if (player1Bullet.x >= aliens[row][col].x &&
              player1Bullet.x <= aliens[row][col].x + ALIEN_WIDTH &&
              player1Bullet.y <= aliens[row][col].y + ALIEN_HEIGHT &&
              player1Bullet.y >= aliens[row][col].y) {
            
            aliens[row][col].alive = false;
            eraseAlien(aliens[row][col].x, aliens[row][col].y);
            player1Bullet.active = false;
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
            
            aliens[row][col].alive = false;
            eraseAlien(aliens[row][col].x, aliens[row][col].y);
            player2Bullet.active = false;
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
  
  // All aliens defeated!
  if (currentMode == SINGLE_PLAYER) {
    initSinglePlayer();
    alienMoveDelay = max(100, alienMoveDelay - 100);
    tft.fillScreen(ST77XX_BLACK);
    drawPlayer(player1X, player1Y, ST77XX_GREEN);
  } else {
    initTwoPlayer();
    alienMoveDelay = max(100, alienMoveDelay - 50);
    tft.fillScreen(ST77XX_BLACK);
    drawPlayer(player1X, player1Y, ST77XX_GREEN);
    drawPlayer(player2X, player2Y, ST77XX_CYAN);
  }
}

void handleMenu() {
  // Encoder changes menu selection
  static int lastEncoderPos = encoderPos;
  if (encoderPos != lastEncoderPos) {
    menuSelection = (encoderPos % 2);
    lastEncoderPos = encoderPos;
    drawMenu();
  }
  
  // Fire button selects
  if (firePressed) {
    firePressed = false;
    tft.fillScreen(ST77XX_BLACK);
    
    if (menuSelection == 0) {
      currentMode = SINGLE_PLAYER;
      initSinglePlayer();
      drawPlayer(player1X, player1Y, ST77XX_GREEN);
      updateHUDSinglePlayer();
    } else {
      currentMode = TWO_PLAYER;
      
      // Try to connect to Xbox controller
      tft.setCursor(10, 50);
      tft.setTextColor(ST77XX_WHITE);
      tft.println("Connecting Xbox...");
      
      if (connectToXboxController()) {
        tft.println("Connected!");
      } else {
        tft.println("Not found");
        tft.println("Starting anyway");
      }
      delay(2000);
      
      tft.fillScreen(ST77XX_BLACK);
      initTwoPlayer();
      drawPlayer(player1X, player1Y, ST77XX_GREEN);
      drawPlayer(player2X, player2Y, ST77XX_CYAN);
      updateHUDTwoPlayer();
    }
  }
}

void handleGameOver() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(30, 50);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.println("GAME OVER");
  
  tft.setTextSize(1);
  tft.setCursor(20, 75);
  tft.setTextColor(ST77XX_WHITE);
  
  if (currentMode == SINGLE_PLAYER) {
    tft.print("Score: ");
    tft.println(player1Score);
  } else {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("P1 Score: ");
    tft.println(player1Score);
    tft.setCursor(20, 90);
    tft.setTextColor(ST77XX_CYAN);
    tft.print("P2 Score: ");
    tft.println(player2Score);
  }
  
  tft.setCursor(15, 110);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("Press to continue");
  
  while (!firePressed && !xboxAButton) {
    delay(10);
  }
  firePressed = false;
  xboxAButtonPrev = false;
  
  currentMode = MENU;
  menuSelection = 0;
  encoderPos = 0;
  drawMenu();
}

void loop() {
  if (currentMode == MENU) {
    handleMenu();
    delay(50);
    return;
  }
  
  if (gameOver) {
    handleGameOver();
    return;
  }
  
  if (currentMode == SINGLE_PLAYER) {
    // Single player mode
    if (firePressed && !player1Bullet.active) {
      player1Bullet.x = player1X + PLAYER_WIDTH/2;
      player1Bullet.y = player1Y - 2;
      player1Bullet.active = true;
      firePressed = false;
    }
    
    updatePlayer1();
    updateBulletsSinglePlayer();
    updateAliens();
    checkCollisions();
    checkWin();
    
  } else if (currentMode == TWO_PLAYER) {
    // Two player CO-OP mode
    if (firePressed && !player1Bullet.active) {
      player1Bullet.x = player1X + PLAYER_WIDTH/2;
      player1Bullet.y = player1Y - 2;
      player1Bullet.active = true;
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