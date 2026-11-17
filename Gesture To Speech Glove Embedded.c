/*
  GESTURE TO SPEECH GLOVE SYSTEM - EMBEDDED C (ATmega328P / Arduino Nano)
  ------------------------------------------------------------------------
  Bare-metal implementation: no Arduino core, no libraries.
  Direct register access for ADC, I2C (TWI) and USART.

  Hardware:
    Flex Sensor 1-4   -> ADC0-ADC3 (A0-A3)
    16x2 LCD (PCF8574 I2C backpack, addr 0x27) -> SDA=A4, SCL=A5
    DFPlayer Mini     -> TX pin -> PD0 (RX of Nano, hardware UART)
                         RX pin <- PD1 (TX of Nano, via 1k resistor)

  Compile (avr-gcc example):
    avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -c main.c -o main.o
    avr-gcc -mmcu=atmega328p main.o -o main.elf
    avr-objcopy -O ihex main.elf main.hex
    avrdude -c arduino -p atmega328p -P <port> -b 57600 -U flash:w:main.hex
*/

#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
#include <string.h>
#include <stdlib.h>

/* ======================= ADC ======================= */

void adc_init(void) {
    ADMUX  = (1 << REFS0);                               // AVcc reference
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0); // /128
}

uint16_t adc_read(uint8_t channel) {
    ADMUX = (ADMUX & 0xF0) | (channel & 0x0F);
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC));
    return ADC;
}

/* ======================= I2C (TWI) MASTER ======================= */

#define TWI_SCL_FREQ 100000UL

void i2c_init(void) {
    TWSR = 0x00;
    TWBR = (uint8_t)(((F_CPU / TWI_SCL_FREQ) - 16) / 2);
    TWCR = (1 << TWEN);
}

void i2c_start(void) {
    TWCR = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
}

void i2c_stop(void) {
    TWCR = (1 << TWINT) | (1 << TWSTO) | (1 << TWEN);
}

void i2c_write(uint8_t data) {
    TWDR = data;
    TWCR = (1 << TWINT) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
}

/* ======================= LCD via PCF8574 (I2C backpack) ======================= */

#define LCD_ADDR     0x27   // change to 0x3F if LCD does not respond
#define LCD_BACKLIGHT 0x08
#define EN_BIT       0x04
#define RS_BIT       0x01

void i2c_lcd_write_byte(uint8_t data) {
    i2c_start();
    i2c_write(LCD_ADDR << 1);
    i2c_write(data);
    i2c_stop();
}

void lcd_pulse_enable(uint8_t data) {
    i2c_lcd_write_byte(data | EN_BIT);
    _delay_us(1);
    i2c_lcd_write_byte(data & ~EN_BIT);
    _delay_us(50);
}

void lcd_send_nibble(uint8_t nibble, uint8_t rs) {
    uint8_t data = (nibble & 0xF0) | LCD_BACKLIGHT | (rs ? RS_BIT : 0);
    lcd_pulse_enable(data);
}

void lcd_send(uint8_t value, uint8_t rs) {
    lcd_send_nibble(value & 0xF0, rs);
    lcd_send_nibble((value << 4) & 0xF0, rs);
}

void lcd_command(uint8_t cmd) { lcd_send(cmd, 0); }
void lcd_data(uint8_t data)   { lcd_send(data, 1); }

void lcd_init(void) {
    _delay_ms(50);
    lcd_send_nibble(0x30, 0); _delay_ms(5);
    lcd_send_nibble(0x30, 0); _delay_us(150);
    lcd_send_nibble(0x30, 0);
    lcd_send_nibble(0x20, 0);           // set 4-bit mode

    lcd_command(0x28);                  // 2 line, 5x8 font
    lcd_command(0x0C);                  // display on, cursor off
    lcd_command(0x06);                  // entry mode: increment
    lcd_command(0x01);                  // clear display
    _delay_ms(2);
}

void lcd_clear(void) {
    lcd_command(0x01);
    _delay_ms(2);
}

void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t rowOffset[] = {0x00, 0x40};
    lcd_command(0x80 | (col + rowOffset[row]));
}

void lcd_print(const char *str) {
    while (*str) lcd_data(*str++);
}

/* ======================= USART (for DFPlayer Mini) ======================= */

#define BAUD 9600UL
#define UBRR_VAL ((F_CPU / (16UL * BAUD)) - 1)

void uart_init(void) {
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)(UBRR_VAL);
    UCSR0B = (1 << TXEN0) | (1 << RXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); // 8N1
}

void uart_tx(uint8_t data) {
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = data;
}

/* ---- DFPlayer Mini command frame ----
   Frame: 7E FF 06 CMD 00 PARAM1 PARAM2 CHECKSUM_H CHECKSUM_L EF
*/
void df_send_command(uint8_t cmd, uint8_t param1, uint8_t param2) {
    uint16_t checksum = 0xFFFF - (0xFF + 0x06 + cmd + 0x00 + param1 + param2) + 1;

    uart_tx(0x7E);
    uart_tx(0xFF);
    uart_tx(0x06);
    uart_tx(cmd);
    uart_tx(0x00);
    uart_tx(param1);
    uart_tx(param2);
    uart_tx((checksum >> 8) & 0xFF);
    uart_tx(checksum & 0xFF);
    uart_tx(0xEF);
}

void df_set_volume(uint8_t vol) {        // 0-30
    df_send_command(0x06, 0x00, vol);
}

void df_play_track(uint16_t track) {     // plays NNNN.mp3 from SD card
    df_send_command(0x03, (track >> 8) & 0xFF, track & 0xFF);
}

/* ======================= GESTURE LOGIC ======================= */

const uint8_t  flexChannel[4] = {0, 1, 2, 3}; // ADC0-ADC3
uint16_t       flexValue[4];
uint8_t        gestureBits[4];                // 0 = bent, 1 = straight
int16_t        threshold[4] = {600, 600, 600, 600}; // calibrate per sensor

const char *gesturePhrase[16] = {
    "Standby",    // 0000
    "Hello",      // 0001
    "Yes",        // 0010
    "No",         // 0011
    "Help",       // 0100
    "Water",      // 0101
    "Food",       // 0110
    "Thank You",  // 0111
    "Please",     // 1000
    "Stop",       // 1001
    "Bathroom",   // 1010
    "Pain",       // 1011
    "Doctor",     // 1100
    "Okay",       // 1101
    "Sorry",      // 1110
    "Ready"       // 1111
};

void read_sensors(void) {
    for (uint8_t i = 0; i < 4; i++) {
        flexValue[i] = adc_read(flexChannel[i]);
        gestureBits[i] = (flexValue[i] >= threshold[i]) ? 1 : 0;
    }
}

uint8_t get_gesture_code(void) {
    uint8_t code = 0;
    code |= gestureBits[0] << 3;
    code |= gestureBits[1] << 2;
    code |= gestureBits[2] << 1;
    code |= gestureBits[3] << 0;
    return code;
}

void code_to_binary_string(uint8_t code, char *buf) {
    for (int8_t i = 3; i >= 0; i--) {
        buf[3 - i] = ((code >> i) & 0x01) ? '1' : '0';
    }
    buf[4] = '\0';
}

void display_and_speak(uint8_t code) {
    char binStr[5];
    code_to_binary_string(code, binStr);

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Gesture: ");
    lcd_print(binStr);

    lcd_set_cursor(0, 1);
    lcd_print(gesturePhrase[code]);

    df_play_track(code + 1); // files 0001.mp3 .. 0016.mp3
}

/* ======================= MAIN ======================= */

int main(void) {
    adc_init();
    i2c_init();
    uart_init();

    lcd_init();
    lcd_print("Gesture Glove");
    lcd_set_cursor(0, 1);
    lcd_print("Initializing...");
    _delay_ms(1000);

    df_set_volume(25); // 0-30
    _delay_ms(200);

    lcd_clear();
    lcd_print("READY / OPEN");

    int8_t lastGesture = -1;

    while (1) {
        read_sensors();
        uint8_t code = get_gesture_code();

        if (code != lastGesture) {
            display_and_speak(code);
            lastGesture = code;
            _delay_ms(1500); // debounce / hold time before next read
        }
    }

    return 0;
}
