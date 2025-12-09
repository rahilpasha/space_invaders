const int startPin = 7;
const int knobPin = 15;
const int knobA = 4;
const int knobB = 5;

volatile int encoderPos = 0;
volatile int lastEncoded = 0;
volatile unsigned long lastInterruptTime = 0;

const int MIN_POS = 0;
const int MAX_POS = 50;

void IRAM_ATTR startButtonISR() {
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > 200) {
    Serial.println("Fire!");
  }
  lastInterruptTime = interruptTime;
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

  pinMode(startPin, INPUT_PULLDOWN);
  pinMode(knobA, INPUT_PULLUP);
  pinMode(knobB, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(startPin), startButtonISR, RISING);
  attachInterrupt(digitalPinToInterrupt(knobA), updateEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(knobB), updateEncoder, CHANGE);
  
  Serial.println("Ready!");
  Serial.print("Position: ");
  Serial.println(encoderPos);
}

void loop() {
  static int lastPos = -1;
  
  if (encoderPos != lastPos) {
    lastPos = encoderPos;
    Serial.print("Position: ");
    Serial.println(encoderPos);
  }
  
  delay(50);
}