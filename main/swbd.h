#ifndef SWBD_H
#define SWBD_H

#include <stdint.h>
#include "ble_utils.h"

#define HI(x) ((x)>>8)
#define LO(x) ((x)& 0xFF)
#define AP_BUTTON GPIO_NUM_4f
#define BLE_CONNECTED_LED GPIO_NUM_8

uint8_t chsum(uint8_t array[6]);
uint8_t get_swbd_speed(uint8_t array[6]);
uint8_t get_swbd_enable(uint8_t array[6]);
uint8_t get_swbd_sensivity(uint8_t array[6]);
uint8_t get_swbd_move_sensor(uint8_t array[6]);
uint8_t get_swbd_hours(uint8_t array[6]);
uint8_t get_swbd_minutes(uint8_t array[6]);

#endif