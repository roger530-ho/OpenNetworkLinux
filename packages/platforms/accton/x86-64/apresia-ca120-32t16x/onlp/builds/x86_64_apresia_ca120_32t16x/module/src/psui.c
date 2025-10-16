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
#include <onlp/platformi/psui.h>
#include <string.h>
#include "platform_lib.h"

#define PSU_STATUS_PRESENT 1
#define PSU_STATUS_POWER_GOOD 1

#define VALIDATE(_id)                           \
    do {                                        \
        if(!ONLP_OID_IS_PSU(_id)) {             \
            return ONLP_STATUS_E_INVALID;       \
        }                                       \
    } while(0)

/**
 * psu_status_info_get - Get an integer status value from a PSU sysfs node.
 *
 * This is a helper function that constructs the sysfs path for a given PSU
 * and status node, and reads an integer value from it.
 *
 * @id: The PSU identifier (1 or 2).
 * @node: The name of the sysfs file to read (e.g., "psu_present").
 * @value: A pointer to an integer to store the result.
 *
 * @return 0 on success, or a negative value on file read error.
 */
static int
psu_status_info_get(int id, char *node, int *value)
{
    char *path[] = { PSU1_SYSFS_PREFIX, PSU2_SYSFS_PREFIX };
    *value = 0;

    return onlp_file_read_int(value, "%s%s", path[id-1], node);
}

/**
 * onlp_psui_init - Initialize the PSU subsystem.
 *
 * This function is called at initialization time to set up the PSU
 * monitoring subsystem. In this implementation, no specific
 * initialization is required.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_psui_init(void)
{
    return ONLP_STATUS_OK;
}

/**
 * psu_g1394_info_get - Get detailed info for a G1394 model PSU.
 *
 * This helper function retrieves detailed operational data (voltage,
 * current, power, serial number) for a specific PSU model via PMBus.
 * It also populates the OIDs of the components associated with this
 * PSU, such as its fan and thermal sensors.
 *
 * @info: A pointer to the PSU information structure to be filled.
 *
 * @return ONLP_STATUS_OK on success.
 */
static int
psu_g1394_info_get(onlp_psu_info_t* info)
{
    int val   = 0;
    int index = ONLP_OID_ID_GET(info->hdr.id);

    /* Set capability
     */
    info->caps = ONLP_PSU_CAPS_AC;

    if (info->status & ONLP_PSU_STATUS_FAILED)
        return ONLP_STATUS_OK;

    /* Set the associated oid_table */
    info->hdr.coids[0] = ONLP_FAN_ID_CREATE(index + CHASSIS_FAN_COUNT);

    info->hdr.coids[1] = ONLP_THERMAL_ID_CREATE((index-1) * 
                        NUM_OF_THERMAL_PER_PSU 
                        + CHASSIS_THERMAL_COUNT
                        + 1);

    info->hdr.coids[2] = ONLP_THERMAL_ID_CREATE((index-1) * 
                        NUM_OF_THERMAL_PER_PSU 
                        + CHASSIS_THERMAL_COUNT
                        + 2);

    info->hdr.coids[3] = ONLP_THERMAL_ID_CREATE((index-1) * 
                        NUM_OF_THERMAL_PER_PSU 
                        + CHASSIS_THERMAL_COUNT
                        + 3);
    /* Read voltage, current and power */
    if (psu_pmbus_info_int_get(index, "psu_v_out", &val) == 0) {
        info->mvout = val;
        info->caps |= ONLP_PSU_CAPS_VOUT;
    }
    if (psu_pmbus_info_int_get(index, "psu_v_in", &val) == 0) {
        info->mvin  = val;
        info->caps |= ONLP_PSU_CAPS_VIN;
    }
    
    if (psu_pmbus_info_int_get(index, "psu_i_out", &val) == 0) {
        info->miout = val;
        info->caps |= ONLP_PSU_CAPS_IOUT;
    }
    if (psu_pmbus_info_int_get(index, "psu_i_in", &val) == 0) {
        info->miin  = val;
        info->caps |= ONLP_PSU_CAPS_IIN;
    }
    
    if (psu_pmbus_info_int_get(index, "psu_p_out", &val) == 0) {
        info->mpout = val;
        info->caps |= ONLP_PSU_CAPS_POUT;
    }
    if (psu_pmbus_info_int_get(index, "psu_p_in", &val) == 0) {
        info->mpin  = val;
        info->caps |= ONLP_PSU_CAPS_PIN;
    }
    
    psu_pmbus_info_str_get(index, "psu_mfr_serial", info->serial, sizeof(info->serial));

    return ONLP_STATUS_OK;
}

/*
 * Get all information about the given PSU oid.
 */
static onlp_psu_info_t pinfo[] =
{
    { }, /* Not used */
    {
        { ONLP_PSU_ID_CREATE(PSU1_ID), "PSU-1", 0 },
    },
    {
        { ONLP_PSU_ID_CREATE(PSU2_ID), "PSU-2", 0 },
    }
};

/**
 * onlp_psui_info_get - Retrieve information for a specific PSU.
 *
 * Fills an onlp_psu_info_t structure with information about a PSU
 * specified by its OID. It first checks for presence and power status.
 * If the PSU is present and powered, it identifies the model and calls
 * a model-specific helper function to retrieve detailed telemetry data.
 *
 * @id: The Object ID (OID) of the PSU.
 * @info: A pointer to the structure to be filled with PSU information.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int
onlp_psui_info_get(onlp_oid_t id, onlp_psu_info_t* info)
{
    int val   = 0;
    int ret   = ONLP_STATUS_OK;
    int index = ONLP_OID_ID_GET(id);
    psu_type_t psu_type;

    VALIDATE(id);

    memset(info, 0, sizeof(onlp_psu_info_t));
    *info = pinfo[index]; /* Set the onlp_oid_hdr_t */

    /* Get the present state */
    if (psu_status_info_get(index, "psu_present", &val) != 0) {
        AIM_LOG_ERROR("Unable to read PSU(%d) node(psu_present)\r\n", index);
        info->status &= ~ONLP_PSU_STATUS_PRESENT;
        return ONLP_STATUS_E_INTERNAL;
    }

    if (val != PSU_STATUS_PRESENT) {
        info->status &= ~ONLP_PSU_STATUS_PRESENT;
        return ONLP_STATUS_OK;
    }
    info->status |= ONLP_PSU_STATUS_PRESENT;


    /* Get power good status */
    if (psu_status_info_get(index, "psu_power_good", &val) != 0) {
        AIM_LOG_ERROR("Unable to read PSU(%d) node(psu_power_good)\r\n", index);
        return ONLP_STATUS_E_INTERNAL;
    }
 
    if (val != PSU_STATUS_POWER_GOOD) {
        info->status |=  ONLP_PSU_STATUS_UNPLUGGED;
    } else {
        info->caps = ONLP_PSU_CAPS_AC;
    }
   
    /* Get PSU type */
    psu_type = get_psu_type(index, info->model, sizeof(info->model));
    switch (psu_type) {
        case PSU_TYPE_AC_G1394_F2B:
            ret = psu_g1394_info_get(info);
            break;
        case PSU_TYPE_UNKNOWN:  /* User insert a unknown PSU or unplugged.*/
            /* Set the associated oid_table */
            info->hdr.coids[0] = ONLP_FAN_ID_CREATE(index + CHASSIS_FAN_COUNT);
            info->hdr.coids[1] = ONLP_THERMAL_ID_CREATE(CHASSIS_THERMAL_COUNT + (index-1)*NUM_OF_THERMAL_PER_PSU + 1);
            info->hdr.coids[2] = ONLP_THERMAL_ID_CREATE(CHASSIS_THERMAL_COUNT + (index-1)*NUM_OF_THERMAL_PER_PSU + 2);
            info->hdr.coids[3] = ONLP_THERMAL_ID_CREATE(CHASSIS_THERMAL_COUNT + (index-1)*NUM_OF_THERMAL_PER_PSU + 3);
            if (val == PSU_STATUS_POWER_GOOD) {
                info->status |= ONLP_PSU_STATUS_FAILED;
            }
            ret = ONLP_STATUS_OK;
            break;
        default:
            ret = ONLP_STATUS_E_UNSUPPORTED;
            break;
    }

    return ret;
}
