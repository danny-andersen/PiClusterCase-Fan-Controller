#include <Wire.h>

const int i2cChannel = 0x10;

const int numFans = 4;
const int fanDriverPins[4] = {3, 9, 10, 11};
const int speedIndicatorGreenPins[4] = { 0, 2, 7, 12 };
const int speedIndicatorRedPins[4] = { 1, 4, 8, 13 };
const int enableDriverPair1Pin = 5;
const int enableDriverPair2Pin = 6;
//int powerStatePins[4] = { 14, 15, 16, 17};
int powerStatePins[4] = { A0, A1, A2, A3};

int currentSpeed[4] = {0, 0, 0, 0};
int powerState[4] = {0, 0, 0, 0};
const int noSlotsOnMinVolts = 593; //If measured power state voltage > 2.9 no slots are on
const int oneSlotOnMinVolts = 350; //If measured power state voltage > 1.7V and <= 2.9V only one slot is on
const byte startOfMsg = 0x55;
const byte endOfMsg = 0xAA;
const byte noFanSpeed = 0xFF;
byte minSpeed[4] = {30, 30, 30, 80}; //These are the defaults
byte mediumSpeed[4] = {30, 30, 30, 80}; //Orange Indicator threshold
byte defaultSpeed[4] = {30, 30, 30, 80}; //One slot on 
byte highSpeed[4] = {50, 50, 50, 120}; //Red Indicator threshold
byte maxSpeed[4] = {50, 50, 50, 120};
byte rangeMultiplier = 2;
byte fanSupplyVolts = 120;
int timeSinceMsg = 15000; //Start as though no received a message
int delayTime = 50; // loop every 50ms
const int messageTimeout = 15000;  // If no message of 15 seconds then use power detection to drive fan on or off
int msgRx = LOW;
bool pwmOut = false; //Whether to drive output speed as a PWM signal or not.
int blinkCnt = 0; //Used to flash indicator LED if not using temp input but just using power sensing
int blinkState = HIGH; //Initial state for indicator LED
const int BLINK_RATE = 500; // Number of ms to turn LED on / off

void setup() {
  for (int i = 0; i < numFans; i++) {
    pinMode(speedIndicatorGreenPins[i], OUTPUT);
    pinMode(speedIndicatorRedPins[i], OUTPUT);
//    pinMode(powerStatePins[i], INPUT);
  }
  pinMode(enableDriverPair1Pin, OUTPUT);
  pinMode(enableDriverPair2Pin, OUTPUT);
  //Set everything to off
  digitalWrite(enableDriverPair1Pin, LOW);
  digitalWrite(enableDriverPair2Pin, LOW);
  for (int i = 0; i < numFans; i++) {
    controlFan(i, 0, LOW);
  }
  Wire.begin(i2cChannel);            // join i2c bus with address #0x10
  Wire.onReceive(receiveMessage); // register receiver
  //Debug I2C bus
//  pinMode(13, OUTPUT);
//  digitalWrite(13, LOW);
}

void loop() {
  delay(delayTime); //ms
  timeSinceMsg += delayTime;
  for (int i=0; i<numFans; i++) {
    powerState[i] = analogRead(powerStatePins[i]);
  }
  if (timeSinceMsg >= messageTimeout) {
    blinkCnt += delayTime;
    timeSinceMsg = messageTimeout; //Dont increment timeout further to stop it rolling over
    //No control message received recently - send no fan speed
    //This will drive the fan according to the power status
    if (blinkCnt >= BLINK_RATE) {
      blinkState = not blinkState;
      blinkCnt = 0;
    } 
    for (int i=0; i<numFans; i++) {
      controlFan(i, noFanSpeed, blinkState);
    }
  } else {
    blinkCnt = 0;
  }
}

//Process any message received
//Message format: {0x55, minSpeed1, maxSpeed1, speed1, minSpeed2, maxSpeed2, speed2, 
// minSpeed3, maxSpeed3, speed3, minSpeed4, maxSpeed4, speed4, pwmOn, fanSupplyVolts, 0xaa}
//Speed Range: 0 = off, 50 = full speed, 0xff = status not known
void receiveMessage(int numBytes) {
  int fanNum = 0;
  int newMinSpeed = -1;
  int newMaxSpeed = -1;
  int newSpeed[4] = {0, 0, 0, 0};
  bool started = false;
  bool pwmRx = false;
  while (Wire.available()) {
    // loop through all bytes
    byte b = Wire.read(); // receive byte
    if ((not started) && (b == startOfMsg)) {
      started = true;
      fanNum = 0;
      timeSinceMsg = 0;
      //Blink led (note comment out when using indicator for fan 4)
//      digitalWrite(13, HIGH);
//      if (msgRx == LOW) {
//        digitalWrite(13, HIGH);
//        msgRx = HIGH;
//      } else {
//        digitalWrite(13, LOW);
//        msgRx = LOW;
//      }
    } else if (started) {
      if (newMinSpeed == -1) {
        newMinSpeed = b;
      } else if (newMaxSpeed == -1) {
        newMaxSpeed = b;
      } else if (fanNum < numFans) {
        newSpeed[fanNum] = b;
        if (newMinSpeed != minSpeed[fanNum] || newMaxSpeed != maxSpeed[fanNum]) {
          minSpeed[fanNum] = newMinSpeed;
          maxSpeed[fanNum] = newMaxSpeed;
          mediumSpeed[fanNum] = minSpeed[fanNum]; //Orange Indicator threshold
          defaultSpeed[fanNum] = minSpeed[fanNum]; //One slot on 
          highSpeed[fanNum] = minSpeed[fanNum] + ((maxSpeed[fanNum] - minSpeed[fanNum]) / 2); //Red Indicator threshold
        }
        fanNum += 1;
        if (fanNum < numFans) {
          newMinSpeed = -1;
          newMaxSpeed = -1;
        }
      } else if (not pwmRx) {
        if (b > 0) {
          //Turn on PWM
          pwmOut = true;
        } else {
          pwmOut = false;
        }
      } else {
        fanSupplyVolts = b;
        rangeMultiplier = 255 / fanSupplyVolts;
      }
    }
  }
  for (int i=0; i< numFans; i++) {
    controlFan(i, newSpeed[i], HIGH);
  }
}

void controlFan(int fanNum, byte speed, int blinkState) {
  if (speed == noFanSpeed) {
    int powerInVolts = powerState[fanNum]; 
    //State of slot unknown - use power state to drive fan on or off
    if (powerInVolts > noSlotsOnMinVolts) {
      speed = 0;
    } else if (powerInVolts >= oneSlotOnMinVolts) {
      //One slot is powered
      speed = defaultSpeed[fanNum];
    } else {
      //Both slots powered
      speed = highSpeed[fanNum];
    }
//    if (powerState[fanNum]){
//      speed = defaultSpeed;
//    } else {
//      speed = 0; //turn it off
//    }
  }
  currentSpeed[fanNum] = speed;
  //Enable LM293 output for fan pair
  if (currentSpeed[0] > 0 || currentSpeed[1] > 0) {
    digitalWrite(enableDriverPair1Pin, HIGH);
  } else {
    digitalWrite(enableDriverPair1Pin, LOW);
  }
  if (currentSpeed[2] > 0 || currentSpeed[3] > 0) {
    digitalWrite(enableDriverPair2Pin, HIGH);
  } else {
    digitalWrite(enableDriverPair2Pin, LOW);
  }
  int outputVoltage = 0;
  if (pwmOut) {
    //Set output voltage based on required speed
    outputVoltage = speed*rangeMultiplier;
    if (outputVoltage >= 240) {
      //Set fully on
      outputVoltage = 255;
    }
  } else {
    //Drive fan either fully on or fully off
    if (speed > 0) {
      outputVoltage = 255;
    } else {
      outputVoltage = 0;
    }
  }
  if (fanNum != 1) {
    analogWrite(fanDriverPins[fanNum], outputVoltage);
  } else {
    analogWrite(fanDriverPins[1], 128);
  }
  setSpeedIndicator(fanNum, speed, blinkState);
}

void setSpeedIndicator(int fanNum, byte speed, int blinkState) {
  //Change colour of indicator LED to match speed setting
  //Green - a slot is powered but fan is off
  //Amber - fan is on but only at medium speed
  //Red - fan is at high speed
  if (speed == 0) {
    int powerInVolts = powerState[fanNum]; 
    if (powerInVolts < noSlotsOnMinVolts) {
      //Show Green led on if at least one of the slots is powered, to show that state is recognised
      digitalWrite(speedIndicatorGreenPins[fanNum], blinkState);
    } else {
      //All off
      digitalWrite(speedIndicatorGreenPins[fanNum], LOW);
    }
    digitalWrite(speedIndicatorRedPins[fanNum], LOW);
  } else if (speed < mediumSpeed[fanNum]) {
    //Green
    digitalWrite(speedIndicatorGreenPins[fanNum], blinkState);
    digitalWrite(speedIndicatorRedPins[fanNum], LOW);
  } else if (speed < highSpeed[fanNum]) {
    //Amber
    digitalWrite(speedIndicatorGreenPins[fanNum], blinkState);
    digitalWrite(speedIndicatorRedPins[fanNum], blinkState);
  } else {
    //Red
    digitalWrite(speedIndicatorGreenPins[fanNum], LOW);
    digitalWrite(speedIndicatorRedPins[fanNum], blinkState);
  }
}
