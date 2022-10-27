/******************************************************************************
*
*  Name: led.c
*
*  Description:
*    This is an example usage of the DP AVR peripheral's vram, reg, and eeprom
*  resources.
*
*  Test case:  
*    - pcset avr vram 0 7f    -- the LED will blink at twice the speed
*    - pcset avr eeprom 0 7f  -- reset the default blink rate
*    - reset the AVR          -- the LED will blink at the same rate
*    - pcset avr vram 1 0     -- disable blinking
*    - pcset avr reg 2b 80    -- turn on the LED directly
    // FIXME: this does not work
*    - pcget avr reg 2a 2     -- returns 80 80 (DDR and data regs)
*    - pcset avr reg 2b 0     -- turn off the LED directly
*    - pcset avr vram 1 1     -- re-enable blinking
*
******************************************************************************/

#include "../../include/pcavr.h"
#include <util/delay.h>

// register and address definitions
#define DELAY_INIT_ADDR ((uint8_t*)0x00)    // EEPROM address to hold the initial delay value
#define DELAY hostRegs[0]                   // current delay value is in host reg 0
#define BLINK_ENABLE hostRegs[1]            // blink enable flag is in host reg 1

// delay ms milliseconds
void DelayMs(int ms)
{
    for (; ms > 0; ms--) 
        _delay_ms(1);
}

int main()
{
    // init communications between the host and the AVR
    pcavr_init();
    
    // make AVR peripheral LED an output
    DDRD |= (1 << DPLED);
    
    // enable blinking and init delay from EEPROM
    BLINK_ENABLE = 1;
    DELAY = eeprom_read_byte(DELAY_INIT_ADDR);

    // blink the LED at a period of DELAY milliseconds
    while(1)
    {
        if (BLINK_ENABLE)
        { 
            // turn the LED on then off
            PORTD |= (1 << DPLED);
            DelayMs(DELAY);
            PORTD &= ~(1 << DPLED);
            DelayMs(DELAY);
        }
    }
    
    return 0;
}
