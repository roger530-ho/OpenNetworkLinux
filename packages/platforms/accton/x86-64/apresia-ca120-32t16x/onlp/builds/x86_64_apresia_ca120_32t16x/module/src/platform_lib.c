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
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <onlplib/file.h>
#include <onlp/onlp.h>
#include "platform_lib.h"

#define PSU_NODE_MAX_PATH_LEN   64
#define PSU_FAN_DIR_LEN         3
#define PSU_MODEL_NAME_LEN      14
#define PSU_SERIAL_NUMBER_LEN   19

/**
 * onlp_get_psu_hwmon_idx - Get the hwmon index for a given PSU.
 *
 * This function locates the hwmon index for a PSU by scanning its
 * device directory in sysfs. The hwmon index (e.g., the 'N' in 'hwmonN')
 * is not fixed and must be discovered at runtime.
 *
 * @id: The PSU identifier (PSU1_ID or PSU2_ID).
 *
 * @return The discovered hwmon index on success, or -1 on failure.
 */
int onlp_get_psu_hwmon_idx(int id)
{
    /* Locate the hwmon index by enumerating the PSU device's own hwmon/ dir.
     * Example layout:
     * <PSU*_PMBUS_PREFIX>/hwmon/hwmonN/{name, device, ...}
     * We parse "hwmonN" -> return N.
     */
    char base[PSU_NODE_MAX_PATH_LEN];
    DIR* d;
    struct dirent* de;
    int idx = -1;

    (void)snprintf(base, sizeof(base), "%s%s",
                   (id == PSU1_ID) ? PSU1_PMBUS_PREFIX : PSU2_PMBUS_PREFIX,
                   "hwmon");

    d = opendir(base);
    if (d == NULL) {
        return -1;
    }

    while ((de = readdir(d)) != NULL) {
        /* Skip "." and ".." and other non-matching entries. */
        if (de->d_name[0] == '.') {
            continue;
        }
        if (strncmp(de->d_name, "hwmon", 5) == 0) {
            /* de->d_name is like "hwmon7" -> parse 7 */
            idx = atoi(de->d_name + 5);
            break;
        }
    }

    closedir(d);
    return idx;
}

/**
 * psu_pmbus_info_int_get - Read an integer value from a PSU PMBus sysfs node.
 *
 * This function reads an integer value from a specified PMBus attribute
 * for a given PSU. It dynamically discovers the PSU's hwmon index to
 * construct the correct file path.
 *
 * @id: The PSU identifier.
 * @node: The name of the sysfs file within the hwmon directory.
 * @value: A pointer to an integer to store the read value.
 *
 * @return 0 on success, or a negative ONLP status code on error.
 */
int psu_pmbus_info_int_get(int id, char *node, int *value)
{
    int  ret = 0;
    int hwmon_idx;
    
    *value = 0;
    hwmon_idx = onlp_get_psu_hwmon_idx(id);
    if (hwmon_idx < 0) {
        return ONLP_STATUS_E_INVALID;
    }
    
    if (PSU1_ID == id) {
        ret = onlp_file_read_int(value, PSU1_PMBUS_FORMAT"%s", hwmon_idx, node);
    }
    else {
        ret = onlp_file_read_int(value, PSU2_PMBUS_FORMAT"%s", hwmon_idx, node);
    }

    if (ret < 0) {
        return ONLP_STATUS_E_INTERNAL;
    }

    return ret;
}

/**
 * psu_pmbus_info_str_get - Read a string from a PSU PMBus sysfs node.
 *
 * This function reads a string value from a specified PMBus attribute
 * for a given PSU. It dynamically discovers the hwmon index and copies
 * the string content into the provided buffer.
 *
 * @id: The PSU identifier.
 * @node: The name of the sysfs file to read.
 * @data_buf: The output buffer to store the string.
 * @data_len: The size of the output buffer.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int psu_pmbus_info_str_get(int id, char* node, char* data_buf, int data_len)
{
    int len = 0;
    char path[PSU_NODE_MAX_PATH_LEN];
    char* string = NULL;
    int hwmon_idx;

    if (node == NULL || data_buf == NULL || data_len <= 0) {
        return ONLP_STATUS_E_PARAM;
    }

    hwmon_idx = onlp_get_psu_hwmon_idx(id);
    if (hwmon_idx < 0) {
        return ONLP_STATUS_E_INVALID;
    }

    (void)snprintf(path, sizeof(path),
                   (id == PSU1_ID) ? PSU1_PMBUS_FORMAT "%s"
                                   : PSU2_PMBUS_FORMAT "%s",
                   hwmon_idx, node);

    len = onlp_file_read_str(&string, path);
    if (string == NULL || len <= 0) {
        AIM_FREE_IF_PTR(string);
        return ONLP_STATUS_E_INTERNAL;
    }

    if (len >= data_len) {
        AIM_FREE_IF_PTR(string);
        return ONLP_STATUS_E_INVALID;
    }

    aim_strlcpy(data_buf, string, data_len);
    AIM_FREE_IF_PTR(string);
    return ONLP_STATUS_OK;
}

/**
 * psu_pmbus_info_int_set - Write an integer value to a PSU PMBus sysfs node.
 *
 * This function writes an integer value to a specified PMBus attribute
 * for a given PSU. It dynamically discovers the PSU's hwmon index to
 * construct the correct file path.
 *
 * @id: The PSU identifier.
 * @node: The name of the sysfs file to write to.
 * @value: The integer value to write.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP status code on error.
 */
int psu_pmbus_info_int_set(int id, char *node, int value)
{
    char path[PSU_NODE_MAX_PATH_LEN] = {0};
    int hwmon_idx;
    
    hwmon_idx = onlp_get_psu_hwmon_idx(id);
    if (hwmon_idx < 0) {
        return ONLP_STATUS_E_INVALID;
    }
    
    switch (id) {
    case PSU1_ID:
        sprintf(path, PSU1_PMBUS_FORMAT"%s", hwmon_idx, node);
        break;
    case PSU2_ID:
        sprintf(path, PSU2_PMBUS_FORMAT"%s", hwmon_idx, node);
        break;
    default:
        return ONLP_STATUS_E_UNSUPPORTED;
    };

    if (onlp_file_write_int(value, path) != 0) {
        AIM_LOG_ERROR("Unable to write data to file (%s)\r\n", path);
        return ONLP_STATUS_E_INTERNAL;
    }

    return ONLP_STATUS_OK;
}

/**
 * get_psu_type - Determine the type of a PSU.
 *
 * This function identifies the PSU model by reading its manufacturer model
 * string via a PMBus sysfs node. It returns an enumeration value
 * corresponding to the identified model.
 *
 * @id: The PSU identifier.
 * @data_buf: An optional buffer to store the raw model name string.
 * @data_len: The size of the data_buf buffer.
 *
 * @return A psu_type_t enumeration value, with PSU_TYPE_UNKNOWN for
 * unidentified models or read errors.
 */
psu_type_t get_psu_type(int id, char *data_buf, int data_len)
{
	int ret = 0;
	char str[PSU_MODEL_NAME_LEN];
	psu_type_t ptype = PSU_TYPE_UNKNOWN;
   
    ret = psu_pmbus_info_str_get(id, "psu_mfr_model", str, PSU_MODEL_NAME_LEN);
	if (ret < 0) {
		return PSU_TYPE_UNKNOWN;
	}

	/* Check AC model name */
	if (strncmp(str, "G1394-0450WNA", strlen("G1394-0450WNA")) == 0) {
		ptype = PSU_TYPE_AC_G1394_F2B;
    }
	else {
		ptype = PSU_TYPE_UNKNOWN;
    }

    if (data_buf != NULL && data_len > 0) {
        aim_strlcpy(data_buf, str, data_len);
    }

	return ptype;
}