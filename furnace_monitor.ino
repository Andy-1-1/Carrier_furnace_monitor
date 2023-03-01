/* Furnace Status Light Monitor
by Andy Fraser, 1 March 2023

An Arduino can be used to monitor the status lamp on a machine (a Carrier 59TP6B
furnace in this case), and report when a fault happens and what kind of fault
happened, by decoding the fault code flashed by that status lamp.

The project is described in my article on Medium at the link below.  If you're not
a Medium subscriber you can read three articles a month for free, but it's
worth joining!  If you choose a paid instead of free membership, please use my referral
link at the end of the article, I'll get a portion of your membership fee, which 
I then spend on hardware and parts for more projects :)

https://medium.com/@andy.fraser/monitor-your-furnace-with-an-arduino-27175600e12a

Caution: this is for watching, decoding, and logging the status light on a furnace.
Do not try and fix or adjust the furnace.  Do not open it to observe or measure 
more details. For example, if you open it and don't seal it again properly, or try
and measure airflow of the intake or exhaust and the setup fails and obstructs it,
you risk carbon monoxide poisoning, which often leads to death.  
Observe and measure from the outside only :)
*/

// includes for the MPL1152 baro+temp sensor, RTC, and OLED
#include <Wire.h>
#include <Adafruit_MPL115A2.h>
#include <SeeedOLED.h>
#include "RTClib.h"

RTC_DS1307 rtc;
Adafruit_MPL115A2 mpl115a2;

// define the pins
#define FLIGHT  7 // furnace lamp sensor input
#define FCLR    6 // pushbutton input to clear FFAIL output, pullup mode
#define FFAIL   5 // furnace error-has-occurred LED output
#define FLED    4 // furnace lamp status LED output

#define TONEPIN 8 // pwm output pin as audio replica of furnace lamp

// short flash is usually 250 mS, long flash is usually 1000 mS
#define FLASHMIN 50 // minimum flash time to filter out noise
#define FLASHSHORT 400 // typ 250, if less than this then it's short
#define FLASHLONG  800 // typ 1000, if more than this then it's long
#define FLASHPAUSE 1500 // not measured yet, off pause time between codes

// time variables, in mS
unsigned long offAtTime = 0; // last time the furnace lamp turned off
unsigned long onAtTime = 0; // last time the furnace lamp turned on
unsigned long offDuration = 0; // duration of last lamp off time
unsigned long onDuration = 0; // duration of last lamp on time
unsigned long lastPrintTime = 0; // mS since last print to serial port
unsigned long isrEntryTime = 0; // time the lamp changed

float pressureKPA = 0, temperatureC = 0;    

// a bunch of global variables needed within the ISR and in the main loop
int numShorts = 0; // number of short flashes
int numLongs = 0;  // number of long flashes
int furnaceCode = 0; // two digit dec value for code
int lightOn  =0; // light went on flag
int lightOff =0; // light went off flag
int newCode =0; // new error code
int offGlitches =0; // count of imperfect lamp transitions to off
int onGlitches =0; // count of imperfect lamp transitions to on
int numShortOffs =0; // off pulses less than FLASHMIN
int numShortOns = 0; // on pulses less than FLASHMIN
int numMidOffs =0; 
int numMidOns =0;
int lastCode =0; // previous error code, shown only on OLED display
int lightState =0;
int firstFlash =1; // flag needed to know if first flash has happened

void setup()
{
  Wire.begin(); // for OLED
  Serial.begin(9600); // serial port to PC
  mpl115a2.begin();  // baro & temp sensor start
  rtc.begin(); // RTC driver start

  SeeedOled.init();  //initialze SEEED OLED display
  SeeedOled.clearDisplay();  //clear the screen and set start position to top left corner
  SeeedOled.setNormalDisplay();  //Set display to normal mode (i.e non-inverse mode)
  SeeedOled.setHorizontalMode();  //Set addressing mode to Horizontal Mode

  pinMode(FLIGHT, INPUT);
  pinMode(FLED, OUTPUT);
  pinMode(FFAIL, OUTPUT);
  pinMode(FCLR, INPUT_PULLUP);

  digitalWrite(FLED, LOW);  // initially on because the furnace led is also initially on
  digitalWrite(FFAIL, HIGH); // Fail light initially off

  mpl115a2.getPT(&pressureKPA,&temperatureC);
  DateTime now = rtc.now();

  // print data column names
  Serial.print("Date");
  Serial.print(",Time");
  Serial.print(",Pressure");
  Serial.print(",Temperature");
  Serial.print(",Error");
  Serial.print(",Shorts");
  Serial.print(",Longs");
  Serial.print(",Lamp");
  Serial.print(",Ontime");
  Serial.print(",Offtime");
  Serial.print(",Short offs");
  Serial.print(",Short ons");
  Serial.print(",Mid offs");
  Serial.print(",On glitches");
  Serial.print(",Off glitches");
  Serial.println(",Cause");

  // print first data
  Serial.print(now.year()); Serial.print("-");
  Serial.print(now.month()); Serial.print("-");
  Serial.print(now.day()); Serial.print(", ");
  Serial.print(now.hour()); Serial.print(":");
  Serial.print(now.minute()); Serial.print(":");
  Serial.print(now.second()); Serial.print(",");
  Serial.print(pressureKPA, 2); Serial.print(",");
  Serial.print(temperatureC, 1); Serial.print(",");
  Serial.print(furnaceCode); Serial.print(",");
  Serial.print(numShorts); Serial.print(",");
  Serial.print(numLongs); Serial.print(",");
  Serial.print(lightState); Serial.print(",");
  Serial.print(onDuration); Serial.print(",");
  Serial.print(offDuration); Serial.print(",");
  Serial.print(numShortOffs); Serial.print(",");
  Serial.print(numShortOns); Serial.print(",");
  Serial.print(numMidOffs); Serial.print(",");
  Serial.print(onGlitches); Serial.print(", ");
  Serial.print(offGlitches); Serial.print(", ");
  Serial.println("start");

  attachInterrupt(digitalPinToInterrupt(FLIGHT), isr, CHANGE);
}

// ISR triggered by furnace light turning off or on
void isr()
{
  isrEntryTime = millis(); // time the lamp changed
  // primitive debouncing of the lamp input
  // read it three times, use the majority value
  // ideally 0 (on) or 3 (off), acceptable 1 (on) or 2 (off)
  lightState =0;
  delayMicroseconds(500);
  lightState = digitalRead(FLIGHT);
  delayMicroseconds(10);
  lightState = lightState + digitalRead(FLIGHT);
  delayMicroseconds(10);
  lightState = lightState + digitalRead(FLIGHT);

  // count poor transitions and report later
  if (lightState ==1) onGlitches = onGlitches +1;
  if (lightState ==2) offGlitches = offGlitches +1;

  if (lightState >= 2) { // light went off
    tone(TONEPIN, 1000);  //  tone to show lamp state on audio recording
    digitalWrite(FLED, HIGH); // set furnace copy led off
    digitalWrite(FFAIL, LOW); // set fail led on
    lightOff =1;
    lightOn =0;
	  offAtTime = isrEntryTime;
		onDuration = offAtTime - onAtTime;
		// light went off, was this a short or long flash
		if ((onDuration > FLASHMIN) && (onDuration < FLASHSHORT)) 
			numShorts = numShorts +1;
		if ((onDuration > FLASHLONG) && (onDuration < FLASHPAUSE))
    	numLongs = numLongs +1;
    // count abnormal pulses
    if (onDuration <= FLASHMIN) numShortOns = numShortOns +1;
    if ((onDuration >= FLASHSHORT) && (onDuration <= FLASHLONG)) numMidOffs = numMidOffs +1;
  } 

  if (lightState <= 1) { // light went on
    noTone(TONEPIN); // stop tone
    digitalWrite(FLED, LOW); // set furnace copy led on
    lightOn =1;
    lightOff =0;
 		onAtTime = isrEntryTime;
  	offDuration = onAtTime - offAtTime;

    // a bit of a hack here, to skip the very first time timeOff gt FLASHOFF
    // this is the first time lamp goes on after starting the monitoring,
    // since onTime was zero
    if (firstFlash ==1) firstFlash =0;
    else{
      if (offDuration > FLASHPAUSE) {
        furnaceCode = numShorts * 10 + numLongs;
        newCode =1;
      }
    }
  }
}

void loop() {
  unsigned long tempTime = 0; // temp variable for time

  if (digitalRead(FCLR) == 0) digitalWrite(FFAIL, HIGH); // turn off Fail LED if button pushed

  // get date, time, pressure, temp
  DateTime now = rtc.now();
  mpl115a2.getPT(&pressureKPA,&temperatureC);

  // some codes only flash once and have no FLASHPAUSE off time, check for these
  // by testing if there was an ISR, and there were some short and long flashes.
  // Use +500 to avoid overlap with the real pause handled in the ISR.
  tempTime = isrEntryTime + FLASHPAUSE + 500;
  if   ((millis() > tempTime) && (numShorts !=0) && (numLongs !=0)) {
        newCode =1;
  }

  // display stuff
  SeeedOled.setTextXY(0, 0);
  SeeedOled.putNumber(now.hour()); SeeedOled.putString(" : ");
  SeeedOled.putNumber(now.minute()); SeeedOled.putString(" : ");
  SeeedOled.putNumber(now.second());
  SeeedOled.setTextXY(2, 0);
  SeeedOled.putString("kPa: "); SeeedOled.putNumber(pressureKPA); 
  SeeedOled.setTextXY(3, 0);
  SeeedOled.putString("C:   "); SeeedOled.putNumber(temperatureC); 
  SeeedOled.setTextXY(4, 0);
  SeeedOled.putNumber(numShortOffs); SeeedOled.putString(" ");
  SeeedOled.putNumber(numShortOns); SeeedOled.putString(" ");
  SeeedOled.putNumber(numMidOffs);  SeeedOled.putString(" ");
  SeeedOled.putNumber(numMidOns); SeeedOled.putString(" ; ");
  SeeedOled.putNumber(lastCode);
  SeeedOled.setTextXY(6, 0);
  // Extra preceding space needed if zero (a single digit) in layout
  if (furnaceCode ==0) {
    SeeedOled.putString("Code:  ");
    SeeedOled.putNumber(furnaceCode);
  }
  else {
    SeeedOled.putString("Code: ");
    SeeedOled.putNumber(furnaceCode);
  }
 
  // print stats if an event (fault code, lamp on, or lamp off) happened
  // use if with all three conditions for verbose output during debug
  if ((lightOn ==1) || (lightOff==1) || (newCode==1)) {
  // if (newCode==1) {
    // clear the lamp durations so they are only reported first time
    if (lightOn ==1) onDuration =0;
    if (lightOff ==1) offDuration =0;
    // print stats in csv format
    Serial.print(now.year()); Serial.print("-");
    Serial.print(now.month()); Serial.print("-");
    Serial.print(now.day()); Serial.print(", ");
    Serial.print(now.hour()); Serial.print(":");
    Serial.print(now.minute()); Serial.print(":");
    Serial.print(now.second()); Serial.print(", ");
    Serial.print(pressureKPA, 2); Serial.print(", ");
    Serial.print(temperatureC, 1); Serial.print(", ");
    Serial.print(furnaceCode); Serial.print(", ");
    Serial.print(numShorts); Serial.print(", ");
    Serial.print(numLongs); Serial.print(", ");
    Serial.print(lightState); Serial.print(", ");
    Serial.print(onDuration); Serial.print(", ");
    Serial.print(offDuration); Serial.print(", ");
    Serial.print(numShortOffs); Serial.print(", ");
    Serial.print(numShortOns); Serial.print(", ");
    Serial.print(numMidOffs); Serial.print(", ");
    Serial.print(onGlitches); Serial.print(", ");
    Serial.print(offGlitches); Serial.print(", ");
    // print reason for reporting
    if (newCode==1){
      Serial.println("newCode");
      lastCode = furnaceCode;
      furnaceCode =0;
      newCode =0;
 		  numShorts =0;
		  numLongs =0;
    } 
    else {
      if (lightOn==1) {
        Serial.println("lightOn");
        lightOn =0;
      }
      if (lightOff==1) {
        Serial.println("lightOff");
        lightOff =0;
      }
    }
  }

  // print stats every 10 seconds
  tempTime = millis();
  if (tempTime > lastPrintTime) {
    lastPrintTime = tempTime + 10000;
    Serial.print(now.year()); Serial.print("-");
    Serial.print(now.month()); Serial.print("-");
    Serial.print(now.day()); Serial.print(", ");
    Serial.print(now.hour()); Serial.print(":");
    Serial.print(now.minute()); Serial.print(":");
    Serial.print(now.second()); Serial.print(", ");
    Serial.print(pressureKPA, 2); Serial.print(", ");
    Serial.print(temperatureC, 1); Serial.print(", ");
    Serial.print(furnaceCode); Serial.print(", ");
    Serial.print(numShorts); Serial.print(", ");
    Serial.print(numLongs); Serial.print(", ");
    Serial.print(lightState); Serial.print(", ");
    Serial.print(onDuration); Serial.print(", ");
    Serial.print(offDuration); Serial.print(", ");
    Serial.print(numShortOffs); Serial.print(", ");
    Serial.print(numShortOns); Serial.print(", ");
    Serial.print(numMidOffs); Serial.print(", ");
    Serial.print(onGlitches); Serial.print(", ");
    Serial.print(offGlitches); Serial.print(", ");
    Serial.println("time");
  }
}

