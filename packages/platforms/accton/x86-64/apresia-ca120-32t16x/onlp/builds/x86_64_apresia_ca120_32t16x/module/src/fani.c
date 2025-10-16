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
 * Fan Platform Implementation Defaults.
 *
 ***********************************************************/
#include <onlplib/file.h>
#include <onlp/platformi/fani.h>
#include "platform_lib.h"

#define PSU_PREFIX_PATH  "/sys/bus/i2c/devices/"

#define MAX_FAN_SPEED     23500

enum fan_id {
    FAN_1_ON_FAN_BOARD = 1,
    FAN_2_ON_FAN_BOARD,
    FAN_3_ON_FAN_BOARD,
    FAN_4_ON_FAN_BOARD,
    FAN_1_ON_PSU_1,
    FAN_1_ON_PSU_2,
};

#define CHASSIS_FAN_INFO(fid) { \
        { ONLP_FAN_ID_CREATE(FAN_##fid##_ON_FAN_BOARD), "Chassis Fan - "#fid, 0, {0} },\
        0x0, \
        ONLP_FAN_CAPS_SET_PERCENTAGE | \
        ONLP_FAN_CAPS_GET_RPM | ONLP_FAN_CAPS_GET_PERCENTAGE, \
        0, \
        0, \
        ONLP_FAN_MODE_INVALID, \
    }

#define PSU_FAN_INFO(pid, fid) { \
        { ONLP_FAN_ID_CREATE(FAN_##fid##_ON_PSU_##pid), "PSU "#pid" - Fan "#fid, 0, {0} },\
        0x0,\
        ONLP_FAN_CAPS_GET_RPM | ONLP_FAN_CAPS_GET_PERCENTAGE, \
        0,\
        0,\
        ONLP_FAN_MODE_INVALID,\
    }

/* Static fan information */
onlp_fan_info_t finfo[] = {
    { }, /* Not used */
    CHASSIS_FAN_INFO(1),
    CHASSIS_FAN_INFO(2),
    CHASSIS_FAN_INFO(3),
    CHASSIS_FAN_INFO(4),
    PSU_FAN_INFO(1,1),
    PSU_FAN_INFO(2,1)
};

#define VALIDATE(_id)                           \
    do {                                        \
        if(!ONLP_OID_IS_FAN(_id)) {             \
            return ONLP_STATUS_E_INVALID;       \
        }                                       \
    } while(0)

/**
 * _onlp_fani_info_get_fan - Get information for a chassis fan.
 *
 * This is a helper function that retrieves the status, speed (RPM), and
 * duty cycle (percentage) for a specific chassis fan by reading from
 * sysfs nodes.
 *
 * @fid: The fan identifier.
 * @info: A pointer to the fan information structure to be filled.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
static int
_onlp_fani_info_get_fan(int fid, onlp_fan_info_t* info)
{
    int value;

    /* get fan present status
     */
    info->status |= ONLP_FAN_STATUS_PRESENT;

    /* get fan fault status (turn on when any one fails)
     */
    if (onlp_file_read_int(&value, "%s""fan%d_fault", FAN_NODE_PATH, fid) < 0) {
        AIM_LOG_ERROR("Unable to read fan fault status from (%s)\r\n", 
                FAN_NODE_PATH);
        return ONLP_STATUS_E_INTERNAL;
    }
    if (value > 0)
        info->status |= ONLP_FAN_STATUS_FAILED;


    /* get fan direction (both : the same)
     */
    info->status |= ONLP_FAN_STATUS_F2B;


    /* get fan speed
     */
    if (onlp_file_read_int(&value, "%s""fan%d_input", FAN_NODE_PATH, fid) < 0) {
        AIM_LOG_ERROR("Unable to read fan speed from (%s)\r\n", FAN_NODE_PATH);

        return ONLP_STATUS_E_INTERNAL;
    }
    info->rpm = value;

    /* get pwm percentage
     */
    if (onlp_file_read_int(&value, "%s""fan_pwm", FAN_NODE_PATH) < 0) {
        AIM_LOG_ERROR("Unable to read fan duty from (%s)\r\n", FAN_NODE_PATH);
        return ONLP_STATUS_E_INTERNAL;
    }
    info->percentage = value;

    return ONLP_STATUS_OK;
}

/**
 * _onlp_get_fan_direction_on_psu - Determine the airflow direction of a PSU fan.
 *
 * This helper function checks the PSU type to determine its fan's airflow
 * direction (Front-to-Back or Back-to-Front).
 *
 * @pid: The PSU identifier.
 *
 * @return The fan direction status flag (e.g., ONLP_FAN_STATUS_F2B) or 0
 * if the PSU type is unknown.
 */
static uint32_t
_onlp_get_fan_direction_on_psu(int pid)
{
    psu_type_t psu_type;

    psu_type = get_psu_type(pid, NULL, 0);
    switch (psu_type) {
        case PSU_TYPE_AC_G1394_F2B:
            return ONLP_FAN_STATUS_F2B;
        case PSU_TYPE_UNKNOWN:
            return 0;
        default:
            return ONLP_FAN_STATUS_B2F;
    }
}

/**
 * _onlp_fani_info_get_fan_on_psu - Get information for a PSU fan.
 *
 * This is a helper function that retrieves the status, direction, and speed
 * (RPM) for a fan located within a specific PSU.
 *
 * @pid: The PSU identifier.
 * @info: A pointer to the fan information structure to be filled.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
static int
_onlp_fani_info_get_fan_on_psu(int pid, onlp_fan_info_t* info)
{
    int val = 0;
    int psu_type;

    info->status |= ONLP_FAN_STATUS_PRESENT;

    /* get fan direction
     */
    info->status |= _onlp_get_fan_direction_on_psu(pid);

    /* get fan fault status
     */
    if (psu_pmbus_info_int_get(pid, "psu_fan1_fault", &val) == ONLP_STATUS_OK)
        info->status |= (val > 0) ? ONLP_FAN_STATUS_FAILED : 0;

    /* get fan speed
     */
    if (psu_pmbus_info_int_get(pid, "psu_fan1_speed_rpm", &val) == ONLP_STATUS_OK) {
        info->rpm = val;
    }

    /* Get PSU type */
    psu_type = get_psu_type(pid, NULL, 0);
    if (psu_type == PSU_TYPE_UNKNOWN) {
        /*
         * For display all PSU information that includes the 
         * fan and temperature, even access hardware fail.
         */
        info->status |= ONLP_FAN_STATUS_FAILED;
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_fani_init - Initialize the fan subsystem.
 *
 * This function is called at initialization time to set up the fan
 * monitoring and control subsystem. In this implementation, no specific
 * initialization is required.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_fani_init(void)
{
    return ONLP_STATUS_OK;
}

/**
 * onlp_fani_info_get - Retrieve information for a specific fan.
 *
 * Fills an onlp_fan_info_t structure with information about a fan
 * specified by its OID. It dispatches the request to the appropriate
 * helper function based on whether the fan is a chassis fan or a PSU fan.
 *
 * @id: The Object ID (OID) of the fan.
 * @info: A pointer to the structure to be filled with fan information.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int
onlp_fani_info_get(onlp_oid_t id, onlp_fan_info_t* info)
{
    int rc = 0;
    int fid;

    VALIDATE(id);

    fid = ONLP_OID_ID_GET(id);
    *info = finfo[fid];

    switch (fid) {
        case FAN_1_ON_PSU_1:
            rc = _onlp_fani_info_get_fan_on_psu(PSU1_ID, info);
            break;
        case FAN_1_ON_PSU_2:
            rc = _onlp_fani_info_get_fan_on_psu(PSU2_ID, info);
            break;
        case FAN_1_ON_FAN_BOARD:
        case FAN_2_ON_FAN_BOARD:
        case FAN_3_ON_FAN_BOARD:
        case FAN_4_ON_FAN_BOARD:
            rc =_onlp_fani_info_get_fan(fid, info);
            break;
        default:
            rc = ONLP_STATUS_E_INVALID;
            break;
    }

    return rc;
}

/**
 * onlp_fani_rpm_set - Set the speed of a fan in RPM.
 *
 * This function is intended to set the fan speed in revolutions per minute.
 * It will only be called if the fan reports the ONLP_FAN_CAPS_SET_RPM
 * capability. This feature is not supported on this platform.
 *
 * @id: The Object ID (OID) of the fan.
 * @rpm: The desired speed in RPM.
 *
 * @return ONLP_STATUS_E_UNSUPPORTED.
 */
int
onlp_fani_rpm_set(onlp_oid_t id, int rpm)
{
    return ONLP_STATUS_E_UNSUPPORTED;
}

/**
 * onlp_fani_percentage_set - Set the fan speed as a percentage of its maximum.
 *
 * This function sets the duty cycle of the fan's PWM signal. It will only
 * be called if the fan reports the ONLP_FAN_CAPS_SET_PERCENTAGE capability.
 * A value of 0 is rejected to prevent stopping the fan completely.
 *
 * @id: The Object ID (OID) of the fan.
 * @p: The desired speed as a percentage (1-100).
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int
onlp_fani_percentage_set(onlp_oid_t id, int p)
{
    int  fid;
    char *path = NULL;

    VALIDATE(id);

    fid = ONLP_OID_ID_GET(id);

    /* reject p=0 (p=0, stop fan) */
    if (p == 0)
        return ONLP_STATUS_E_INVALID;

    switch (fid) {
        case FAN_1_ON_FAN_BOARD:
        case FAN_2_ON_FAN_BOARD:
        case FAN_3_ON_FAN_BOARD:
        case FAN_4_ON_FAN_BOARD:
            path = FAN_NODE(fan_pwm);
            break;
        default:
            return ONLP_STATUS_E_INVALID;
    }

    if (onlp_file_write_int(p, path) < 0) {
        AIM_LOG_ERROR("Unable to write data to file (%s)\r\n", path);
        return ONLP_STATUS_E_INTERNAL;
    }

    return ONLP_STATUS_OK;
}
