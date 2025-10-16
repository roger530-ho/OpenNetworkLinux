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
 * Thermal Sensor Platform Implementation.
 *
 ***********************************************************/
#include <onlplib/file.h>
#include <onlp/platformi/thermali.h>
#include "platform_lib.h"
#include <glob.h>
#include <libgen.h>

#define CPU_CORETEMP_PATH "/sys/devices/platform/coretemp.0/hwmon/hwmon*/temp*_input"
#define CPU_TEMP_SCAN_MAX_FILES 64   /* Maximum number of temperature input files to scan */
#define CPU_TEMP_PATH_MAX_LEN 256    /* Maximum temperature file path length */

#define VALIDATE(_id)                           \
    do {                                        \
        if(!ONLP_OID_IS_THERMAL(_id)) {         \
            return ONLP_STATUS_E_INVALID;       \
        }                                       \
    } while(0)

/*
 * Structure to store temperature sensor file information
 */
typedef struct {
    char path[CPU_TEMP_PATH_MAX_LEN]; /* Full path of the temperature file */
    int index;               /* Index number extracted from the filename */
} temp_entry;

static char* devfiles__[] =     /* must map with onlp_thermal_id */
{
    NULL,
    NULL,   /* CPU_CORE files */
    "/sys/bus/i2c/devices/41-0048*temp1_input",
    "/sys/bus/i2c/devices/41-0049*temp1_input",
    "/sys/bus/i2c/devices/49-004a*temp1_input",
    "/sys/bus/i2c/devices/41-004b*temp1_input",
    "/sys/bus/i2c/devices/37-0059*psu_temp1_input",
    "/sys/bus/i2c/devices/37-0059*psu_temp2_input",
    "/sys/bus/i2c/devices/37-0059*psu_temp3_input",
    "/sys/bus/i2c/devices/37-0058*psu_temp1_input",
    "/sys/bus/i2c/devices/37-0058*psu_temp2_input",
    "/sys/bus/i2c/devices/37-0058*psu_temp3_input"
};

/* Static values */
static onlp_thermal_info_t linfo[] = {
	{ }, /* Not used */
	{ 
        { ONLP_THERMAL_ID_CREATE(THERMAL_CPU_CORE), "CPU Core", 0}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {80000, 110000, 110000}
    },	
	{ 
        { ONLP_THERMAL_ID_CREATE(THERMAL_1_ON_MAIN_BROAD), "LM77-1-48", 0}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {50000, 55000, 81000}
    },
	{ 
        { ONLP_THERMAL_ID_CREATE(THERMAL_2_ON_MAIN_BROAD), "LM77-2-49", 0}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {50000, 62000, 83000}
    },
	{ 
        { ONLP_THERMAL_ID_CREATE(THERMAL_3_ON_FAN_BROAD), "LM77-3-4A", 0}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {50000, 55000, 76000}
    },
	{ 
        { ONLP_THERMAL_ID_CREATE(THERMAL_4_ON_CPU_BROAD), "LM77-4-4B", 0}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {50000, 55000, 88000}
    },
    {   { ONLP_THERMAL_ID_CREATE(THERMAL_1_ON_PSU1), "PSU-1 Thermal Sensor 1", ONLP_PSU_ID_CREATE(PSU1_ID)}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {74000, 80000, 84000}
    },
    {   { ONLP_THERMAL_ID_CREATE(THERMAL_2_ON_PSU1), "PSU-1 Thermal Sensor 2", ONLP_PSU_ID_CREATE(PSU1_ID)}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {74000, 80000, 84000}
    },
    {   { ONLP_THERMAL_ID_CREATE(THERMAL_3_ON_PSU1), "PSU-1 Thermal Sensor 3", ONLP_PSU_ID_CREATE(PSU1_ID)}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {74000, 80000, 84000}
    },
    {   { ONLP_THERMAL_ID_CREATE(THERMAL_1_ON_PSU2), "PSU-2 Thermal Sensor 1", ONLP_PSU_ID_CREATE(PSU2_ID)}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {74000, 80000, 84000}
    },
    {   { ONLP_THERMAL_ID_CREATE(THERMAL_2_ON_PSU2), "PSU-2 Thermal Sensor 2", ONLP_PSU_ID_CREATE(PSU2_ID)}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {74000, 80000, 84000}
    },
    {   { ONLP_THERMAL_ID_CREATE(THERMAL_3_ON_PSU2), "PSU-2 Thermal Sensor 3", ONLP_PSU_ID_CREATE(PSU2_ID)}, 
            ONLP_THERMAL_STATUS_PRESENT,
            ONLP_THERMAL_CAPS_ALL, 0, {74000, 80000, 84000}
    }
};

/**
 * onlp_thermali_init - Initialize the thermal subsystem.
 *
 * This function is called at initialization time to set up the thermal
 * monitoring subsystem. In this implementation, no specific initialization
 * is required.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_thermali_init(void)
{
	return ONLP_STATUS_OK;
}

/**
 * get_temp_from_glob_pattern - Find and sort temperature sensor files using a glob pattern.
 *
 * This function searches the filesystem for temperature sensor sysfs files
 * matching a given glob pattern. It extracts an index number from each
 * filename (e.g., the '2' from 'temp2_input') and populates an array
 * with the full path and index. The function is designed to handle multiple
 * sensor files discovered through a wildcard path.
 *
 * @pattern: The glob pattern to match sensor files.
 * @result: A pre-allocated array of temp_entry structures to store the results.
 * @max_results: The maximum number of entries the result array can hold.
 *
 * @return The number of sensor files found, or 0 on error or if no files are found.
 */
size_t get_temp_from_glob_pattern(const char *pattern, temp_entry *result, size_t max_results) {
	glob_t glob_result;
	size_t i, count = 0;
	int index;

	if (!pattern || !result || max_results == 0) {
		return 0;
	}

	/* Perform glob pattern matching with GLOB_NOSORT for better performance */
	if (glob(pattern, GLOB_NOSORT, NULL, &glob_result) != 0) {
		AIM_LOG_ERROR("Failed to find files matching pattern: %s\n", pattern);
		return 0;
	}

	/* Process each matched file and extract temperature indices */
	for (i = 0; i < glob_result.gl_pathc && count < max_results; i++) {
		char path_buf[CPU_TEMP_PATH_MAX_LEN], *filename;
		const char *full_path = glob_result.gl_pathv[i];

		if (strlen(glob_result.gl_pathv[i]) >= CPU_TEMP_PATH_MAX_LEN) {
			AIM_LOG_ERROR("Path too long (max %d chars): %s\n", 
				      CPU_TEMP_PATH_MAX_LEN - 1, full_path);
			continue;
		}

		snprintf(path_buf, sizeof(path_buf), "%s", full_path);
		filename = basename(path_buf);
		if (filename && sscanf(filename, "temp%d_input", &index) == 1) {
			snprintf(result[count].path, sizeof(result[count].path), "%s", full_path);
			result[count].index = index;
			count++;
		}
	}

	/* Free glob resources */
	globfree(&glob_result);

	return count;
}

/**
 * get_max_cpu_coretemp - Get the maximum temperature among all CPU cores.
 *
 * This function finds all CPU core temperature sysfs files using a
 * predefined glob pattern. It reads the temperature from each file and
 * returns the highest value found. This is used to report a single
 * representative temperature for the CPU.
 *
 * @max_cpu_coretemp: A pointer to an integer where the maximum temperature
 * in millidegrees Celsius will be stored.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int get_max_cpu_coretemp(int *max_cpu_coretemp)
{
	int cpu_coretemp = 0;
	int i = 0;
	size_t count;
	temp_entry temp_entries[CPU_TEMP_SCAN_MAX_FILES] = {0};

	*max_cpu_coretemp = 0;
	count = get_temp_from_glob_pattern(CPU_CORETEMP_PATH, temp_entries, AIM_ARRAYSIZE(temp_entries));
	if (count == 0) {
		AIM_LOG_ERROR("No CPU core temperature sensors found\n");
		return ONLP_STATUS_E_INTERNAL;
	}

	for(i = 0; i < count; i++){
		if(onlp_file_read_int(&cpu_coretemp, temp_entries[i].path) < 0) {
			AIM_LOG_ERROR("Unable to read cpu coretemp from %s\r\n", temp_entries[i].path);
			return ONLP_STATUS_E_INTERNAL;
		}

		if(cpu_coretemp > *max_cpu_coretemp) {
			*max_cpu_coretemp = cpu_coretemp;
		}
	}

	return ONLP_STATUS_OK;
}

/**
 * onlp_thermali_info_get - Retrieve information for a specific thermal sensor.
 *
 * Fills an onlp_thermal_info_t structure with information about a thermal
 * sensor specified by its OID. This includes its description, capabilities,
 * status, thresholds, and current temperature reading. For CPU core sensors,
 * it gets the maximum core temperature. For other sensors, it reads the
 * temperature directly from the corresponding sysfs file.
 *
 * @id: The Object ID (OID) of the thermal sensor.
 * @info: A pointer to the structure to be filled with sensor information.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int
onlp_thermali_info_get(onlp_oid_t id, onlp_thermal_info_t* info)
{
	int tid;
	int psu_id;
	psu_type_t psu_type;
	VALIDATE(id);

	tid = ONLP_OID_ID_GET(id);

	/* Set the onlp_oid_hdr_t and capabilities */
	*info = linfo[tid];

	if ((tid >= THERMAL_1_ON_PSU1) && (tid <= THERMAL_3_ON_PSU2)) {
		psu_id = ((tid - THERMAL_1_ON_PSU1) / NUM_OF_THERMAL_PER_PSU) + 1;

		/* Get PSU type */
		psu_type = get_psu_type(psu_id, NULL, 0);
		if (psu_type == PSU_TYPE_UNKNOWN) {
			/* For display all PSU information that includes the fan and 
                         * temperature, even access hardware fail.
			 */
			info->status |= ONLP_THERMAL_STATUS_FAILED;
			return ONLP_STATUS_OK;
		}
	}

	if (tid == THERMAL_CPU_CORE) {
		return get_max_cpu_coretemp(&info->mcelsius);
	}

	return onlp_file_read_int(&info->mcelsius, devfiles__[tid]);
}
