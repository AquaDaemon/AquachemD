
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/i2c-dev.h>

#include "i2c_bus.h"
#include "utils.h"          // LOG()

#define USE_I2C_MUTEX_LOCK  // master kill switch, comment out to disable all locking

#define I2C_MAX_ADDR  128
#define I2C_MAX_FD    1024

static pthread_mutex_t _addr_mutex[I2C_MAX_ADDR];
static short           _fd_addr[I2C_MAX_FD];   // fd -> locked addr, -1 = none
static pthread_once_t  _once = PTHREAD_ONCE_INIT;

static void _init(void)
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  for (int i = 0; i < I2C_MAX_ADDR; i++) pthread_mutex_init(&_addr_mutex[i], &attr);
  for (int i = 0; i < I2C_MAX_FD; i++)   _fd_addr[i] = -1;
  pthread_mutexattr_destroy(&attr);
}

// Optional policy. Default: lock everything. Example to lock only the pump:
//   #define I2C_NEEDS_LOCK(a)  ((a) == EZO_PMP_ADDR)
/*
#ifndef I2C_NEEDS_LOCK
#define I2C_NEEDS_LOCK(a) (1)
#endif
*/

int i2c_lock_addr(int addr)
{
#ifdef USE_I2C_MUTEX_LOCK
  pthread_once(&_once, _init);
  if (!i2c_addr_valid(addr)) return -1;
  //if (!I2C_NEEDS_LOCK(addr)) return 0;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  += I2C_LOCK_TIMEOUT_MS / 1000;
  ts.tv_nsec += (long)(I2C_LOCK_TIMEOUT_MS % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

  int rc = pthread_mutex_timedlock(&_addr_mutex[addr], &ts);
  if (rc != 0) {
    LOG(LOG_ERR, "I2C lock 0x%02X failed: %s\n", addr,
        rc == ETIMEDOUT ? "timeout" : rc == EDEADLK ? "relock by same thread" : "error");
    return -1;
  }
#endif
  return 0;
}

int i2c_unlock_addr(int addr)
{
#ifdef USE_I2C_MUTEX_LOCK
  //if (addr < 0 || addr >= I2C_MAX_ADDR || !I2C_NEEDS_LOCK(addr)) return 0;
  if (!i2c_addr_valid(addr)) return 0;
  return pthread_mutex_unlock(&_addr_mutex[addr]);
#else
  return 0;
#endif
}

int i2c_open(const char *bus, int addr)
{
  if (addr >= 0 && !i2c_addr_valid(addr)) return -1;       // 0 and > 127 never address a device
  if (addr >= 0 && i2c_lock_addr(addr) != 0) return -1;

  int fd = open(bus, O_RDWR);
  if (fd < 0) goto fail;

  if (addr >= 0 && ioctl(fd, I2C_SLAVE, addr) < 0) { close(fd); goto fail; }

  if (fd < I2C_MAX_FD) _fd_addr[fd] = (short)addr;   // owner-only write, no lock needed
  return fd;

fail:
  if (addr >= 0) i2c_unlock_addr(addr);
  return -1;
}

int i2c_close(int fd)
{
  int addr = -1;
  if (fd >= 0 && fd < I2C_MAX_FD) { addr = _fd_addr[fd]; _fd_addr[fd] = -1; }

  int rc = close(fd);              // close first, then release the lock
  if (addr >= 0) i2c_unlock_addr(addr);
  return rc;
}

