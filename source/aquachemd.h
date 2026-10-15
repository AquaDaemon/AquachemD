#ifndef AQUACHEMD_H_
#define AQUACHEMD_H_


#include <stdbool.h>
//#include "ezo.h"
//#include "1wire.h"
//#include "gpio.h"
#include "config.h"
#include "acd_types.h"

#define SET_DIRTY(flag)    ((flag) = true)
#define CLEAR_DIRTY(flag)  ((flag) = false)

#define SENSOR_FAULT_THRESHOLD 4

//void setKeyLed(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state);
//bool stateChangeRequest(struct aquachemdata *acdata, acd_key_t *key, acd_state_t state);

#define SIGRESTART     SIGUSR1
#define SIGRUPGRADE    SIGUSR2
//#define SIGCLEANUPEXIT SIGUSR3

void intHandler(int sig_num);
void set_upgrade_version(const char *version);
void aquachemd_request_reload(void);
void aquachemd_force_sensor_poll(void);
void scan_sensors(bool activeSystem, bool deepscan);

/**
 * SET_IF_CHANGED: Updates a variable and sets a flag if the value has changed.
 *
 * @src: The variable to be updated (can be a struct member).
 * @val: The new value.
 * @flag: A boolean flag to set to true if a change occurs.
 *
 * This macro uses GCC extensions for type safety and to prevent
 * double-evaluation of the `val` argument.
 */
#include <stdbool.h>
#include <string.h>



#define ASSIGN_IF_CHANGED(src, val, global_dirty, local_dirty) \
    ({                                                         \
        __typeof__(src) __new_val = (val);                     \
        bool __changed = false;                                \
        if ((src) != __new_val) {                              \
            (src) = __new_val;                                 \
            (global_dirty) = true;                             \
            (local_dirty)  = true;                             \
            __changed = true;                                  \
        }                                                      \
        __changed;                                             \
    })

#define DISPLAY_MSG_SIZE 64

struct aquachemdata
{
  char self[64];  // Filename being executed
  volatile bool is_dirty;

  int open_websockets;
  bool acdManagerActive;
  bool haveConditions;

  acd_key_t *keys; // Linked list of all keys (sensors, pumps, GPIOs, etc.) for easy access and management

  char display_message[DISPLAY_MSG_SIZE];
};

static inline acd_key_t *get_master(struct aquachemdata *acdata) { return acdata->keys; }

#define UNKNOWN -9999 


#endif // AQUACHEMD_H_

