
#ifndef AQD_MG_COMPAT_H_
#define AQD_MG_COMPAT_H_

/*
 * Mongoose Compatibility & Project Flags Bridge
 * -------------------------------------------
 * Provides abstraction for Mongoose 7.x API changes (label vs fn_data)
 * and defines custom bitmask flags for AquaDaemon's, AqualinkD & AquachemD connection states.
 *
 * REQUIRED: Must be force-included via Makefile:
 * This is so mongoose.c/.h include it without modifying the source
 * CFLAGS += -include deps/mongoose/aqd_mg_compat.h
 */

 /*
 * Make sure to clean the flags on every new connection since mongoose reuses memory
 
 case MG_EV_ACCEPT: 
    // 1. Scrub reused memory block to clear ghost flags
    nc->fn_data = NULL;

 */

// Mongose auto arch configure seems to silently fail without this 
#define _GNU_SOURCE


#include <stdint.h>
#include "mongoose.h"

// Define your flags
#define AQD_MG_CON_MQTT            (1 << 0)
#define AQD_MG_CON_WS_SIM          (1 << 1)
#define AQD_MG_CON_WS_AQM          (1 << 2)
#define AQD_MG_CON_MQTT_CONNECTING (1 << 3)


// Helper Macros to handle the pointer casting
/*
// Mongoose 7.19+ Pointer Mapping. We are using fn_data
#define GET_AQD_FLAGS(nc)    ((uintptr_t)(nc)->fn_data)
#define SET_AQD_FLAGS(nc, f) ((nc)->fn_data = (void *)(uintptr_t)(f))

#define IS_MQTT_CONNECTING(nc) (GET_AQD_FLAGS(nc) & AQD_MG_CON_MQTT_CONNECTING)
#define IS_WEBSOCKET(nc)       ((nc)->is_websocket)
*/

// Mongoose 7.23+ Pointer Mapping. We are using data

#include <string.h>

static inline uintptr_t aqd_get_flags(const struct mg_connection *nc) {
  uintptr_t f;
  memcpy(&f, nc->data, sizeof(f));   // avoids alignment/aliasing problems
  return f;
}
static inline void aqd_set_flags(struct mg_connection *nc, uintptr_t f) {
  memcpy(nc->data, &f, sizeof(f));
}

#define GET_AQD_FLAGS(nc)      aqd_get_flags(nc)
#define SET_AQD_FLAGS(nc, f)   aqd_set_flags((nc), (uintptr_t)(f))
#define IS_MQTT_CONNECTING(nc) (GET_AQD_FLAGS(nc) & AQD_MG_CON_MQTT_CONNECTING)
#define IS_WEBSOCKET(nc)       ((nc)->is_websocket)


#endif //AQD_MG_COMPAT_H_