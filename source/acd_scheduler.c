
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <regex.h>


//#include "mongoose.h"
#include "aquachemd.h"
#include "acd_scheduler.h"
#include "config.h"
//#include "aq_systemutils.h"
#include "net_interface.h"
#include "utils.h"
#include "cJSON.h"



/*
Need to change to add a header

0 6 * * * root curl -s -S --show-error -o /dev/null \
  -H 'X-AquachemD-Source: cron' \
  -X PUT \
  http://localhost:80/api/Filter_Pump/set -d value=1

  // Look in net_services.c -> action_web_request() for the other side

*/


// Helper to safely extract strings from cJSON objects into our struct
void get_json_string(cJSON *obj, const char *key, char *dest, size_t dest_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(dest, item->valuestring, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

int save_schedules_js(const char* inBuf, int inSize, char* outBuf, int outSize) {
    FILE *fp;
    bool fileexists = false;
    const net_iface *iface = get_first_valid_interface();

    // Parse the incoming JSON buffer
    cJSON *root = cJSON_ParseWithLength(inBuf, inSize);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        LOG(LOG_ERR, "JSON Parse Error before: %s\n", error_ptr ? error_ptr : "unknown");
        return snprintf(outBuf, outSize, "{\"message\":\"Invalid JSON data\"}");
    }

    // Get the "schedules" array
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(devices)) {
        cJSON_Delete(root);
        return snprintf(outBuf, outSize, "{\"message\":\"Missing device array\"}");
    }

    // Open Cron File
    if (access(CRON_FILE, F_OK) == 0) {fileexists = true;}

    fp = fopen(CRON_FILE, "w");
    if (fp == NULL) {
        LOG(LOG_ERR, "Open file failed '%s'\n", CRON_FILE);
        cJSON_Delete(root);
        return snprintf(outBuf, outSize, "{\"message\":\"Error Saving Schedules\"}");
    }

    // Write Header
    fprintf(fp, "#***** AUTO GENERATED DO NOT EDIT *****\n");
    fprintf(fp, "PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin\n");

    // Iterate through the JSON array
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        aqs_cron cline = {0}; // Clean struct for each line

        // Extract values
        cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        // Handle both boolean true/false or string "1"/"0" for robustness
        cline.enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item) : 
                        (cJSON_IsString(enabled_item) && enabled_item->valuestring[0] == '1');

        get_json_string(item, "min",   cline.minute, sizeof(cline.minute));
        get_json_string(item, "hour",  cline.hour,   sizeof(cline.hour));
        get_json_string(item, "daym",  cline.daym,   sizeof(cline.daym));
        get_json_string(item, "month", cline.month,  sizeof(cline.month));
        get_json_string(item, "dayw",  cline.dayw,   sizeof(cline.dayw));
        get_json_string(item, "url",   cline.url,    sizeof(cline.url));
        get_json_string(item, "value", cline.value,  sizeof(cline.value));

        // Write to File
        LOG(LOG_INFO, "Writing Cron: %s %s %s...\n", cline.minute, cline.hour, cline.url);
        
        fprintf(fp, "%s%s %s %s %s %s root curl -s -S --show-error %s -o /dev/null %s%s -d value=%s -X PUT\n",
                (cline.enabled ? "" : "#"),
                cline.minute, cline.hour, cline.daym, cline.month, cline.dayw,
                (iface->isLocalurlTLS ? "--insecure" : ""),
                (iface->localurl[0] == '\0' ? _acdconfig_.listen_address : iface->localurl),
                cline.url, cline.value);
    }

    fprintf(fp, "#***** AUTO GENERATED DO NOT EDIT *****\n");

    // Cleanup
    if (!fileexists) {
      chmod(CRON_FILE, S_IRUSR | S_IWUSR);
    }
    
    cJSON_Delete(root);
    fclose(fp);

    return snprintf(outBuf, outSize, "{\"message\":\"Saved Schedules\"}");
}


int build_schedules_js(char* buffer, int size) {
    memset(buffer, 0, size);
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t read_size;
    regex_t regexCompiled;
    const char *regexString = "(#{0,1})([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s.*(\\/api\\/.*\\/set).* value=([0-9]+).*";
    
    // Pickup the letter after number (d,w,h) for day, week, hour.
    // NOTE the below regex expects the '-' for '-PUT' for the end limit
    //const char *regexString = "(#{0,1})([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s([^\\s]+)\\s.*(\\/api\\/.*\\/set).* value=([0-9]+[wdh]?) -.*";
    
    size_t maxGroups = 15;
    regmatch_t groupArray[maxGroups];

    // 1. Initialize the root JSON object and array
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "schedules");
    cJSON *schedules_array = cJSON_AddArrayToObject(root, "schedules");

    if (regcomp(&regexCompiled, regexString, REG_EXTENDED) != 0) {
        cJSON_AddStringToObject(root, "message", "Error compiling regex");
        goto finalize_json; 
    }

    fp = fopen(CRON_FILE, "r");
    if (fp == NULL) {
        cJSON_AddStringToObject(root, "message", "Error reading schedules file");
        regfree(&regexCompiled);
        goto finalize_json;
    }

    while ((read_size = getline(&line, &len, fp)) != -1) {
        if (regexec(&regexCompiled, line, maxGroups, groupArray, 0) == 0) {
            if (groupArray[8].rm_so != (size_t)-1) {
                // Create a schedule item object
                cJSON *item = cJSON_CreateObject();

                // Group 1: check if enabled (starts with # means disabled)
                bool is_enabled = (line[groupArray[1].rm_so] == '#') ? false : true;
                cJSON_AddBoolToObject(item, "enabled", is_enabled);

                // Helper macro-like logic to extract strings directly into cJSON
                #define ADD_GROUP_STRING(key, grp) do { \
                    char tmp[64]; \
                    int l = groupArray[grp].rm_eo - groupArray[grp].rm_so; \
                    snprintf(tmp, sizeof(tmp), "%.*s", l, line + groupArray[grp].rm_so); \
                    cJSON_AddStringToObject(item, key, tmp); \
                } while(0)

                ADD_GROUP_STRING("min", 2);
                ADD_GROUP_STRING("hour", 3);
                ADD_GROUP_STRING("daym", 4);
                ADD_GROUP_STRING("month", 5);
                ADD_GROUP_STRING("dayw", 6);
                ADD_GROUP_STRING("url", 9);
                ADD_GROUP_STRING("value", 10);

                cJSON_AddItemToArray(schedules_array, item);
            }
        }
    }

    fclose(fp);
    regfree(&regexCompiled);
    if (line) free(line);

finalize_json:;
    // 2. Render the JSON to the buffer
    char *rendered = cJSON_PrintUnformatted(root);
    int final_len = 0;
    if (rendered) {
        final_len = strlen(rendered);
        if (final_len < size) {
            strncpy(buffer, rendered, size);
        } else {
            LOG(LOG_ERR, "Buffer too small for JSON output\n");
        }
        free(rendered);
    }

    // 3. Clean up memory
    cJSON_Delete(root);
    return final_len;
}

