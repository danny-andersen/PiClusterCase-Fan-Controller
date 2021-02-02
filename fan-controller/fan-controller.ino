//#include <avr.h>
#include <Wire.h>

// I/O constants
const int PINS_FAN_DRIVER[4] = {3, 9, 10, 11};
const int PINS_INDICATOR_LED_GREEN[4] = { 0, 2, 7, 12 };
const int PINS_INDICATOR_LED_RED[4] = { 1, 4, 8, 13 };
const int PIN_ENABLE_DRIVER_PAIR_1 = 5;
const int PIN_ENABLE_DRIVER_PAIR_2 = 6;
const int PINS_POWER_STATE_IN[4] = { A0, A1, A2, A3};
const int NO_SLOT_ON_VOLTS_MIN = 593; //If measured power state voltage > 2.9 no slots are on
const int ONE_SLOT_ON_VOLTS_MIN = 350; //If measured power state voltage > 1.7V and <= 2.9V only one slot is on

// Message constants
const int I2C_CHANNEL = 0x10;
const byte START_MSG = 240;
const byte END_MSG = 250;
const byte NO_FAN_SPEED = 255;
const byte MSG_LEN = 17; //bytes in message
const byte DATA_LEN = 14; //data length
const byte DATA_BYTES_PER_FAN = 3; //3 data bytes per fan
const byte CRC7_POLY = 0x91;
const int NUM_FANS = 4;
const int MSG_TIMEOUT = 150;  // If no message rx 15 seconds then use power detection to drive fan on or off
const int COMMS_HUNG_TIMEOUT = 600;  // If no message rx for 60 seconds then allow watchdog to time out and do a reset 
const int BLINK_RATE = 5; // Number of DELAY_TIMEs to turn indicator LED on / off if no fan speed set for fan
const int DELAY_TIME = 100; // loop every 100ms

int currentSpeed[4] = {0, 0, 0, 0};
int powerState[4] = {0, 0, 0, 0};

byte minSpeed[4] = {60, 60, 60, 40}; //These are the defaults
byte mediumSpeed[4] = {0, 0, 0, 0}; //Orange Indicator threshold
byte highSpeed[4] = {0,0,0,0}; //Red Indicator threshold
byte maxSpeed[4] = {100, 100, 100, 120};
byte fanSupplyVolts = 120;
float rangeMultiplier = 255.0 / fanSupplyVolts;
int timeSinceMsg = MSG_TIMEOUT; //Start as though no received a message
int msgRx = LOW;
bool pwmOut = true; //Whether to drive output speed as a PWM signal or not.
int blinkCnt = 0; //Used to flash indicator LED if not using temp input but just using power sensing
int blinkState = HIGH; //Initial state for indicator LED

void setup() {
  for (int i = 0; i < NUM_FANS; i++) {
    pinMode(PINS_INDICATOR_LED_GREEN[i], OUTPUT);
    pinMode(PINS_INDICATOR_LED_RED[i], OUTPUT);
  }
  pinMode(PIN_ENABLE_DRIVER_PAIR_1, OUTPUT);
  pinMode(PIN_ENABLE_DRIVER_PAIR_2, OUTPUT);
  //Set everything to off
  digitalWrite(PIN_ENABLE_DRIVER_PAIR_1, LOW);
  digitalWrite(PIN_ENABLE_DRIVER_PAIR_2, LOW);
  for (int fanNum = 0; fanNum < NUM_FANS; fanNum++) {
    controlFan(fanNum, 0, LOW);
    mediumSpeed[fanNum] = minSpeed[fanNum]; //Orange Indicator threshold
    highSpeed[fanNum] = minSpeed[fanNum] + ((maxSpeed[fanNum] - minSpeed[fanNum]) / 2); //Red Indicator threshold
  }
  Wire.begin(I2C_CHANNEL);            // join i2c bus with address #0x10
  Wire.onReceive(receiveMessage); // register receiver
  //Debug I2C bus
//  pinMode(13, OUTPUT);
//  digitalWrite(13, LOW);
  setWatchDog(); //Two second watchdog timeout
}

void loop() {
  delay(DELAY_TIME); //ms
  timeSinceMsg++;
  for (int i=0; i<NUM_FANS; i++) {
    powerState[i] = analogRead(PINS_POWER_STATE_IN[i]);
  }
  if (timeSinceMsg >= MSG_TIMEOUT) {
    //No control message received recently - blink indicator LED and send no fan speed
    //This will drive the fan according to the power status
    blinkCnt ++;
    if (blinkCnt >= BLINK_RATE) {
      blinkState = not blinkState;
      blinkCnt = 0;
    } 
    for (int i=0; i<NUM_FANS; i++) {
      controlFan(i, NO_FAN_SPEED, blinkState);
    }
  } else {
    blinkCnt = 0;
  }
  if (timeSinceMsg >= COMMS_HUNG_TIMEOUT) {
    //Not received any comms for an extended period
    //This could be because I2C Wire comms has hung 
    //So force a reset
    softwareReset();
  }
  //Kick the watchdog so that we dont get reset
  kickWatchDog(); 
}

//Process any message received
//Message format: {0x55, minSpeed1, maxSpeed1, speed1, minSpeed2, maxSpeed2, speed2, 
// minSpeed3, maxSpeed3, speed3, minSpeed4, maxSpeed4, speed4, pwmOn, fanSupplyVolts, 0xaa}
//Speed Range: 0 = off, 50 = full speed, 0xff = status not known
void receiveMessage(int numBytes) {
  bool validMsg = true;
  byte data[DATA_LEN]; //14 bytes of data
  int dataCnt = 0;
  bool start = false;
  bool crcRx = false;
  while (Wire.available()) {
    // loop through all bytes
    byte b = Wire.read(); // receive byte
    if (!start) {
      if (b == START_MSG) {
        start = true;
      }
    } else if (dataCnt < DATA_LEN) {
      data[dataCnt++] = b;
    } else if (not crcRx) {
      //Check crc
      byte crc = getCRC(data, DATA_LEN);
      if (b != crc) {
        validMsg = false;
      }
      crcRx = true;
    } else if (b != END_MSG) {
        validMsg = false;
    }
  }
  if (validMsg) {
    int newSpeed[4] = {0, 0, 0, 0};
    int offset = 0;
    timeSinceMsg = 0;
    for (int fanNum = 0; fanNum < NUM_FANS; fanNum++) {
      offset = fanNum * DATA_BYTES_PER_FAN; //3 data bytes per fan
      int newMinSpeed = data[offset++];
      int newMaxSpeed = data[offset++];
      newSpeed[fanNum] = data[offset++];
      if (newMinSpeed != minSpeed[fanNum] || newMaxSpeed != maxSpeed[fanNum]) {
        minSpeed[fanNum] = newMinSpeed;
        maxSpeed[fanNum] = newMaxSpeed;
        mediumSpeed[fanNum] = minSpeed[fanNum]; //Orange Indicator threshold
        highSpeed[fanNum] = minSpeed[fanNum] + ((maxSpeed[fanNum] - minSpeed[fanNum]) / 2); //Red Indicator threshold
        //Check if speed being set is greater than the max speed
        //Note: but only if the new speed is not the NO_FAN_SPEED indicator
        if (newSpeed[fanNum] != NO_FAN_SPEED && newSpeed[fanNum] > maxSpeed[fanNum]) {
          newSpeed[fanNum] = maxSpeed[fanNum];
        }
      }
      offset = NUM_FANS * DATA_BYTES_PER_FAN;
      if (data[offset++] > 0) {
        //Turn on PWM
        pwmOut = true;
      } else {
        pwmOut = false;
      }
      fanSupplyVolts = data[offset++];
      rangeMultiplier = 255.0 / fanSupplyVolts;
    }
    //Set new speed
    for (int i=0; i< NUM_FANS; i++) {
      controlFan(i, newSpeed[i], HIGH);
    }
  }
}

byte getCRC(byte message[], byte length) {
  byte crc = 0;
 
  for (int i = 0; i < length; i++) {
    crc ^= message[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc ^= CRC7_POLY;
      crc >>= 1;
    }
  }
  return crc;
}

void controlFan(int fanNum, byte speed, int blinkState) {
  if (speed == NO_FAN_SPEED) {
    int powerInVolts = powerState[fanNum]; 
    //State of slot unknown - use power state to drive fan on or off
    if (powerInVolts > NO_SLOT_ON_VOLTS_MIN) {
      speed = 0;
    } else if (powerInVolts >= ONE_SLOT_ON_VOLTS_MIN) {
      //One slot is powered
      speed = mediumSpeed[fanNum];
    } else {
      //Both slots powered
      // speed = highSpeed[fanNum];
      speed = maxSpeed[fanNum];
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
    digitalWrite(PIN_ENABLE_DRIVER_PAIR_1, HIGH);
  } else {
    digitalWrite(PIN_ENABLE_DRIVER_PAIR_1, LOW);
  }
  if (currentSpeed[2] > 0 || currentSpeed[3] > 0) {
    digitalWrite(PIN_ENABLE_DRIVER_PAIR_2, HIGH);
  } else {
    digitalWrite(PIN_ENABLE_DRIVER_PAIR_2, LOW);
  }
  int outputVoltage = 0;
  if (pwmOut) {
    //Set output voltage based on required speed
    outputVoltage = byte(speed*rangeMultiplier);
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
  // if (fanNum != 1) {
    analogWrite(PINS_FAN_DRIVER[fanNum], outputVoltage);
  // } else {
  //   analogWrite(fanDriverPins[1], 128);
  // }
  setSpeedIndicator(fanNum, speed, blinkState);
}

void setSpeedIndicator(int fanNum, byte speed, int blinkState) {
  //Change colour of indicator LED to match speed setting
  //Green - a slot is powered but fan is off
  //Amber - fan is on but only at medium speed
  //Red - fan is at high speed
  if (speed == 0) {
    int powerInVolts = powerState[fanNum]; 
    if (powerInVolts < NO_SLOT_ON_VOLTS_MIN) {
      //Show Green led on if at least one of the slots is powered, to show that state is recognised
      digitalWrite(PINS_INDICATOR_LED_GREEN[fanNum], blinkState);
    } else {
      //All off
      digitalWrite(PINS_INDICATOR_LED_GREEN[fanNum], LOW);
    }
    digitalWrite(PINS_INDICATOR_LED_RED[fanNum], LOW);
  } else if (speed < mediumSpeed[fanNum]) {
    //Green
    digitalWrite(PINS_INDICATOR_LED_GREEN[fanNum], blinkState);
    digitalWrite(PINS_INDICATOR_LED_RED[fanNum], LOW);
  } else if (speed < highSpeed[fanNum]) {
    //Amber
    digitalWrite(PINS_INDICATOR_LED_GREEN[fanNum], blinkState);
    digitalWrite(PINS_INDICATOR_LED_RED[fanNum], blinkState);
  } else {
    //Red
    digitalWrite(PINS_INDICATOR_LED_GREEN[fanNum], LOW);
    digitalWrite(PINS_INDICATOR_LED_RED[fanNum], blinkState);
  }
}

void setWatchDog() {
  // enable watchdog 
  //Ideally this would use the in-built watchdog
  //but am using an old boot loader which causes and endless reset on timeout
  // wdt_enable(WDTO_2S);
}

void kickWatchDog() {
  // reset watchdog with the provided prescaller
  // wdt_reset();
}

void(* resetFunc) (void) = 0; //declare reset function @ address 0

void softwareReset() {
  // force watchdog timeout by setting short time
  // wdt_enable(WDTO_30MS);
  // wait for the prescaller time to expire
  // while(1) {}
  //As watchdog timeout causes an endless reset due to the old bootloader not resetting the watchdog
  //use the simple function that restarts the code.
  resetFunc();  //call reset
}