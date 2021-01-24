#include <Wire.h>

int i2cChannel = 0x10;

int numFans = 4;
int fanDriverPins[4] = {3, 9, 10, 11};
int speedIndicatorGreenPins[4] = { 0, 2, 7, 12 };
int speedIndicatorRedPins[4] = { 1, 4, 8, 13 };
int enableDriverPair1Pin = 5;
int enableDriverPair2Pin = 6;
//int powerStatePins[4] = { 14, 15, 16, 17};
int powerStatePins[4] = { A0, A1, A2, A3};

int currentSpeed[4] = {0, 0, 0, 0};
int powerState[4] = {0, 0, 0, 0};
int noSlotsOnMinVolts = 593; //If measured power state voltage > 2.9 no slots are on
int oneSlotOnMinVolts = 350; //If measured power state voltage > 1.7V and <= 2.9V only one slot is on
byte startOfMsg = 0x55;
byte endOfMsg = 0xAA;
byte noFanSpeed = 0xFF;
byte mediumSpeed = 17; //Orange Indicator threshold
byte defaultSpeed = 25; //One slot on 
byte highSpeed = 35; //Red Indicator threshold
byte maxSpeed = 50;
byte rangeMulitplier = 5; //Output is 5xrange (max value in 50, 250 output)
int timeSinceMsg = 0;
int delayTime = 50; // loop every 50ms
int messageTimeout = 15000;  // If no message of 15 seconds then use power detection to drive fan on or off
int msgRx = LOW;

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
    controlFan(i, 0);
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
    timeSinceMsg = messageTimeout; //Dont increment timeout further to stop it rolling over
    //No control message received recently - send no fan speed
    //This will drive the fan according to the power status
    for (int i=0; i<numFans; i++) {
      controlFan(i, noFanSpeed);
    }
  }
}

//Process any message received
//Message format: {0x55, speed1, speed2, speed3, speed4, 0xaa}
//Speed Range: 0 = off, 50 = full speed, 0xff = status not known
void receiveMessage(int numBytes) {
  int fanNum = 0;
  bool started = false;
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
      if (b == endOfMsg) {
        started = false;
        //Comment this out - just used for tx debug
//        digitalWrite(13, LOW);
      } else {
        if (started && fanNum < numFans) {
          //Only process if message has started
          controlFan(fanNum, b);
        }
        fanNum += 1;
      }
    }
  }
}

void controlFan(int fanNum, byte speed) {
  if (speed == noFanSpeed) {
    int powerInVolts = powerState[fanNum]; 
    //State of slot unknown - use power state to drive fan on or off
    if (powerInVolts > noSlotsOnMinVolts) {
      speed = 0;
    } else if (powerInVolts >= oneSlotOnMinVolts) {
      //One slot is powered
      speed = defaultSpeed;
    } else {
      //Both slots powered
      speed = maxSpeed;
    }
//    if (powerState[fanNum]){
//      speed = defaultSpeed;
//    } else {
//      speed = 0; //turn it off
//    }
  }
  //Send speed to PWM output for fan
  currentSpeed[fanNum] = speed;
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
  int outputVoltage = speed*rangeMulitplier;
  if (outputVoltage >= 245) {
    //Set fully on
    outputVoltage = 255;
  }
  analogWrite(fanDriverPins[fanNum], outputVoltage);
  setSpeedIndicator(fanNum, speed);
}

void setSpeedIndicator(int fanNum, byte speed) {
  //Change colour of indicator LED to match speed setting
  if (speed == 0) {
    int powerInVolts = powerState[fanNum]; 
    if (powerInVolts < noSlotsOnMinVolts) {
      //Show Green led on if at least one of the slots is powered, to show that state is recognised
      digitalWrite(speedIndicatorGreenPins[fanNum], HIGH);
    } else {
      //All off
      digitalWrite(speedIndicatorGreenPins[fanNum], LOW);
    }
    digitalWrite(speedIndicatorRedPins[fanNum], LOW);
  } else if (speed < mediumSpeed) {
    //Green
    digitalWrite(speedIndicatorGreenPins[fanNum], HIGH);
    digitalWrite(speedIndicatorRedPins[fanNum], LOW);
  } else if (speed < highSpeed) {
    //Amber
    digitalWrite(speedIndicatorGreenPins[fanNum], HIGH);
    digitalWrite(speedIndicatorRedPins[fanNum], HIGH);
  } else {
    //Red
    digitalWrite(speedIndicatorGreenPins[fanNum], LOW);
    digitalWrite(speedIndicatorRedPins[fanNum], HIGH);
  }
}