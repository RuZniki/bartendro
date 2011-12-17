#define F_CPU 16000000UL 
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <stdarg.h>
#include <stdlib.h>

#include "../master/packet.h"

// Bit manipulation macros
#define sbi(a, b) ((a) |= 1 << (b))       //sets bit B in variable A
#define cbi(a, b) ((a) &= ~(1 << (b)))    //clears bit B in variable A
#define tbi(a, b) ((a) ^= 1 << (b))       //toggles bit B in variable A

#define BAUD 38400
#define UBBR (F_CPU / 16 / BAUD - 1)

static uint8_t g_address = 0xFF;
static uint8_t g_response_payload[2] = { 0, 0 };

static volatile uint8_t g_spi_ch_in = 0;
static volatile uint8_t g_spi_ch_out = 0;
static volatile uint8_t g_spi_char_received = 0;
static volatile uint8_t g_ss_reset = 0;
static volatile uint8_t g_hall_sensor_1 = 0;
static volatile uint8_t g_hall_sensor_2 = 0;

ISR(SPI_STC_vect)
{
    g_spi_ch_in = SPDR;
    SPDR = g_spi_ch_out;
    g_spi_char_received = 1;
}

ISR(PCINT0_vect)
{
    if (PINB & (1<<PINB2))
        g_ss_reset = 1;
    else
        g_ss_reset = 0;
}

ISR(PCINT1_vect)
{
    if (PINC & (1<<PINC0))
        g_hall_sensor_1++;
    if (PINC & (1<<PINC1))
        g_hall_sensor_2++;
}

void serial_init(void)
{
    /*Set baud rate */ 
    UBRR0H = (unsigned char)(UBBR>>8); 
    UBRR0L = (unsigned char)UBBR; 
    /* Enable transmitter */ 
    UCSR0B = (1<<TXEN0); 
    /* Set frame format: 8data, 1stop bit */ 
    UCSR0C = (0<<USBS0)|(3<<UCSZ00); 
}
void serial_tx(unsigned char ch)
{
    while ( !( UCSR0A & (1<<UDRE0)) );
    UDR0 = ch;
}
#define MAX 80 
void dprintf(const char *fmt, ...)
{
    va_list va;
    va_start (va, fmt);
    char buffer[MAX];
    char *ptr = buffer;
    vsnprintf(buffer, MAX, fmt, va);
    va_end (va);
    for(ptr = buffer; *ptr; ptr++)
    {
        if (*ptr == '\n') serial_tx('\r');
        serial_tx(*ptr);
    }
}

void spi_slave_init(void)
{
	// Set MISO as output 
	DDRB = (1<<PORTB4);

	//SPCR = [SPIE][SPE][DORD][MSTR][CPOL][CPHA][SPR1][SPR0]
	//SPI Control Register = Interrupt Enable, Enable, Data Order, Master/Slave select, Clock Polarity, Clock Phase, Clock Rate
	SPCR = (1<<SPE)|(1<<SPIE)|(1<<SPR0);	// Enable SPI, set clock rate fck/16 
}

void spi_slave_stop(void)
{
	SPCR &= ~((1<<SPE)|(1<<SPIE));	// Disable SPI
}

uint8_t spi_transfer(uint8_t tx)
{
    uint8_t rec, reset, ch;

    cli();
    g_spi_ch_out = tx;
    sei();

    for(;;)
    {
        cli();
        reset = g_ss_reset;
        rec = g_spi_char_received;
        sei();

        if (reset)
            return 0;

        if (rec)
            break;
    }

    cli();
    g_spi_char_received = 0;
    ch = g_spi_ch_in;
    sei();
    return ch;
}

uint8_t spi_transfer_poll(uint8_t tx)
{
    SPDR = tx;
	/* Wait for reception complete */
	while(!(SPSR & (1<<SPIF)))
		;
	/* Return Data Register */
	return SPDR;
}

uint8_t receive_packet(packet *rx)
{
    static uint8_t ch = 0;
    uint8_t *prx = (uint8_t*)rx;
    uint8_t received = 0, old, reset = 0;

    for(; received < sizeof(packet);)
    {
        cli();
        reset = g_ss_reset;
        sei();
        if (reset)
            return 0;

        old = ch;
        ch = spi_transfer(ch);
//        dprintf("%x\n", ch);

        // Look for the packet header
        if (prx == (uint8_t*)rx && ch != 0xFF)
            continue;

        *prx = ch;
        prx++;
        received++;
    }
    return 1;
}

void setup(void)
{
	DDRD |= (1<<PORTD7); //LED pin
    DDRC |= (1<<PORTC0); //LED pin

    PCMSK0 |= (1<<PCINT2);
    PCICR |= (1<<PCIE0);

    PCMSK1 |= (1<<PCINT8)|(1<<PCINT9);
    PCICR |= (1<<PCIE1);

    serial_init();
}

void test(void)
{
    uint8_t ch = 0;

    for(;;)
    {
        ch = spi_transfer(ch);
        dprintf("%x\n", ch);
    }
}

void wait_for_reset()
{
    uint8_t count = 0, reset;

    dprintf("Wait for reset signal\n");
    for(;;)
    {
        cli();
        reset = g_ss_reset;
        sei();
        if (!reset)
        {
            _delay_ms(1);
            count++;
        }
        else
            break;
        if (count == 100)
        {
           tbi(PORTC, 0);
           count = 0;
        }
    }
}

void address_assignment(void)
{
    uint8_t ch = 0;

    for(;;)
    {
        ch = spi_transfer(ch);
        if (ch > 0 && ch < 0xff)
        {
            g_address = ch;
            ch++;
            spi_transfer(ch);
            break;
        }
    }

    dprintf("got address: %d\n", g_address);
}

void led_pwm_setup(void)
{
	/* Set to Fast PWM */
	TCCR0A |= _BV(WGM01) | _BV(WGM00);
	TCCR2A |= _BV(WGM21) | _BV(WGM20);

	// Set the compare output mode
	TCCR0A |= _BV(COM0A1);
	TCCR0A |= _BV(COM0B1);
	TCCR2A |= _BV(COM2B1);

	// Reset timers and comparators
	OCR0A = 0;
	OCR0B = 0;
	OCR2B = 0;
	TCNT0 = 0;
	TCNT2 = 0;

    // Set the clock source
	TCCR0B |= _BV(CS00);
	TCCR2B |= _BV(CS20);

    // Set PWM pins as outputs
    DDRD |= (1<<PD6)|(1<<PD5)|(1<<PD3);
}

void motor_pwm_setup(void)
{
	/* Set to Fast PWM */
	TCCR1A |= _BV(WGM11) | _BV(WGM10);

	// Set the compare output mode
	TCCR1A |= _BV(COM1A1);

	// Reset timers and comparators
	OCR1A = 0;
	TCNT1 = 0;

    // Set the clock source
	TCCR1B |= _BV(CS10);

    // Set PWM pins as outputs
    DDRB |= (1<<PB1);
}

void set_motor_speed(uint8_t speed)
{
    OCR1A = speed;
}

void set_led_color(uint8_t red, uint8_t green, uint8_t blue)
{
    OCR2B = 255 - red;
    OCR0A = 255 - blue;
    OCR0B = 255 - green;
}

void motor_test(void)
{
    uint8_t i = 0, h1 = 0, h2 = 0, last_h1 = 0, last_h2 = 0;

	while (1)
    {
        cli();
        h1 = g_hall_sensor_1;
        h2 = g_hall_sensor_2;
        sei();

        if (h1 != last_h1 || h2 != last_h2)
        {
            dprintf("speed: %d h1: %d h2: %d\n", i, h1, h2);
            cli();
            g_hall_sensor_1 = 0;
            g_hall_sensor_2 = 0;
            sei();
        }

        set_motor_speed(i++);
        _delay_ms(20); 

        last_h1 = h1;
        last_h2 = h2;
    }
}

int main (void)
{
    packet  p;
    uint8_t i, *ptr;

	setup();
    led_pwm_setup();
    motor_pwm_setup();

    dprintf("slave starting\n");
    sei();

    wait_for_reset();
	spi_slave_init();
    address_assignment();

    for(;;)
    {
        for(;;)
        {
            if (!receive_packet(&p))
            {
                dprintf("got reset notice!\n");
                // If SS went high, reset and start over
                spi_slave_stop();
                set_motor_speed(0);
                g_address = 0;
                wait_for_reset();
                spi_slave_init();
                address_assignment();
                continue;
            }

            // If we have no address yet, ignore all packets
            if (g_address == 0)
            {
                dprintf("ignore packet\n");
                continue;
            }

            if (p.addr != g_address)
                continue;
            if (p.type == PACKET_TYPE_START)
            {
                set_motor_speed(0);
                sbi(PORTC, 0);
                dprintf("turn off\n");
            }
            else
            if (p.type == PACKET_TYPE_STOP)
            {
                set_motor_speed(255);
                cbi(PORTC, 0);
                dprintf("turn on\n");
            }
            else
            {
                uint8_t *pp = (uint8_t *)&p;

                dprintf("bad packet: ");
                for(i = 0; i < sizeof(packet); i++, pp++)
                    dprintf("%x ", *pp);
                dprintf("\n");
            }
        }
    }
}
