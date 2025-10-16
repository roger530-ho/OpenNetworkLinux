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
#include <unistd.h>
#include <limits.h>
#include <onlp/platformi/sysi.h>
#include <onlp/platformi/ledi.h>
#include <onlp/platformi/thermali.h>
#include <onlp/platformi/fani.h>
#include <onlp/platformi/psui.h>
#include "platform_lib.h"

#define BIOS_VER_PATH "/sys/devices/virtual/dmi/id/bios_version"
#define PREFIX_PATH_ON_CPLD_DEV "/sys/bus/i2c/devices/2-0060/"
#define REFERENCE_THERMAL_ID (ONLP_THERMAL_ID_CREATE(THERMAL_2_ON_MAIN_BROAD)) /* LM77-2-49 */

/* Fan speed levels corresponding to thermal conditions */
enum {
    LEVEL_FAN_0 = 0,
    LEVEL_FAN_1,
    LEVEL_FAN_2,
    LEVEL_FAN_3,
    LEVEL_FAN_MAX,
    LEVEL_FAN_TOTAL
};

/* Fan control policy: maps temperature thresholds to fan duty and states */
typedef struct fan_ctrl_policy {
    int duty_cycle;
    int temp_threshold_high;
    int temp_threshold_low;
} fan_ctrl_policy_t;

/* Fan control policy table */
fan_ctrl_policy_t thermal_policy[] = {
    /* duty,  high (rise enter), low (fall leave) */
    [LEVEL_FAN_0]      = {30,   0,     INT_MIN}, /* 30%: fall leave to lower N/A */
    [LEVEL_FAN_1]      = {40, 39000,   35000},   /* 40%: rise@39C, fall@35C */
    [LEVEL_FAN_2]      = {50, 43000,   39000},   /* 50%: rise@43C, fall@39C */
    [LEVEL_FAN_3]      = {60, 48000,   44000},   /* 60%: rise@48C, fall@44C */
    [LEVEL_FAN_MAX]    = {100, 52500,   48500},  /*100%: rise@52.5C, fall@48.5C */
};

typedef struct {
    const char* name;
    const char* path;
} cpld_version_entry_t;

enum {
    CPLD_IDX_FPGA = 0,
    CPLD_IDX_CPLD,
    NUM_OF_CPLD_VER
};

static const cpld_version_entry_t cpld_versions[NUM_OF_CPLD_VER] = {
    { "FPGA",  "/sys/bus/i2c/devices/2-0060/cpld_version"},
    { "CPLD",  "/sys/bus/i2c/devices/2-0064/cpld_version"},
};

/**
 * onlp_sysi_platform_get - Get the platform name string.
 *
 * Returns a constant string that identifies the hardware platform.
 *
 * @return A pointer to a null-terminated string containing the platform name.
 */
const char*
onlp_sysi_platform_get(void)
{
    return "x86-64-apresia-ca120-32t16x-r0";
}

/**
 * onlp_sysi_onie_data_get - Read the ONIE TLV information data.
 *
 * Reads the ONIE (Open Network Install Environment) data, which is split
 * across two separate sysfs files. It allocates a 512-byte buffer, reads
 * 256 bytes from the first file into the first half of the buffer, and 256
 * bytes from the second file into the second half. The caller is
 * responsible for freeing the allocated buffer using onlp_sysi_onie_data_free.
 *
 * @data: A pointer to a uint8_t* that will be updated to point to the
 * newly allocated buffer containing the ONIE data.
 * @size: A pointer to an integer that will be set to the size of the
 * data (512 bytes) on success.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int
onlp_sysi_onie_data_get(uint8_t** data, int* size)
{
    uint8_t* rdata;
    int sz;

    if (data == NULL || size == NULL) {
        return ONLP_STATUS_E_PARAM;
    }

    *data = NULL;
    *size = 0;

    rdata = aim_zmalloc(512);
    if (rdata == NULL) {
        return ONLP_STATUS_E_INTERNAL;
    }

    /* Read first 256 bytes */
    if (onlp_file_read(rdata, 256, &sz, IDPROM_PATH1) != ONLP_STATUS_OK || sz != 256) {
        aim_free(rdata);
        return ONLP_STATUS_E_INTERNAL;
    }

    /* Read next 256 bytes into the second half */
    if (onlp_file_read(rdata + 256, 256, &sz, IDPROM_PATH2) != ONLP_STATUS_OK || sz != 256) {
        aim_free(rdata);
        return ONLP_STATUS_E_INTERNAL;
    }

    *data = rdata;
    *size = 512;
    return ONLP_STATUS_OK;
}

/**
 * onlp_sysi_onie_data_free - Free the memory allocated for ONIE data.
 *
 * Frees the buffer that was allocated by onlp_sysi_onie_data_get.
 *
 * @data: The pointer to the buffer to be freed.
 */
void
onlp_sysi_onie_data_free(uint8_t* data)
{
    AIM_FREE_IF_PTR(data);
}

/**
 * onlp_sysi_oids_get - Get a list of all component Object IDs (OIDs) on the platform.
 *
 * Populates a provided table with the OIDs for all manageable components
 * on the system, including thermal sensors, LEDs, PSUs, and fans.
 *
 * @table: A pointer to an array of onlp_oid_t to be filled with the OIDs.
 * @max: The maximum number of OIDs the table can hold.
 *
 * @return 0 on success.
 */
int
onlp_sysi_oids_get(onlp_oid_t* table, int max)
{
    int i;
    onlp_oid_t* e = table;
    memset(table, 0, max*sizeof(onlp_oid_t));

    /* 5 Thermal sensors on the chassis */
    for (i = 1; i <= CHASSIS_THERMAL_COUNT; i++) {
        *e++ = ONLP_THERMAL_ID_CREATE(i);
    }

    /* 6 LEDs on the chassis */
    for (i = 1; i <= CHASSIS_LED_COUNT; i++) {
        *e++ = ONLP_LED_ID_CREATE(i);
    }

    /* 2 PSUs on the chassis */
    for (i = 1; i <= CHASSIS_PSU_COUNT; i++) {
        *e++ = ONLP_PSU_ID_CREATE(i);
    }

    /* 4 Fans on the chassis */
    for (i = 1; i <= CHASSIS_FAN_COUNT; i++) {
        *e++ = ONLP_FAN_ID_CREATE(i);
    }

    return 0;
}

/**
 * convert_version_format - Convert a decimal version string to hexadecimal format.
 *
 * Takes an input string in decimal format, such as "M.N" or "M", and converts
 * it to an uppercase hexadecimal string like "MM.NN" or "MM". If the input
 * format is invalid, the output will be "ERR".
 *
 * @input: The null-terminated input string containing the decimal version.
 * @output: A character buffer to store the formatted hexadecimal output string.
 * @output_size: The size of the output buffer.
 */
void convert_version_format(const char* input, char* output, size_t output_size)
{
    int major, minor;

    if (output == NULL || output_size == 0) {
        return;
    }
    output[0] = '\0';

    if (input != NULL && sscanf(input, "%d.%d", &major, &minor) == 2) {
        snprintf(output, output_size, "%02X.%02X", major, minor);
    } else if (input != NULL && sscanf(input, "%d", &major) == 1) {
        snprintf(output, output_size, "%02X", major);
    } else {
        snprintf(output, output_size, "ERR");
    }
}

/**
 * onlp_sysi_platform_info_get - Get detailed platform information.
 *
 * Gathers various version strings and platform details. It reads and
 * formats CPLD and FPGA versions, reads the BIOS version, and reads and
 * decodes the ONIE information. This collected information is formatted
 * into strings and stored in the onlp_platform_info_t structure. The
 * caller must free the allocated strings using onlp_sysi_platform_info_free.
 *
 * @pi: A pointer to an onlp_platform_info_t structure to be filled.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int
onlp_sysi_platform_info_get(onlp_platform_info_t* pi)
{
    int i, len;
    int rc = ONLP_STATUS_E_INTERNAL;
    uint8_t* rdata = NULL;
    int rdata_size = 0;
    char* v[NUM_OF_CPLD_VER] = { 0 };
    char  n[NUM_OF_CPLD_VER][8]; /* "MM.NN\0" fits */
    onlp_onie_info_t onie;
    char* bios_ver = NULL;

    memset(n, 0, sizeof(n));
    memset(&onie, 0, sizeof(onie));

    /* Read and format CPLD versions. Free temporary strings ASAP. */
    for (i = 0; i < AIM_ARRAYSIZE(cpld_versions); i++) {
        len = onlp_file_read_str(&v[i], cpld_versions[i].path);
        if (v[i] != NULL && len > 0) {
            convert_version_format(v[i], n[i], sizeof(n[i]));
        }
        AIM_FREE_IF_PTR(v[i]);
    }

    pi->cpld_versions = aim_fstrdup("\r\n\t   %s: %s"
                                    "\r\n\t   %s: %s",
                                    cpld_versions[CPLD_IDX_FPGA].name, n[CPLD_IDX_FPGA],
                                    cpld_versions[CPLD_IDX_CPLD].name, n[CPLD_IDX_CPLD]);

    /* BIOS version */
    if (onlp_file_read_str(&bios_ver, BIOS_VER_PATH) < 0) {
        goto out;
    }

    /* ONIE blob (512 bytes) and decode */
    if (onlp_sysi_onie_data_get(&rdata, &rdata_size) < 0) {
        goto out;
    }
    if (onlp_onie_decode(&onie, rdata, rdata_size) < 0) {
        goto out;
    }

    pi->other_versions = aim_fstrdup("\r\n\t   BIOS: %s\r\n\t   ONIE: %s",
                                     bios_ver, onie.onie_version);

    rc = ONLP_STATUS_OK;

out:
    onlp_onie_info_free(&onie);
    AIM_FREE_IF_PTR(bios_ver);
    AIM_FREE_IF_PTR(rdata);
    return rc;
}

/**
 * onlp_sysi_platform_info_free - Free memory allocated for platform information.
 *
 * Frees the string members (cpld_versions and other_versions) within an
 * onlp_platform_info_t structure that were allocated by
 * onlp_sysi_platform_info_get.
 *
 * @pi: A pointer to the onlp_platform_info_t structure whose members
 * should be freed.
 */
void
onlp_sysi_platform_info_free(onlp_platform_info_t* pi)
{
    AIM_FREE_IF_PTR(pi->cpld_versions);
    AIM_FREE_IF_PTR(pi->other_versions);
}

/**
 * onlp_sysi_platform_manage_fans - Manage platform fans based on thermal status.
 *
 * Periodically checks chassis fan health and system thermal sensor readings.
 * It calculates the required fan duty cycle based on the reference system
 * temperature, implementing hysteresis by comparing current and previous
 * temperatures against the 'thermal_policy' table.
 *
 * The function forces fans to maximum speed and logs alarms if any
 * fan fails, a sensor read error occurs, or any thermal sensor
 * exceeds its warning/shutdown threshold. A MAC reset is triggered
 * on critical temperature alarms.
 *
 * @return Always returns 0.
 */
int onlp_sysi_platform_manage_fans(void)
{
    static int prev_warning = 0;
    static int prev_sys_temp = 0;
    static int prev_duty_cycle = 50;
    int warning = 0;
    int shutdown = 0;
    int sys_temp = ONLP_STATUS_E_INVALID;
    int fan_status_error = 0, temp_status_error = 0;
    int fan_fail = 0;
    onlp_thermal_info_t thermali[CHASSIS_THERMAL_COUNT-1];
    int i = 0;
    int duty_cycle = 0;

    /* 1. refresh fan status */
    fan_fail = 0;
    fan_status_error = 0;
    for(i = 1; i <= CHASSIS_FAN_COUNT; i++){
        onlp_fan_info_t fan_info;
        if (onlp_fani_info_get(ONLP_FAN_ID_CREATE(i), &fan_info) 
            != ONLP_STATUS_OK) {
            AIM_LOG_WARN("Unable to get fan(%s) status.\r\n", fan_info.hdr.description);
            fan_status_error = 1;
            break;
        }

        if (fan_info.status & ONLP_FAN_STATUS_FAILED || 
            !(fan_info.status & ONLP_FAN_STATUS_PRESENT)) {
            AIM_LOG_WARN("Fan(%s) is not working\r\n", fan_info.hdr.description);
            fan_fail = 1;
            break;
        }
    }

    /* 2. refresh temperature status */
    temp_status_error = 0;
    for(i = 0; i < CHASSIS_THERMAL_COUNT-1; i++){
        if (onlp_thermali_info_get(ONLP_THERMAL_ID_CREATE(i + THERMAL_CPU_CORE), &thermali[i]) 
            != ONLP_STATUS_OK) {
            thermali[i].status = ONLP_STATUS_E_MISSING;
            AIM_LOG_WARN("Unable to read thermal(%s) status.\n\r", thermali[i].hdr.description);
            temp_status_error = 1;
        } else { 
            /* LM77-2-49 */
            if (REFERENCE_THERMAL_ID == thermali[i].hdr.id) {
                sys_temp = thermali[i].mcelsius;
            }
        }
    }

    if(fan_fail || fan_status_error || temp_status_error || sys_temp == ONLP_STATUS_E_INVALID){
        AIM_LOG_WARN("Error occurred while updating fan and thermal status\n\r");
    } else {
        if(prev_sys_temp != sys_temp){
            if (prev_sys_temp < sys_temp) { /* Handle temperature rising scenario */
                AIM_LOG_INFO("Temperature RISING: prev_temp=%d, curr_temp=%d", prev_sys_temp, sys_temp);
                for (i = 0; i < AIM_ARRAYSIZE(thermal_policy); i++) {
                    if (sys_temp >= thermal_policy[i].temp_threshold_high) {
                        duty_cycle = thermal_policy[i].duty_cycle;
                    } else {
                        break;
                    }
                }
                AIM_LOG_INFO("Temperature RISING: Final duty_cycle selected: %d", duty_cycle);
            } else if (prev_sys_temp > sys_temp) { /* Handle temperature falling scenario */
                AIM_LOG_INFO("Temperature FALLING: prev_temp=%d, curr_temp=%d", prev_sys_temp, sys_temp);
                for (i = AIM_ARRAYSIZE(thermal_policy) - 1; i >= 0; i--) {
                    if (sys_temp > thermal_policy[i].temp_threshold_low) {
                        duty_cycle = thermal_policy[i].duty_cycle;
                        break;
                    }
                }
                if (i < 0) {
                    duty_cycle = thermal_policy[0].duty_cycle; /* fallback to lowest */
                }
                AIM_LOG_INFO("Temperature FALLING: Final duty_cycle selected: %d", duty_cycle);
            }
            prev_sys_temp = sys_temp;
        } else {
            duty_cycle = prev_duty_cycle;
        }
    }

    /* 3. check temperature of each thermal sensor */
    for(i = 0; i < CHASSIS_THERMAL_COUNT-1; i++){
        if(thermali[i].status == ONLP_STATUS_E_MISSING) {
            continue;
        }
        if(thermali[i].mcelsius >= thermali[i].thresholds.shutdown){
            warning = 1;
            shutdown = 1;
            AIM_LOG_WARN("thermal(%s) temperature(%d) reach shutdown threshold(%d)\n\r",
                thermali[i].hdr.description, thermali[i].mcelsius, thermali[i].thresholds.shutdown);
        } else if(thermali[i].mcelsius >= thermali[i].thresholds.error){
            warning = 1;
            AIM_LOG_WARN("thermal(%s) temperature(%d) reach high threshold(%d)\n\r",
                thermali[i].hdr.description, thermali[i].mcelsius, thermali[i].thresholds.error);
        }
    }

    /* 4. action */
    if(warning || shutdown || fan_fail || fan_status_error || temp_status_error || sys_temp == ONLP_STATUS_E_INVALID) {
        duty_cycle = thermal_policy[LEVEL_FAN_MAX].duty_cycle;
    }
    if(prev_duty_cycle != duty_cycle) {
        AIM_LOG_INFO("Fan duty cycle changed: %d -> %d", prev_duty_cycle, duty_cycle);
        for(i = 1; i <= CHASSIS_FAN_COUNT; i++){
            onlp_fani_percentage_set(ONLP_FAN_ID_CREATE(i), duty_cycle);
        }
        prev_duty_cycle = duty_cycle;
    }

    if(prev_warning != warning){
        if(warning) {
            AIM_LOG_WARN("Alarm for temperature high is detected\n\r");
        } else {
            AIM_LOG_INFO("Alarm for temperature high is cleared\n\r");
        }
        prev_warning = warning;
    }

    if (warning) {
        AIM_LOG_WARN("Alarm-Critical for temperature critical is detected\n\r");
        AIM_SYSLOG_CRIT("Temperature critical", "Temperature critical", 
                "Alarm-Critical for temperature critical is detected\n\r");
        /* Sync log buffer to disk */
        system("sync;sync;sync");
        system("/sbin/fstrim -av");
    }

    return 0;
}
