/*
 * Linux GPIO backend using sysfs
 *
 * Copyright (C) 2010, Florian Fainelli <f.fainelli@gmail.com>
 *
 * This file is part of "cc2530prog", this file is distributed under
 * a 2-clause BSD license, see LICENSE for details.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "gpio.h"
#include <wiringPi.h>

int
gpio_init(void)
{
    return wiringPiSetupGpio();
}

int
gpio_export(int n)
{
    return 0;
}

int gpio_unexport(int n)
{
    return 0;
}

int
gpio_set_direction(int n, enum gpio_direction direction)
{
	static int modes[] = {
		[GPIO_DIRECTION_IN]	= INPUT,
		[GPIO_DIRECTION_OUT]	= OUTPUT,
		[GPIO_DIRECTION_HIGH]	= INPUT,
	};
    pinMode(n, modes[direction]);
    return 0;
}

int
gpio_get_value(int n, bool *value)
{
    *value = (bool) (digitalRead(n)) & 1;
	return 0;
}

int
gpio_set_value(int n, bool value)
{
    digitalWrite(n, (int) value);
    return 0;
}
