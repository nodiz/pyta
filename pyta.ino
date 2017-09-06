#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "PinChangeInterrupt.h"

#define portOfPin(P)(((P)>=0&&(P)<8) ? &PORTD : (((P)>7&&(P)<14) ? &PORTB : &PORTC))
#define ddrOfPin(P)(((P)>=0&&(P)<8) ? &DDRD : (((P)>7&&(P)<14) ? &DDRB : &DDRC))
#define pinOfPin(P)(((P)>=0&&(P)<8) ? &PIND : (((P)>7&&(P)<14) ? &PINB : &PINC))
#define pinIndex(P)((uint8_t)(P>13 ? P-14 : P& 7))
#define pinMask(P)((uint8_t)(1<<pinIndex(P)))

#define pinAsInput(P) *(ddrOfPin(P))&=~pinMask(P)
#define pinAsInputPullUp(P) *(ddrOfPin(P))&=~pinMask(P); digitalHigh(P)
#define pinAsOutput(P) *(ddrOfPin(P))|=pinMask(P)
#define digitalLow(P) *(portOfPin(P))&=~pinMask(P)
#define digitalHigh(P) *(portOfPin(P))|=pinMask(P)
#define isHigh(P)((*(pinOfPin(P))& pinMask(P))>0)
#define isLow(P)((*(pinOfPin(P))& pinMask(P))==0)
#define digitalState(P)((uint8_t)isHigh(P))

#define T_START 1e3   // time (ms) to consider a button pressed
#define T_AJD 2.8e2   // time (ms) for each new step
#define T_OPEN 5e2    // time (ms) require to user to open the box
#define T_EEPROM 3e2  // time (s) before saving to eeprom
#define T_IDLE 45     // time (s) of idling before turning off display

#define OVERVAL 60    // overflow: negative value when time is higher
#define H_MAX 24      // time (h) in 1 day

//GPIO CONSTANTS

#define LOCKER 2
#define D_BACKLIGHT 4
#define G_LED 5
#define BUTTON_PLUS 9
#define BUTTON_MINUS 6
#define BUTTON_START 7
#define BUTTON_RESET 3

typedef struct Temps {
        uint8_t seconds;
        uint8_t minutes;
        uint8_t hours;
        uint8_t days;
} TEMPS;

LiquidCrystal_I2C lcd(0x27, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);

uint8_t time_running = 0;
volatile uint32_t t_tmp = 0;
volatile int16_t sleep_eeprom = 240; //60 seconds before saving first time
volatile uint8_t i = 0;
volatile uint8_t idle_tmp;
char digits[2];

const TEMPS ZERO = {0, 0, 0, 0};
const TEMPS OUT = {0xff,0xff,0xff,0xff};
TEMPS prec = OUT;
TEMPS parsed = ZERO;


// INTERRPTIONS

ISR(TIMER1_COMPA_vect) {
        idle_tmp++;
        if (time_running != 1)
                return;
        if (--parsed.seconds > OVERVAL) {
                parsed.seconds = 59;
                if (--parsed.minutes > OVERVAL) {
                        parsed.minutes = 59;
                        if (--parsed.hours > H_MAX) {
                                parsed.hours = 23;
                                if (--parsed.days > 10) {
                                        time_running = 2;
                                        return;
                                }
                        }
                }
        }
        sleep_eeprom++;
}

void isrIdle(){
        digitalHigh(D_BACKLIGHT);
        idle_tmp=0;
}

// MAIN FUNCTIONS

void setup(){
        //SETTING INTERRUPT ON TIMER
        TCCR1A = 0x00;
        OCR1A = 15625;
        TIFR1 = TIFR1;
        //ENABLING INTERRUPT
        sei();
        TCNT1 = 0;
        TCCR1B = (1 << WGM12) | (1 << CS12) | (1 << CS10);
        TIMSK1 = (1 << OCIE1A);
        SMCR = (1 << SE);
        //INPUT/OUTPUT SETTINGS
        pinAsInputPullUp(BUTTON_PLUS);
        pinAsInputPullUp(BUTTON_MINUS);
        pinAsInputPullUp(BUTTON_START);
        pinAsInputPullUp(BUTTON_RESET);
        pinAsOutput(LOCKER);
        pinAsOutput(G_LED);
        pinAsOutput(D_BACKLIGHT);

        attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(BUTTON_PLUS), isrIdle, FALLING);
        attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(BUTTON_MINUS), isrIdle, FALLING);
        attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(BUTTON_START), isrIdle, FALLING);
        attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(BUTTON_RESET), isrIdle, FALLING);

        //LET'S START
        game_starting();

        //EEPROM CHECKING
        TEMPS t_tmp = OUT;
        if (((    t_tmp.minutes = EEPROM.read(0))!=0xff)
            ||  ((t_tmp.hours = EEPROM.read(1))!=0xff)
            ||  ((t_tmp.days = EEPROM.read(2))!=0xff)) {
                parsed = t_tmp;
                eeprom_fake();
                return;
        }
}

void loop() {
        while (!time_running) {
                // turning off display for power saving
                if (idle_tmp==T_IDLE)
                        digitalLow(D_BACKLIGHT);
                // showing each 15 seconds if alive with a led
                if (isLow(D_BACKLIGHT))
                        if (idle_tmp%15==10) {
                                digitalHigh(G_LED);
                                delay(5);
                                digitalLow(G_LED);
                        }
                // button management
                if (isLow(BUTTON_RESET)) {
                        parsed = ZERO;
                        display_update();
                        digitalHigh(LOCKER);
                        delay(T_OPEN);
                        digitalLow(LOCKER);
                }
                if (isLow(BUTTON_START)) {
                        t_tmp = millis();
                        while (isLow(BUTTON_START))
                                if (t_tmp - millis() > T_START) {
                                        start_counting();
                                        break;
                                }
                }
                if (isLow(BUTTON_PLUS))
                        button_manager(BUTTON_PLUS, time_add);
                if (isLow(BUTTON_MINUS))
                        button_manager(BUTTON_MINUS, time_sub);
        }
        //COUNTDOWN
        sei();
        SMCR = (1 << SE);
        asm ("sleep");
        if (time_running == 1) {
                if (idle_tmp==T_IDLE/3)
                        digitalLow(D_BACKLIGHT);
                display_update();
                if (sleep_eeprom == T_EEPROM) {
                        sleep_eeprom = 0;
                        EEPROM.write(0, parsed.minutes);
                        EEPROM.write(1, parsed.hours);
                        EEPROM.write(2, parsed.days);
                }
        }
        //AT THE END
        if (time_running == 2) end_locking();
}

void button_manager(uint8_t button, void (*func)(void)) {
        t_tmp = millis();
        uint32_t up = 4;
        (*func)();
        display_update();
        delay(T_AJD);
        while (isLow(button)) {
                for (i=0; i<(up/4); i++)
                        (*func)();
                up++;
                display_update();
                delay(T_AJD);
        }
}

// TIME UTILITIES 

void time_add() {
        if (time_equiv(parsed,ZERO)) {
                parsed = {30,0,0,0};
                return;
        }
        if (time_equiv(parsed,{30,0,0,0}))
        {
                parsed = {0,5,0,0};
                return;
        }
        parsed.minutes += 5;
        if (parsed.minutes > OVERVAL) {
                parsed.hours++;
                parsed.minutes -= 60;
                if (parsed.hours > H_MAX) {
                        parsed.days++;
                        parsed.hours -= H_MAX;
                }
        }
        //EXTRA SECURITY
        if (parsed.hours >= H_MAX) {
                parsed = {59, 59, 23, 0};
                blink_led();
                return;
        }
        if (parsed.days >= 1) {
                parsed = {59, 59, 23, 0};
                blink_led();
                return;
        }
}

void time_sub()
{
        if (time_equiv({0,5,0,0},parsed)) {
                parsed = {30,0,0,0};
                return;
        }
        if (time_equiv({30,0,0,0},parsed)) {
                parsed=ZERO;
                return;
        }
        parsed.minutes -= 5;
        if (parsed.minutes > OVERVAL) {
                parsed.minutes += 60;
                if (--parsed.hours > H_MAX) {
                        parsed.hours += H_MAX;
                        if (--parsed.days > 7) {
                                parsed = ZERO;
                                blink_led();
                                return;
                        }
                }
        }
}

bool time_equiv(TEMPS t1, TEMPS t2)
{
        if ((t1.seconds == t2.seconds) &&
            (t1.minutes == t2.minutes) &&
            (t1.hours == t2.hours))
            return true;
        return false;
}

void display_update() {
        if (parsed.hours != prec.hours) {
                lcd.setCursor(0, 1);
                sprintf(digits, "%02d", parsed.hours);
                lcd.print(digits);
                prec.hours = parsed.hours;
        }
        if (parsed.minutes != prec.minutes) {
                lcd.setCursor(6, 1);
                sprintf(digits, "%02d", parsed.minutes);
                lcd.print(digits);
                prec.minutes = parsed.minutes;
        }
        if (parsed.seconds != prec.seconds) {
                lcd.setCursor(12, 1);
                sprintf(digits, "%02d", parsed.seconds);
                lcd.print(digits);
                prec.seconds = parsed.seconds;
        }
}


//USER INTERACTIONS

void game_starting()
{
        digitalHigh(D_BACKLIGHT);
        lcd.begin(16, 2);
        lcd.setCursor(0, 0);
        lcd.print("Wanna get free?");
        delay(100);
        lcd.setCursor(0, 1);
        lcd.print("Just be brave :)");
        delay(500);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("TIME SET:");
        lcd.setCursor(0, 1);
        lcd.print("00h   00m   00s");
}

void eeprom_fake(){
        blink_led();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("HEY");
        for (int i = 0; i < 3; i++) {
                lcd.print(".");
                delay(30);
        }
        delay(100);
        lcd.setCursor(0, 1);
        lcd.print("Some time left");
        delay(800);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("RESET!!");
        delay(1000);
        sleep_eeprom = 0;
        lcd.setCursor(0, 1);
        lcd.print("00h   00m   00s");
        start_counting();
}

void start_counting()
{
        time_running = 1;
        lcd.backlight();
        delay(100);
        lcd.noBacklight();
        delay(100);
        lcd.backlight();
        lcd.setCursor(0, 0);
        lcd.print("TIME REMAINING:");
}

void blink_led() {
        digitalHigh(G_LED);
        delay(100);
        digitalLow(G_LED);
        delay(100);
        digitalHigh(G_LED);
        delay(100);
        digitalLow(G_LED);
        delay(100);
}

void end_locking() {
        extern volatile unsigned long timer0_millis, timer0_overflow_count;
        timer0_millis = timer0_overflow_count = 0;
        digitalHigh(D_BACKLIGHT);
        time_running = 0;
        EEPROM.write(0, 0xff);
        EEPROM.write(1, 0xff);
        EEPROM.write(2, 0xff);
        prec = OUT;
        digitalHigh(LOCKER);
        blink_led();
        blink_led();
        delay(1500);
        digitalLow(LOCKER);
        parsed = {0, 0, 0, 0};
        display_update();
        lcd.setCursor(0, 0);
        lcd.print("TIME SET:        ");
}
