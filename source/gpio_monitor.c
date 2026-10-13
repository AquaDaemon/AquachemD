
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <gpiod.h>
#include <poll.h>
#include <sys/eventfd.h> // NEW: For zero-overhead thread signaling

#include "aquachemd.h"
#include "acd_types.h"
#include "gpio_monitor.h"
#include "utils.h"
#include "gpio.h"
#include "state_manager.h"


#define MAX_GPIO_FDS 20

// static pthread_t gpio_thread_id = 0;
// static volatile bool run_gpio_monitor = false;
static int shutdown_fd = -1; // NEW: The shutdown signal file descriptor

static acd_thread_t _gpiomrunstate = {
    .parent_id = 0,
    .id = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .state = ACD_STARTING
};

/*
To use _ll_mutex, define it globally in aquachem.h
// Define and initialize the mutex globally
//pthread_mutex_t _ll_mutex = PTHREAD_MUTEX_INITIALIZER;

// Then declare here as extern
//extern pthread_mutex_t _ll_mutex;
*/

void *gpio_monitor_worker(void *ptr);

void start_gpio_monitor(struct aquachemdata *acddata)
{
    // if (run_gpio_monitor) return;
    pthread_mutex_lock(&_gpiomrunstate.mutex);

    if (_gpiomrunstate.state == ACD_KEEPRUNNING)
    {
        pthread_mutex_unlock(&_gpiomrunstate.mutex);
        LOG(LOG_NOTICE, "GPIO Monitor services thread is already running\n");
        return;
    }

    // Capture the thread ID of the parent (the thread calling this function)
    _gpiomrunstate.parent_id = pthread_self();

    // Create the event file descriptor
    shutdown_fd = eventfd(0, EFD_NONBLOCK);
    if (shutdown_fd < 0)
    {
        _gpiomrunstate.state = ACD_FAILED;
        pthread_mutex_unlock(&_gpiomrunstate.mutex);
        LOG(LOG_ERR, "GPIO: Failed to create eventfd\n");
        return;
    }

    _gpiomrunstate.state = ACD_STARTING;
    pthread_mutex_unlock(&_gpiomrunstate.mutex);

    // run_gpio_monitor = true;
    if (pthread_create(&_gpiomrunstate.id, NULL, gpio_monitor_worker, (void *)acddata) != 0)
    {
        // run_gpio_monitor = false;
        pthread_mutex_lock(&_gpiomrunstate.mutex);
        _gpiomrunstate.state = ACD_FAILED;
        pthread_mutex_unlock(&_gpiomrunstate.mutex);
        LOG(LOG_ERR, "Could not create GPIO Monitor thread\n");
        close(shutdown_fd);
    }
}

void stop_gpio_monitor()
{
    // Trigger the state change
    pthread_mutex_lock(&_gpiomrunstate.mutex);
    if (_gpiomrunstate.state == ACD_KEEPRUNNING) {
        _gpiomrunstate.state = ACD_CLEANUP;
        
        // 2. Wake the loop using eventfd
        uint64_t msg = 1;
        write(shutdown_fd, &msg, sizeof(uint64_t));
    }
    pthread_mutex_unlock(&_gpiomrunstate.mutex);

    // Wait for the thread to actually finish
    // We only join if we have a valid ID (not 0)
    if (_gpiomrunstate.id != 0) {
        pthread_join(_gpiomrunstate.id, NULL);
        _gpiomrunstate.id = 0; // Reset ID after join
    }

    // Cleanup resources
    if (shutdown_fd >= 0) {
        close(shutdown_fd);
        shutdown_fd = -1;
    }
}

typedef enum {
MON_OUTPUT, 
MON_COND, 
MON_FLOW
} monitor_type_t;

// --- 1. Clean Context Structure ---
// Bundles the metadata so we aren't juggling parallel arrays
typedef struct
{
    void *parent_obj;
    gpio_handle_t *handle;
    //bool is_condition;
    monitor_type_t type;
} monitor_ctx_t;

/*
// --- 2. The Setup Helper ---
static void register_line(gpio_handle_t *gh, void *obj, bool is_cond,
                          struct pollfd *fds, monitor_ctx_t *ctx, int *count) {
    if (*count >= MAX_GPIO_FDS || !gh || !gh->request) return;

    int fd = gpiod_line_request_get_fd(gh->request);
    if (fd >= 0) {
        fds[*count].fd = fd;
        fds[*count].events = POLLIN; // Wait for data to read

        ctx[*count].parent_obj = obj;
        ctx[*count].handle = gh;
        ctx[*count].is_condition = is_cond;

        (*count)++;
    } else {
        LOG(LOG_ERR, "GPIO: Invalid FD for pin %d\n", gh->pin);
    }
}
*/

void *gpio_monitor_worker(void *ptr)
{
    struct aquachemdata *acddata = (struct aquachemdata *)ptr;
    struct gpiod_edge_event_buffer *event_buffer;
    struct pollfd poll_fds[MAX_GPIO_FDS + 1];
    monitor_ctx_t ctx_map[MAX_GPIO_FDS];
    int num_lines = 0;

    LOG(LOG_NOTICE, "GPIO: Starting hardware monitor thread\n");

    // pthread_mutex_lock(&_ll_mutex);
    pthread_mutex_lock(&_gpiomrunstate.mutex);
    _gpiomrunstate.state = ACD_KEEPRUNNING;
    pthread_mutex_unlock(&_gpiomrunstate.mutex);

    for (acd_key_t *curr = acddata->keys; curr; curr = curr->next)
    {
        // Register Keys (Outputs) - Monitor for ERRORS ONLY
        if ((curr->type == ACD_TYPE_GPIO_PMP || curr->type == ACD_TYPE_GPIO_OUTPUT) && curr->data.gpio.request)
        {
            int fd = gpiod_line_request_get_fd(curr->data.gpio.request);
            if (fd >= 0)
            {
                poll_fds[num_lines].fd = fd;
                poll_fds[num_lines].events = 0; // We don't want event triggers, just errors
                ctx_map[num_lines] = (monitor_ctx_t){curr, &curr->data.gpio, MON_OUTPUT};
                num_lines++;
            }
        }
        else if ( (curr->type == ACD_TYPE_GPIO_COND || curr->type == ACD_TYPE_GPIO_INPUT) && curr->data.gpio.request)
        {
            // Register Conditions (Inputs) - Monitor for EVENTS + ERRORS
            int fd = gpiod_line_request_get_fd(curr->data.gpio.request);
            if (fd >= 0)
            {
                poll_fds[num_lines].fd = fd;
                poll_fds[num_lines].events = POLLIN; // Wake up on edge changes
                ctx_map[num_lines] = (monitor_ctx_t){curr, &curr->data.gpio, MON_COND};
                num_lines++;
            }
        }
        else if ( (curr->type == ACD_TYPE_GPIO_FLOW) && curr->data.gpio.request)
        {
            // Register Conditions (Inputs) - Monitor for EVENTS + ERRORS
            int fd = gpiod_line_request_get_fd(curr->data.gpio.request);
            if (fd >= 0)
            {
                poll_fds[num_lines].fd = fd;
                poll_fds[num_lines].events = POLLIN; // Wake up on edge changes
                ctx_map[num_lines] = (monitor_ctx_t){curr, &curr->data.gpio, MON_FLOW};
                num_lines++;
            }
        }
    }

    // pthread_mutex_unlock(&_ll_mutex);

    // Add shutdown handle
    int shutdown_idx = num_lines;
    poll_fds[shutdown_idx] = (struct pollfd){shutdown_fd, POLLIN, 0};

    event_buffer = gpiod_edge_event_buffer_new(16);

    // while (run_gpio_monitor) {
    while (atomic_load_explicit(&_gpiomrunstate.state, memory_order_relaxed) == ACD_KEEPRUNNING)
    {
        int ret = poll(poll_fds, num_lines + 1, -1);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        if (poll_fds[shutdown_idx].revents & POLLIN)
            break;

        for (int i = 0; i < num_lines; i++)
        {
            // --- CATCH HARDWARE ERRORS ---
            if (poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                LOG(LOG_ERR, "GPIO CRITICAL: Hardware failure on Pin %d\n", ctx_map[i].handle->pin);
                continue;
            }

            // --- HANDLE INPUT EVENTS (Conditions/Sensors) ---
            //if (ctx_map[i].is_condition && (poll_fds[i].revents & POLLIN))
            if (ctx_map[i].type == MON_COND && (poll_fds[i].revents & POLLIN))
            {
                // Drain the libgpiod edge events so the interrupt clears
                if (gpiod_line_request_read_edge_events(ctx_map[i].handle->request, event_buffer, 16) > 0)
                {

                    // Use our new domain-specific function
                    // This handles ACTIVE_LOW and REQ_ON/OFF internally
                    int met = sensor_is_met(ctx_map[i].handle);

                    if (met != GPIO_ERROR)
                    {
                        acd_key_t *c = (acd_key_t *)ctx_map[i].parent_obj;
                        // Only act if the state actually changed
                        if (c->met != (bool)met)
                        {
                            c->met = (bool)met;

                            LOG(LOG_INFO, "GPIO Condition Change: %s is now %s\n",
                                c->label,
                                c->met ? "SATISFIED" : "NOT MET");

                            SET_DIRTY(acddata->is_dirty);
                            set_key_state(acddata, c, (c->met ? ACD_LED_ON : ACD_LED_OFF));
                        }
                    }
                }
            }
            else if (ctx_map[i].type == MON_FLOW && (poll_fds[i].revents & POLLIN))
            {
               int n = gpiod_line_request_read_edge_events(ctx_map[i].handle->request, event_buffer, 16);
               if (n > 0)
                 __atomic_fetch_add(&((acd_key_t *)ctx_map[i].parent_obj)->data.flow.pulses, (uint64_t)n, __ATOMIC_RELAXED);
            }
        }
    }

    LOG(LOG_INFO, "Stopping GPIO Monitor services thread\n");
    gpiod_edge_event_buffer_free(event_buffer);

    pthread_mutex_lock(&_gpiomrunstate.mutex);
    _gpiomrunstate.state = ACD_FINISHED;
    pthread_cond_signal(&_gpiomrunstate.cond); // Wake up anyone waiting for exit
    pthread_mutex_unlock(&_gpiomrunstate.mutex);

    pthread_exit(0);

    return NULL;
}
