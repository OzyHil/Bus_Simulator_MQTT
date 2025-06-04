#ifndef LEDS_H
#define LEDS_H

#include "General.h"

// Definições dos pinos conectados aos LEDs
#define GREEN_LED 11 
#define BLUE_LED 12  
#define RED_LED 13  
#define WRAP_PWM_LED 1  // PWM para controle de brilho

// Estrutura para representar uma cor RGB
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color;

// Definições de cores para LEDs
extern const led_color GREEN;
extern const led_color YELLOW;
extern const led_color DARK;
extern const led_color RED;
extern const led_color BLUE;
extern const led_color MAGENTA;

extern const led_color BLUE_DIM;
extern const led_color MAGENTA_DIM;
extern const led_color RED_DIM;
extern const led_color YELLOW_DIM;
extern const led_color GREEN_DIM;

// Inicializa os pinos dos LEDs
void configure_leds();

// Ajusta o brilho de um LED via PWM
void set_led_brightness(uint gpio, uint8_t level);

// Define a cor de saída RGB no LED RGB
void set_led_color(led_color color);

#endif