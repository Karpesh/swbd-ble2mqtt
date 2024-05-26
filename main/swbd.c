#include "swbd.h"




// контрольная сумма
uint8_t chsum(uint8_t array[6])
{
	uint8_t i;
	uint16_t sum=0;
	for (i=0; i<5; i++) {
		sum = sum  + array[i]*211;
		sum =(sum ^ (sum >> 8));
	}	
	return LO(sum);
}

// "скорость" (мощность) качания
uint8_t get_swbd_speed(uint8_t array[6])
{
	uint8_t speed = speed=array[0]&0b00000111;
	return speed;
}

// запущено качание или нет 
uint8_t get_swbd_enable(uint8_t array[6])
{
	uint8_t orderON=(array[0]>>7);
    return orderON;
}

// чувствительность микрофона 
uint8_t get_swbd_sensivity(uint8_t array[6])
{
    uint8_t sensivity=array[1]&0b00000111;
    return sensivity;
}

// датчик движения 
uint8_t get_swbd_move_sensor(uint8_t array[6])
{
    uint8_t move_sensor=(array[1]>>7);
    return move_sensor;
}

// количество часов 
uint8_t get_swbd_hours(uint8_t array[6])
{
    uint16_t time=(uint16_t)((array[2]<<8)|(array[3]));
    uint8_t hours=time/60; 
    return hours;
}

// количество минут 
uint8_t get_swbd_minutes(uint8_t array[6])
{
    uint16_t time=(uint16_t)((array[2]<<8)|(array[3]));
    uint8_t minutes=time%60; 
    return minutes;
}