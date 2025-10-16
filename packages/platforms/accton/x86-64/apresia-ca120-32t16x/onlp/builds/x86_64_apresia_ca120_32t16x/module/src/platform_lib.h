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
#ifndef __PLATFORM_LIB_H__
#define __PLATFORM_LIB_H__

#include <onlplib/file.h>
#include "x86_64_apresia_ca120_32t16x_log.h"

#define CHASSIS_FAN_COUNT       4
#define CHASSIS_THERMAL_COUNT   5
#define CHASSIS_PSU_COUNT       2
#define CHASSIS_LED_COUNT       6

#define NUM_OF_THERMAL_PER_PSU 3

#define PSU1_ID 1
#define PSU2_ID 2

#define PSU1_SYSFS_PREFIX "/sys/bus/i2c/devices/2-0060/ca120-32t16x-psu1*"
#define PSU2_SYSFS_PREFIX "/sys/bus/i2c/devices/2-0060/ca120-32t16x-psu2*"
#define PSU1_PMBUS_FORMAT "/sys/bus/i2c/devices/37-0059/hwmon/hwmon%d/"
#define PSU2_PMBUS_FORMAT "/sys/bus/i2c/devices/37-0058/hwmon/hwmon%d/"
#define PSU1_PMBUS_PREFIX "/sys/bus/i2c/devices/37-0059/"
#define PSU2_PMBUS_PREFIX "/sys/bus/i2c/devices/37-0058/"

#define FAN_NODE_PATH    "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fan*"
#define FAN_NODE(node)   FAN_NODE_PATH#node

#define IDPROM_PATH1 "/sys/bus/i2c/devices/39-0050/eeprom"
#define IDPROM_PATH2 "/sys/bus/i2c/devices/39-0051/eeprom"

int psu_pmbus_info_int_get(int id, char *node, int *value);
int psu_pmbus_info_int_set(int id, char *node, int value);
int psu_pmbus_info_str_get(int id, char *node, char *data_buf, int data_len);

enum onlp_thermal_id
{
    THERMAL_RESERVED = 0,
    THERMAL_CPU_CORE,
    THERMAL_1_ON_MAIN_BROAD,
    THERMAL_2_ON_MAIN_BROAD,
    THERMAL_3_ON_FAN_BROAD,
    THERMAL_4_ON_CPU_BROAD,
    THERMAL_1_ON_PSU1,
    THERMAL_2_ON_PSU1,
    THERMAL_3_ON_PSU1,
    THERMAL_1_ON_PSU2,
    THERMAL_2_ON_PSU2,
    THERMAL_3_ON_PSU2,
};

typedef enum psu_type {
    PSU_TYPE_UNKNOWN,
    PSU_TYPE_AC_G1394_F2B,
    PSU_TYPE_AC_B2F
} psu_type_t;

psu_type_t get_psu_type(int id, char* modelname, int modelname_len);

#define AIM_FREE_IF_PTR(p) \
	do \
	{ \
		if (p) { \
			aim_free(p); \
			p = NULL; \
		} \
	} while (0)

#define DEBUG_MODE 0

#if (DEBUG_MODE == 1)
    #define DEBUG_PRINT(fmt, args...)                                        \
        printf("%s:%s[%d]: " fmt "\r\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
    #define DEBUG_PRINT(fmt, args...)
#endif

#endif  /* __PLATFORM_LIB_H__ */
