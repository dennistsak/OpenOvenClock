#include <LedControl.h>
#include <DS3231.h>
#include <Switch.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/wdt.h>

#define SS_PIN 10
#define MOSI_PIN 11
#define SCK_PIN 13

#define ENCODER_MASK 0b00001100


/* 7-Segment Display */
LedControl lc = LedControl(MOSI_PIN, SCK_PIN, SS_PIN, 1);

unsigned long delaytime = 500;

int load_anim_rows[] = {0,1,2,3,3,3,3,2,1,0,0,0};
int load_anim_col[] = {1,1,1,1,2,3,4,4,4,4,5,6};
void settingLabel(int);
void settingValue(int);
uint32_t getTimeSec(byte, byte, byte);
bool timePassed(uint32_t, uint32_t);
void beep();
void doubleBeep();
void beepError();
void alertShort();
void alertLong();
void lockOven();
void unlockOven();
void factoryReset();

/* Real Time Clock */
DS3231 rtc;
bool h12Flag;
bool pmFlag;
byte HOUR, MINUTE, SECOND, TIMER_H, TIMER_M, TIMER_S;
uint32_t targetSec;
uint32_t nowSec;
uint32_t remaining;

/* Rotary encoder */
int pinA = 2;
int pinB = 3;
volatile byte flag1 = 0;
volatile byte flag2 = 0;
volatile byte encPosition = 0;
volatile byte oldEncPos = 0;
volatile byte regVals = 0;
volatile unsigned long debounce = 200;
volatile unsigned long long_press = 3000;
Switch encButton = Switch(4, INPUT, false, 50, 2500, 250, 10);

/* Relay */
int relayPin = 5;
volatile bool heaterOn = false;

/* Buzzer */
int buzzerPin = 7;

/* FSM variables */
unsigned int edit_time_state = 0;
unsigned long edit_time_stamp = 0;
bool edit_time_vis = true;

unsigned long refresh_delay = 0;
bool disp_time_vis = true;
unsigned long anim_d = 0;
unsigned int anim_i = 0;
unsigned long anim_j = 0;
unsigned long ovenUnlockMillis = 0;
const unsigned long ovenMaxUnlockTime = 10UL * 60UL * 60UL * 1000UL; // 10h
const unsigned long ecoStartTime = 20UL * 3600;
const unsigned long ecoStopTime = 6UL * 3600;
// const unsigned long ovenMaxUnlockTime = 10000UL; // 10s
bool locked = true;

/* settings variables */
int setting_i = 0;
bool edit_setting = false;
byte brightness = 1;
bool eco_mode = false;
bool silent_mode = false;

/* EEPROM addresses */
#define E_INIT     10 /* initialization check address */
#define BRIGHTNESS 0
#define ECO        1
#define SILENT     2

unsigned int state = 0;

void setup() {
  /* IMPORTANT! DO NOT CHANGE! */
  pinMode(SS_PIN, OUTPUT);  // PB2 / SS must be OUTPUT
  digitalWrite(SS_PIN, HIGH);
  /*---------------------------*/

  /* Enable watchdog timer */
  wdt_enable(WDTO_8S);

  /* Setup relay pin */
  pinMode(relayPin, OUTPUT);
  heaterOn = false;

  /* Setup buzzer pin */
  pinMode(buzzerPin, OUTPUT);

  /* Start serial */
  Serial.begin(9600);

  /* Check EEPROM state */
  if (EEPROM.read(E_INIT) != 'I') {
    Serial.println("EEPROM not initialized!");
    EEPROM.write(BRIGHTNESS, brightness);
    EEPROM.write(ECO, eco_mode);
    EEPROM.write(SILENT, silent_mode);
    EEPROM.write(E_INIT, 'I');
    Serial.println("EEPROM initialized Successfully!");
  }

  /* Load settings from EEPROM */
  brightness = EEPROM.read(BRIGHTNESS);
  eco_mode = EEPROM.read(ECO);
  silent_mode = EEPROM.read(SILENT);
  Serial.println("-------- Settings --------");
  Serial.print("Brighness: ");
  Serial.println(brightness, DEC);
  Serial.print("Eco: ");
  Serial.println(eco_mode);
  Serial.print("Silent: ");
  Serial.println(silent_mode);
  Serial.println("--------------------------");

  /* 7-Segment display test animation */
  lc.shutdown(0, false);
  lc.setIntensity(0, 1);
  lc.clearDisplay(0);
  for (int i=0; i<12; i++) {
    lc.clearDisplay(0);
    lc.setLed(0, load_anim_rows[i], load_anim_col[i], true);
    if (!silent_mode) {
      tone(buzzerPin, 8000);
    }
    delay(25);
    noTone(buzzerPin);
    delay(25);
  }
  lc.clearDisplay(0);

  /* Start I2C communication for RTC */
  Wire.begin();

  /* Initial FSM state */
  state = 0;
  edit_time_state = 0;

  /* Encoder pin interrupt initialization */
  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pinA), pinARoutine, RISING);
  attachInterrupt(digitalPinToInterrupt(pinB), pinBRoutine, RISING);

  beep();
}


int value1 = 0;
int value2 = 0;


void pinARoutine(){
  regVals = PIND & ENCODER_MASK;
  if (regVals == B00001100 && flag1) {
    flag2 = 0;
    flag1 = 0;
    encPosition--;
  } else if (regVals == B00000100) {
    flag2 = 1;
  }
}

void pinBRoutine(){
  regVals = PIND & ENCODER_MASK;
  if (regVals == 0b00001100 && flag2) {
    flag2 = 0;
    flag1 = 0;
    encPosition++;
  } else if (regVals == 0b00001000) {
    flag1 = 1;
  }
}

void loop() {
  /* Feed the watchdog. */
  wdt_reset();

  /* Update heater relay logic */
  if (heaterOn && !locked) {
    digitalWrite(relayPin, HIGH);
  } else {
    digitalWrite(relayPin, LOW);
  }

  /* Lock oven automatically if it's been 10h */
  if (millis() - ovenUnlockMillis > ovenMaxUnlockTime && !locked) {
    lockOven();
  }

  /* FSM */
  switch(state)
  {
    case 0: // ---------- SET TIME -------------
    {
      if (millis() - edit_time_stamp > 300) { 
        edit_time_vis = !edit_time_vis;
        edit_time_stamp = millis();

        if (edit_time_state == 0)
        {
          lc.clearDisplay(0);
          lc.setIntensity(0, brightness);
          if (edit_time_vis) {
            lc.setDigit(0, 0, HOUR/10, false);
            lc.setDigit(0, 1, HOUR%10, false);
          }
          lc.setDigit(0, 2, MINUTE/10, false);
          lc.setDigit(0, 3, MINUTE%10, false);
        } 
        if (edit_time_state == 1)
        {
          lc.clearDisplay(0);
          lc.setIntensity(0, brightness);
          lc.setDigit(0, 0, HOUR/10, false);
          lc.setDigit(0, 1, HOUR%10, false);
          if (edit_time_vis) {
            lc.setDigit(0, 2, MINUTE/10, false);
            lc.setDigit(0, 3, MINUTE%10, false);
          }
        }
        lc.setLed(0, 1, 0, true);
        lc.setLed(0, 3, 0, true);
      }
      break;
    }
    case 1: // ---------- DISPLAY TIME ----------
    {
      if (millis() - refresh_delay > 1000) {
        disp_time_vis = !disp_time_vis;
        refresh_delay = millis();
        byte hour = rtc.getHour(h12Flag, pmFlag);
        byte min = rtc.getMinute();
        byte dig01, dig23;

        dig01 = hour;
        dig23 = min;

        lc.clearDisplay(0);
        
        /* Eco mode brightness logic */
        if (eco_mode) {
          nowSec = getTimeSec(hour, min, 0); /* Get currect seconds with only hour and minute accuracy. */
          if (nowSec >= ecoStartTime || nowSec < ecoStopTime) {
            lc.setIntensity(0, 0);
          } else {
            lc.setIntensity(0, brightness);
          }
        } else {
          lc.setIntensity(0, brightness);
        }

        if (dig01 > 9) {
          lc.setDigit(0, 0, dig01/10, false);
        }
        lc.setDigit(0, 1, dig01%10, false);
        lc.setDigit(0, 2, dig23/10, false);
        lc.setDigit(0, 3, dig23%10, false);
        if (disp_time_vis) {
          lc.setLed(0, 1, 0, true);
          lc.setLed(0, 3, 0, true);
        }

        if (!locked) {
          heaterOn = true;
        } else {
          heaterOn = false;
        }
      }
      break;
    }
    case 2: // ---------- SET TIMER SCREEN ----------
    {
      if (millis() - edit_time_stamp > 300) { 
        edit_time_vis = !edit_time_vis;
        edit_time_stamp = millis();

        if (edit_time_state == 0)
        {
          lc.clearDisplay(0);
          lc.setIntensity(0, brightness);
          if (edit_time_vis) {
            byte h0 = TIMER_H/10;
            byte h1 = TIMER_H%10;
            if (h0 != 0) { lc.setDigit(0, 0, h0, false); }
            if (h1 != 0) { lc.setDigit(0, 1, h1, false); }
          } else {
            lc.setLed(0, 0, 7, true);
            lc.setLed(0, 1, 7, true);
          }
          lc.setDigit(0, 2, TIMER_M/10, false);
          lc.setDigit(0, 3, TIMER_M%10, false);
        } 
        if (edit_time_state == 1)
        {
          lc.clearDisplay(0);
          lc.setIntensity(0, brightness);
          lc.setDigit(0, 0, TIMER_H/10, false);
          lc.setDigit(0, 1, TIMER_H%10, false);
          if (edit_time_vis) {
            byte m0 = TIMER_M/10;
            byte m1 = TIMER_M%10;
            lc.setDigit(0, 2, m0, false);
            lc.setDigit(0, 3, m1, false);
          } else {
            lc.setLed(0, 2, 7, true);
            lc.setLed(0, 3, 7, true);
          }
        }
        lc.setLed(0, 1, 0, true);
        lc.setLed(0, 3, 0, true);
      }
      break;
    }
    case 3: // ---------- TIMER COUNTDOWN ----------
    {
      if (timePassed(nowSec, targetSec)) {
        state = 5;
        alertLong();
      }
      if (millis() - refresh_delay > 1000) {
        refresh_delay = millis();

        // Get remaining seconds
        byte nowHour = rtc.getHour(h12Flag, pmFlag);
        byte nowMinute = rtc.getMinute();
        byte nowSecond = rtc.getSecond();
        nowSec = getTimeSec(nowHour, nowMinute, nowSecond);
        remaining = (targetSec + 86400UL - nowSec) % 86400UL;
        byte hour = remaining / 3600;
        byte min = (remaining % 3600) / 60;
        byte sec = remaining % 60;
        byte dig01, dig23;

        if (remaining < 3600) {
          dig01 = min;
          dig23 = sec;
        } else {
          dig01 = hour;
          dig23 = min;
        }

        lc.clearDisplay(0);
        lc.setIntensity(0, brightness);
        if (dig01 > 9) {
          lc.setDigit(0, 0, dig01/10, false);
        }
        lc.setDigit(0, 1, dig01%10, false);
        lc.setDigit(0, 2, dig23/10, false);
        lc.setDigit(0, 3, dig23%10, false);
        lc.setLed(0, 1, 0, true);
        lc.setLed(0, 3, 0, true);
      }
      if (millis() - anim_d > 50 && disp_time_vis) {
        if (remaining > 3600 || remaining < 600) {
          anim_d = millis();
          anim_i++;
          if(anim_i > 6) { anim_i = 1; }
          for (int i=0; i<7; i++) {
            lc.setLed(0, 0, i, false);
          }
          lc.setLed(0, 0, anim_i, true);
        }
      }
      break;
    }
    case 4: // ---------- SETTINGS MENU ----------
    {
      if (millis() - edit_time_stamp > 200) {
        edit_time_vis = !edit_time_vis;
        edit_time_stamp = millis();
        lc.clearDisplay(0);
        lc.setIntensity(0, brightness);
        settingLabel(setting_i);
        if (!edit_setting) {
          settingValue(setting_i); 
        } else { 
          if (edit_time_vis) {
            settingValue(setting_i);
          }
        }
      }
      break;
    }
    case 5: // ---------- OVEN HALT --------------
    {
      if (millis() - refresh_delay > 50) {
        refresh_delay = millis();
        heaterOn = false;
        lc.clearDisplay(0);
        lc.setIntensity(0, brightness);
        lc.setChar(0, 0, 'e', false);
        lc.setChar(0, 1, 'n', false);
        lc.setChar(0, 2, 'd', false);
      }
      break;
    }
    default:
      Serial.println("Invalid FSM state (main).");
      break;
  }

  /* ----- Handle encoder position change ----- */
  if(oldEncPos != encPosition) {
    Serial.println(encPosition);
    int direction = 0;
    if (encPosition < oldEncPos || (encPosition == 255 && oldEncPos == 0)) { direction = 1; }
    if (encPosition == 0 && oldEncPos == 255) { direction = 0; }
    switch(state)
    {
      case 0: // ---------- SET TIME -------------
      {
        if (direction == 0)
        {
          if (edit_time_state == 0) {
            if (HOUR < 23) { HOUR++; }
            else { HOUR = 0; }
          }
          if (edit_time_state == 1) {
            if (MINUTE < 59) { MINUTE++; }
            else { MINUTE = 0; }
          }
        }
        else if (direction == 1)
        {
          if (edit_time_state == 0) {
            if (HOUR > 0) { HOUR--; }
            else { HOUR = 23; }
          }
          if (edit_time_state == 1) {
            if (MINUTE > 0) { MINUTE--; }
            else { MINUTE = 59; }
          }
        }
        beep();
        break;
      }
      case 1: // ---------- DISPLAY TIME ----------
      {
        beepError();
        if (locked) { showLockscreen(); }
        break;
      }
      case 2: // ---------- SET TIMER SCREEN ----------
      {
        if (direction == 0)
        {
          if (edit_time_state == 0) {
            if (TIMER_H < 9) { TIMER_H++; }
            //else { TIMER_H = 0; }
          }
          if (edit_time_state == 1) {
            if (TIMER_M < 59) { TIMER_M++; }
            else { TIMER_M = 0; }
          }
        }
        else if (direction == 1)
        {
          if (edit_time_state == 0) {
            if (TIMER_H > 0) { TIMER_H--; }
            //else { TIMER_H = 6; }
          }
          if (edit_time_state == 1) {
            if (TIMER_M > 0) { TIMER_M--; }
            else { TIMER_M = 59; }
          }
        }
        beep();
        break;
      }
      case 3: // ---------- TIMER COUNTDOWN ----------
      {
        beepError();
        break;
      }
      case 4: // ---------- SETTINGS MENU ----------
      {
        if (edit_setting)
        {
          switch(setting_i)
          {
            case 0:
            {
              if (direction == 0)
              {
                if (brightness < 15) { brightness++; }
              }
              else if (direction == 1)
              {
                if (brightness > 0) { brightness--; }
              }
              break;
            }
            case 1:
            {
              eco_mode = !eco_mode;
              break;
            }
            case 2:
            {
              silent_mode = !silent_mode;
              break;
            }
            case 3:
            {
              break;
            }
            default:
              break;
          }
        } else {
          if (direction == 0)
          {
            setting_i++;
            if (setting_i > 3) { setting_i = 0; }
          }
          else if (direction == 1)
          {
            setting_i--;
            if (setting_i < 0) { setting_i = 3; }
          }
        }
        beep();
        break;
      }
      case 5: // ---------- OVEN HALT --------------
      {
        state = 1;
        lockOven();
        beep();
        break;
      }
      default:
        Serial.println("Invalid FSM state (encoder turn).");
        break;
    }
    oldEncPos = encPosition;
  }

  encButton.poll();
  /* ----- Handle encoder button short press ----- */
  if (encButton.singleClick()) {
    Serial.println("Short press.");
    switch(state)
    {
      case 0: // ---------- SET TIME -------------
      {
        if (edit_time_state == 0) {
          edit_time_state = 1;
          rtc.setHour(HOUR);
        } else {
          rtc.setMinute(MINUTE);
          rtc.setSecond(0);
          edit_time_state = 0;
          state = 1;
        }
        beep();
        break;
      }
      case 1: // ---------- DISPLAY TIME ----------
      {
        if (locked) {
          beepError();
          showLockscreen();
          break;  
        }
        edit_time_state = 0;
        TIMER_H = 0;
        TIMER_M = 0;
        TIMER_S = 0;
        state = 2;
        beep();
        break;
      }
      case 2: // ---------- SET TIMER SCREEN ----------
      {
        if (edit_time_state == 0) {
          edit_time_state = 1;
        } else {
          edit_time_state = 0;

          // Calculate duration and start oven
          byte hour = rtc.getHour(h12Flag, pmFlag);
          byte minute = rtc.getMinute();
          byte second = rtc.getSecond();
          uint32_t nowSec = getTimeSec(hour, minute, second);
          uint32_t addSec = getTimeSec(TIMER_H, TIMER_M, 0);
          targetSec = (nowSec + addSec) % 86400UL;

          ovenUnlockMillis = millis();
          heaterOn = true;
          state = 3;
          alertShort();
        }
        beep();
        break;
      }
      case 3: // ---------- TIMER COUNTDOWN ----------
      {
        state = 5;
        beep();
        break;
      }
      case 4: // ---------- SETTINGS MENU ----------
      {
        edit_setting = !edit_setting;
        if (!edit_setting) {
          EEPROM.update(BRIGHTNESS, brightness);
          EEPROM.update(ECO, eco_mode);
          EEPROM.update(SILENT, silent_mode);
          doubleBeep();
        } else {
          beep();
        }
        break;
      }
      case 5: // ---------- OVEN HALT --------------
      {
        state = 1;
        lockOven();
        beep();
        break;
      }
      default:
        Serial.println("Invalid FSM state (short press).");
        break;
    }
  }

  /* ----- Handle encoder button long press ----- */
  if (encButton.longPress()) {
    Serial.println("Long press.");
    switch(state)
    {
      case 0: // ---------- SET TIME -------------
      {
        if (edit_time_state == 1) { edit_time_state = 0; }
        else if (edit_time_state == 0) { state = 1; }
        beep();
        break;
      }
      case 1: // ---------- DISPLAY TIME ----------
      {
        // TODO: TEST LOCK UNLOCK
        if (locked) {
          alertShort();
          unlockOven();
        } else {
          alertShort();
          lockOven();
        }
        break;
      }
      case 2: // ---------- SET TIMER SCREEN ----------
      {
        if (edit_time_state == 1) { edit_time_state = 0; }
        else { state = 1; }
        beep();
        break;
      }
      case 3: // ---------- TIMER COUNTDOWN ----------
      {
        state = 5;
        beep();
        break;
      }
      case 4: // ---------- SETTINGS MENU ----------
      {
        if (setting_i == 3 && edit_setting) {
          factoryReset();
          break;
        }
        state = 1;
        beep();
        break;
      }
      case 5: // ---------- OVEN HALT --------------
      {
        state = 1;
        lockOven();
        beep();
        break;
      }
      default:
        Serial.println("Invalid FSM state (long press).");
        break;
    }
  }

  /* ----- Handle encoder button double press ----- */
  if (encButton.doubleClick() && state == 1) {
    if (!locked) {
      Serial.println("Double press.");
      beep();
      edit_setting = false;
      state = 4;
    } else {
      beepError();
      showLockscreen();
    }
  }
}

uint32_t getTimeSec(byte hour, byte minute, byte second) {  
  return (uint32_t)hour * 3600 + (uint32_t)minute * 60 + (uint32_t)second;
}

bool timePassed(uint32_t now, uint32_t target) {
  return ((now + 86400UL - target) % 86400UL) < 1;
}

void settingLabel(int setting) {
  switch(setting_i)
  {
    case 0:
    {
      lc.setChar(0, 0, 'b', false);
      lc.setLed(0, 1, 3, true);
      lc.setLed(0, 1, 5, true);
      lc.setLed(0, 1, 6, true);
      break;
    }
    case 1:
    {
      lc.setChar(0, 0, 'e', false);
      lc.setChar(0, 1, 'c', false);
      lc.setChar(0, 2, 'o', false);
      break;
    }
    case 2:
    {
      lc.setChar(0, 0, 'b', false);
      lc.setChar(0, 1, 'e', false);
      lc.setChar(0, 2, 'p', false);
      break;
    }
    case 3:
    {
      lc.setChar(0, 0, 'f', false);
      lc.setChar(0, 1, 'a', false);
      lc.setChar(0, 2, 'c', false);
      lc.setLed(0, 3, 4, true);
      lc.setLed(0, 3, 5, true);
      lc.setLed(0, 3, 6, true);
      lc.setLed(0, 3, 7, true);
      break;
    }
    default:
      break;
  }
}

void settingValue(int setting) {
  switch(setting_i)
  {
    case 0:
    {
      if (brightness > 9) {
        lc.setDigit(0, 2, brightness/10, false);
      }
      lc.setDigit(0, 3, brightness%10, false);
      break;
    }
    case 1:
    {
      if (eco_mode) {
        lc.setDigit(0, 3, 1, false);
        break;
      }
      lc.setDigit(0, 3, 0, false);
      break;
    }
    case 2:
    {
      if (silent_mode) {
        lc.setDigit(0, 3, 0, false);
        break;
      }
      lc.setDigit(0, 3, 1, false);
      break;
    }
    case 3:
    {
      lc.setLed(0, 0, 0, true);
      lc.setLed(0, 2, 0, true);
      break;
    }
    default:
      break;
  }
}

void beep() {
  if (silent_mode) { return; }
  tone(buzzerPin, 3750, 25);
}

void doubleBeep() {
  if (silent_mode) { return; }
  tone(buzzerPin, 3750, 25);
  delay(100);
  tone(buzzerPin, 3750, 25);
}

void beepError() {
  tone(buzzerPin, 150, 150);
}

void alertShort() {
  tone(buzzerPin, 4000, 500);
}

void alertLong() {
  tone(buzzerPin, 4000, 3000);
}

void lockOven() {
  locked = true;
  state = 1;
  lc.clearDisplay(0);
  lc.setDigit(0, 1, 0, false);
  delay(100);
  lc.setLed(0, 1, 0, true);
  lc.setLed(0, 3, 0, true);
  delay(100);
  lc.setLed(0, 2, 7, true);
  delay(100);
  lc.setLed(0, 3, 7, true);
  delay(100);
  lc.setLed(0, 3, 3, true);
  lc.setLed(0, 3, 5, true);
  delay(1000);
}

void unlockOven() {
  locked = false;
  ovenUnlockMillis = millis();
  showLockscreen();
  delay(100);
  lc.setLed(0, 3, 3, false);
  lc.setLed(0, 3, 5, false);
  delay(100);
  lc.setLed(0, 3, 7, false);
  delay(100);
  lc.setLed(0, 2, 7, false);
  delay(100);
  lc.setLed(0, 3, 0, false);
  lc.setDigit(0, 1, 0, false);
  delay(100);
  lc.clearDisplay(0);
  delay(250);
}
 
void showLockscreen() {
  lc.clearDisplay(0);
  lc.setDigit(0, 1, 0, true);
  lc.setLed(0, 3, 0, true);
  lc.setLed(0, 2, 7, true);
  lc.setLed(0, 3, 7, true);
  lc.setLed(0, 3, 3, true);
  lc.setLed(0, 3, 5, true);
  delay(1000);
}

void factoryReset() {
  EEPROM.write(E_INIT, 'F');
  wdt_disable();
  wdt_enable(WDTO_2S);
  delay(5000);
}
