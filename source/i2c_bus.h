#ifndef I2C_BUS_H_
#define I2C_BUS_H_

#define I2C_LOCK_TIMEOUT_MS  5000

// 1 if addr can address a device on the bus: 7-bit, and 0 is the general-call
// address, never a device.  Generic: used by every sensor type, not just EZO.
static inline int i2c_addr_valid(int addr) { return addr >= 1 && addr <= 127; }

// Lock addr, open bus, set slave address. Returns fd, or -1 on failure
// (including lock timeout / same-thread relock / invalid address).
// addr < 0 = no lock, no ioctl (bus-level use only: scans, availability checks).
int i2c_open(const char *bus, int addr);

// Close fd and release the lock taken by i2c_open().
int i2c_close(int fd);

// For code that moves one fd across many addresses (bus scans).
int i2c_lock_addr(int addr);
int i2c_unlock_addr(int addr);

#endif