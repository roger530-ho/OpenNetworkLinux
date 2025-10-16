/************************************************************
 * <bsn.cl fy=2014 v=onl>
 *
 *           Copyright 2014 Big Switch Networks, Inc.
 *           Copyright 2025 Accton Technology Corporation.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 * </bsn.cl>
 ************************************************************
 *
 *
 *
 ***********************************************************/
#include <onlplib/file.h>
#include <onlp/platformi/ledi.h>
#include "platform_lib.h"

#define VALIDATE(_id)                           \
    do {                                        \
        if(!ONLP_OID_IS_LED(_id)) {             \
            return ONLP_STATUS_E_INVALID;       \
        }                                       \
    } while(0)

#define LED_FORMAT "/sys/bus/i2c/devices/2-0060/ca120-32t16x-led/leds/sys::%s/brightness"

/* LED related data
 */
enum led_light_mode { /*must be the same with the definition @ kernel driver */
    LED_MODE_OFF = 0,
    LED_MODE_GREEN,
    LED_MODE_GREEN_BLINK,
    LED_MODE_AMBER,
    LED_MODE_AMBER_BLINK,
    LED_MODE_RED,
    LED_MODE_RED_BLINK,
    LED_MODE_BLUE,
    LED_MODE_BLUE_BLINK,
    LED_MODE_AUTO,
    LED_MODE_UNKNOWN
};

enum onlp_led_id
{
    LED_LOC = 1,
    LED_FAULT,    
    LED_FAN,
    LED_PSU2,
    LED_PSU1,
    LED_SD_CARD,
};

typedef struct led_light_mode_map {
    enum onlp_led_id id;
    enum led_light_mode driver_led_mode;
    enum onlp_led_mode_e onlp_led_mode;
} led_light_mode_map_t;

led_light_mode_map_t led_map[] = {
{LED_LOC,  LED_MODE_OFF,   ONLP_LED_MODE_OFF},
{LED_LOC,  LED_MODE_AMBER, ONLP_LED_MODE_ORANGE},
{LED_FAN,  LED_MODE_OFF,   ONLP_LED_MODE_OFF},
{LED_FAN,  LED_MODE_RED,   ONLP_LED_MODE_RED},
{LED_FAULT, LED_MODE_OFF,  ONLP_LED_MODE_OFF},
{LED_FAULT, LED_MODE_RED,  ONLP_LED_MODE_RED},
{LED_PSU1, LED_MODE_AUTO,  ONLP_LED_MODE_AUTO},
{LED_PSU2, LED_MODE_AUTO,  ONLP_LED_MODE_AUTO},
{LED_SD_CARD, LED_MODE_OFF,  ONLP_LED_MODE_OFF},
{LED_SD_CARD,  LED_MODE_GREEN,   ONLP_LED_MODE_GREEN},
};

static char *leds[] =  /* must map with onlp_led_id */
{
    NULL,
    "loc",
    "fault",
    "fan",
    "psu2",
    "psu1",
    "sdcard",
};
/*
 * Get the information for the given LED OID.
 */
static onlp_led_info_t linfo[] =
{
    { }, /* Not used */
    {
        { ONLP_LED_ID_CREATE(LED_LOC), "Chassis LED 1 (ID)", 0, {0} },
        ONLP_LED_STATUS_PRESENT,
        ONLP_LED_CAPS_ON_OFF | ONLP_LED_CAPS_ORANGE,
    },
    {
        { ONLP_LED_ID_CREATE(LED_FAULT), "Chassis LED 2 (FAULT)", 0, {0} },
        ONLP_LED_STATUS_PRESENT,
        ONLP_LED_CAPS_ON_OFF | ONLP_LED_CAPS_RED,
    },
    {
        { ONLP_LED_ID_CREATE(LED_FAN), "Chassis LED 3(F/F)", 0, {0} },
        ONLP_LED_STATUS_PRESENT,
        ONLP_LED_CAPS_ON_OFF | ONLP_LED_CAPS_RED,
    },
    {
        { ONLP_LED_ID_CREATE(LED_PSU2), "Chassis LED 4 (P2)", 0, {0} },
        ONLP_LED_STATUS_PRESENT,
        ONLP_LED_CAPS_ON_OFF | ONLP_LED_CAPS_AUTO,
    },
    {
        { ONLP_LED_ID_CREATE(LED_PSU1), "Chassis LED 5 (P1)", 0, {0} },
        ONLP_LED_STATUS_PRESENT,
        ONLP_LED_CAPS_ON_OFF | ONLP_LED_CAPS_AUTO,
    },
    {
        { ONLP_LED_ID_CREATE(LED_PSU1), "Chassis LED 6 (SDCARD)", 0, {0} },
        ONLP_LED_STATUS_PRESENT,
        ONLP_LED_CAPS_ON_OFF | ONLP_LED_CAPS_GREEN,
    }    
};

/**
 * driver_to_onlp_led_mode - Translate a driver-specific LED mode to an ONLP mode.
 *
 * This helper function maps a numeric mode value from the kernel driver
 * to the corresponding standardized ONLP LED mode enumeration.
 *
 * @id: The specific LED identifier.
 * @driver_led_mode: The mode value used by the driver.
 *
 * @return The corresponding onlp_led_mode_e value, or 0 if no match is found.
 */
static int driver_to_onlp_led_mode(enum onlp_led_id id, enum led_light_mode driver_led_mode)
{
    int i, nsize = sizeof(led_map)/sizeof(led_map[0]);
    
    for (i = 0; i < nsize; i++)
    {
        if (id == led_map[i].id && driver_led_mode == led_map[i].driver_led_mode)
        {
            return led_map[i].onlp_led_mode;
        }
    }
    
    return 0;
}

/**
 * onlp_to_driver_led_mode - Translate an ONLP LED mode to a driver-specific mode.
 *
 * This helper function maps a standardized ONLP LED mode enumeration
 * to the corresponding numeric mode value required by the kernel driver.
 *
 * @id: The specific LED identifier.
 * @onlp_led_mode: The standardized ONLP LED mode.
 *
 * @return The corresponding driver-specific mode value, or 0 if no match is found.
 */
static int onlp_to_driver_led_mode(enum onlp_led_id id, onlp_led_mode_t onlp_led_mode)
{
    int i, nsize = sizeof(led_map)/sizeof(led_map[0]);
    
    for(i = 0; i < nsize; i++)
    {
        if (id == led_map[i].id && onlp_led_mode == led_map[i].onlp_led_mode)
        {
            return led_map[i].driver_led_mode;
        }
    }
    
    return 0;
}

/**
 * onlp_ledi_init - Initialize the LED subsystem.
 *
 * This function is called at initialization time to set up the LED
 * control subsystem. In this implementation, no specific initialization
 * is required.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_ledi_init(void)
{
    return ONLP_STATUS_OK;
}

/**
 * onlp_ledi_info_get - Retrieve information for a specific LED.
 *
 * Fills an onlp_led_info_t structure with information about an LED
 * specified by its OID. It retrieves the LED's capabilities from a
 * static table and reads its current mode from sysfs.
 *
 * @id: The Object ID (OID) of the LED.
 * @info: A pointer to the structure to be filled with LED information.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int
onlp_ledi_info_get(onlp_oid_t id, onlp_led_info_t* info)
{
    int  lid, value;    
    VALIDATE(id);
    
    lid = ONLP_OID_ID_GET(id);

    /* Set the onlp_oid_hdr_t and capabilities */
    *info = linfo[ONLP_OID_ID_GET(id)];

    /* Get LED mode */
    if (onlp_file_read_int(&value, LED_FORMAT, leds[lid]) < 0) {
        return ONLP_STATUS_E_INTERNAL;
    }

    info->mode = driver_to_onlp_led_mode(lid, value);

    /* Set the on/off status */
    if (info->mode != ONLP_LED_MODE_OFF) {
        info->status |= ONLP_LED_STATUS_ON;
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_ledi_set - Turn an LED on or off.
 *
 * This function provides a basic interface to turn an LED on or off.
 * 'Off' is mapped to ONLP_LED_MODE_OFF. The 'on' state is considered
 * ambiguous for multi-mode LEDs and is unsupported by this function.
 * Use onlp_ledi_mode_set for more specific control.
 *
 * @id: The Object ID (OID) of the LED.
 * @on_or_off: A boolean value; 0 for off, non-zero for on.
 *
 * @return ONLP_STATUS_OK on success (for turning off),
 * ONLP_STATUS_E_UNSUPPORTED for turning on, or a negative status code.
 */
int
onlp_ledi_set(onlp_oid_t id, int on_or_off)
{
    VALIDATE(id);

    if (!on_or_off) {
        return onlp_ledi_mode_set(id, ONLP_LED_MODE_OFF);
    }

    return ONLP_STATUS_E_UNSUPPORTED;
}

/**
 * onlp_ledi_mode_set - Set the operational mode of an LED.
 *
 * This function sets a specific mode (e.g., color, blinking) for an LED.
 * It translates the requested ONLP mode to the corresponding driver value
 * and writes it to the appropriate sysfs file.
 *
 * @id: The Object ID (OID) of the LED.
 * @mode: The desired mode from the onlp_led_mode_t enumeration.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int
onlp_ledi_mode_set(onlp_oid_t id, onlp_led_mode_t mode)
{
    int  lid;
    char path[96] = {0};        

    VALIDATE(id);
    
    lid = ONLP_OID_ID_GET(id);
    sprintf(path, LED_FORMAT, leds[lid]);

    if (onlp_file_write_int(onlp_to_driver_led_mode(lid , mode), path) != 0) {
        return ONLP_STATUS_E_INTERNAL;
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_ledi_ioctl - Generic LED ioctl interface.
 *
 * This function is a placeholder for platform-specific, non-standard
 * LED control operations. It is not implemented on this platform.
 *
 * @id: The Object ID (OID) of the LED.
 * @vargs: A va_list of arguments for the ioctl.
 *
 * @return ONLP_STATUS_E_UNSUPPORTED.
 */
int
onlp_ledi_ioctl(onlp_oid_t id, va_list vargs)
{
    return ONLP_STATUS_E_UNSUPPORTED;
}