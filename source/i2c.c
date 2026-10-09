

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <math.h>
#include <errno.h>

#include "i2c.h"
#include "utils.h"
#include "ezo.h"
#include "i2c_bus.h"

const char* i2c_get_driver_name(i2c_sensor_type_t type)
{
  switch(type){
    case I2C_SENSOR_PTE7300:
      return "PTE7300";
    break;
    case I2C_SENSOR_HSC:
      return "HSC/SSC";
    break;
    default:
      return "";
    break;
  }
}

// ─── Real implementation ──────────────────────────────────────────────────────
// All code in this block is compiled only when DUMMY_SENSORS is NOT defined.
// To build with fake sensors: make dummy
// To build normally:          make

#ifndef DUMMY_SENSORS

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

// Set to 0 if bring-up shows PTE7300 registers are actually big-endian on
// your unit — see the header comment on PTE7300 byte order. Quick check
// with i2c-tools from the shell (adjust bus/addr as needed):
//   i2cset -y 1 0x6c 0x22 0x93 0x8b i   # write START, LSB-first as this driver does
//   i2cget -y 1 0x6c 0x2e w             # i2c_smbus word read is LSB-first too
// If the pressure reading only makes sense with the two data bytes swapped,
// flip this to 0.
#define PTE7300_LITTLE_ENDIAN 1

int pte7300_validate_status(unsigned short raw_status);

// ─── Generic transport ────────────────────────────────────────────────────────
// Every transaction goes through i2c_open()/i2c_close() (i2c_bus.c): i2c_open()
// takes the per-address lock, opens `bus_path` and selects the slave address;
// i2c_close() releases the lock.  The address is validated first because
// i2c_open() treats a negative address as "no lock, no ioctl", which is meant for
// bus-level use only (scans, availability), never for talking to a device.
//
// The I2C_RDWR paths carry the address in each message, so the I2C_SLAVE that
// i2c_open() issues is redundant for them.  It is harmless unless a kernel driver
// has claimed the address (EBUSY), in which case the transfer is refused.

int i2c_bus_available(const char *bus_path)
{
  int fd = i2c_open(bus_path, -1);      // proves the bus device opens; no address, no lock
  if (fd < 0) return 0;
  i2c_close(fd);
  return 1;
}

int i2c_write_reg(const char *bus_path, int address, unsigned char reg,
                   const unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len < 0 || len > 32 || (len > 0 && data == NULL)) return I2C_ERROR;

  unsigned char buf[1 + 32];
  buf[0] = reg;
  if (len > 0) memcpy(&buf[1], data, len);

  int fd = i2c_open(bus_path, address);
  if (fd < 0) return I2C_ERROR;

  struct i2c_msg msg = { .addr = (unsigned short)address, .flags = 0,
                          .len = (unsigned short)(1 + len), .buf = buf };
  struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };

  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  i2c_close(fd);
  return (ret < 0) ? I2C_ERROR : I2C_SUCCESS;
}

int i2c_read_reg(const char *bus_path, int address, unsigned char reg,
                  unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len <= 0 || data == NULL) return I2C_ERROR;

  int fd = i2c_open(bus_path, address);
  if (fd < 0) return I2C_ERROR;

  struct i2c_msg msgs[2];
  msgs[0].addr  = (unsigned short)address;
  msgs[0].flags = 0;
  msgs[0].len   = 1;
  msgs[0].buf   = &reg;

  msgs[1].addr  = (unsigned short)address;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len   = (unsigned short)len;
  msgs[1].buf   = data;

  struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };

  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  i2c_close(fd);
  return (ret < 0) ? I2C_ERROR : I2C_SUCCESS;
}

int i2c_read_bytes(const char *bus_path, int address, unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len <= 0 || data == NULL) return I2C_ERROR;

  int fd = i2c_open(bus_path, address);   // also selects the slave address
  if (fd < 0) return I2C_ERROR;

  int ret = read(fd, data, len);
  i2c_close(fd);
  return (ret == len) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_write_bytes(const char *bus_path, int address, const unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  if (len <= 0 || data == NULL) return I2C_ERROR;

  int fd = i2c_open(bus_path, address);   // also selects the slave address
  if (fd < 0) return I2C_ERROR;

  int ret = write(fd, data, len);
  i2c_close(fd);
  return (ret == len) ? I2C_SUCCESS : I2C_ERROR;
}

// 1 if something ACKs at `address`, else 0.  An invalid address is "not there"
// (0), never an error code: I2C_ERROR is -1, which a caller would read as true.
int i2c_probe_address(const char *bus_path, int address)
{
  if (!i2c_addr_valid(address)) return 0;

  int fd = i2c_open(bus_path, address);
  if (fd < 0) return 0;

  unsigned char dummy;
  int ok = (read(fd, &dummy, 1) >= 0);
  i2c_close(fd);
  return ok;
}

// ─── Known-device table (extend this as you add more generic I2C sensors) ───
// Only sensors with a fixed/default factory address are worth listing here —
// HSC/SSC addresses are order-code specific, so they're deliberately absent.

typedef struct {
  int         addr;
  const char *name;
} i2c_addr_map_t;

static const i2c_addr_map_t i2c_known_devices[] = {
  { PTE7300_ADDR_DEFAULT, "PTE7300 pressure (non-CRC)" },
  { PTE7300_ADDR_CRC,     "PTE7300 pressure (CRC)" },
  { 0,                    NULL                          }
};

const char *i2c_name_from_addr(int addr)
{
  for (int i = 0; i2c_known_devices[i].name != NULL; i++)
    if (i2c_known_devices[i].addr == addr)
      return i2c_known_devices[i].name;

  // See if it's an ezo device
  return ezo_name_from_addr(addr);
}

// Active identification, used only by deepscan.  Unlike the scan itself (which
// sends no data), this WRITES to the device, so it must not be pointed at a
// chip in the wrong protocol:
//   - The PTE7300 answers at two addresses.  0x6C is the plain register protocol;
//     0x6D is the CRC-framed protocol.  The probe below is a plain register read,
//     which at 0x6D is a malformed frame, so 0x6D is never queried.
//   - The HSC/SSC probe is a bare multi-byte read.  It is never sent to 0x6C,
//     where a register-mapped PTE7300 lives.
static const char *i2c_query_device_type(int addr)
{
  if (addr == PTE7300_ADDR_CRC) return NULL;

  // 1. Try PTE7300 probe: read 2 bytes from the STATUS register (0x36)
  unsigned char pte_buf[2];
  if (i2c_read_reg(I2C_GENERIC_BUS, addr, PTE7300_REG_STATUS, pte_buf, 2) == I2C_SUCCESS) {
    unsigned short status_reg;
#if PTE7300_LITTLE_ENDIAN
    status_reg = (unsigned short)(pte_buf[0] | (pte_buf[1] << 8));
#else
    status_reg = (unsigned short)((pte_buf[0] << 8) | pte_buf[1]);
#endif
    if (pte7300_validate_status(status_reg) == I2C_SUCCESS) {
      return "PTE7300";
    }
  }

  // The PTE7300's own address ends here, whatever the status said.
  if (addr == PTE7300_ADDR_DEFAULT) return NULL;

  // 2. Try Honeywell HSC/SSC probe: registerless 4-byte read
  unsigned char hsc_buf[4];
  if (i2c_read_bytes(I2C_GENERIC_BUS, addr, hsc_buf, 4) == I2C_SUCCESS) {
    unsigned char status_bits = hsc_buf[0] & HSC_STATUS_MASK; // 0xC0 mask
    if (status_bits == HSC_STATUS_NORMAL || status_bits == HSC_STATUS_STALE) {
      return "HSC/SSC";
    }
  }

  return NULL;
}

/* Same probing rule as i2cdetect's default (auto) mode:
 *   SMBus Read Byte   for 0x30-0x37 and 0x50-0x5F  (EEPROM ranges, where a Quick
 *                      Write can corrupt data)
 *   SMBus Quick Write for everything else.
 * (The previous comment and ranges here were inverted.)
 * Returns 1 = ACK, 0 = no answer, -1 = address claimed by a kernel driver
 * (EBUSY; i2cdetect prints "UU").                                              */
static int i2c_probe_address_safe(int fd, int addr)
{
  if (ioctl(fd, I2C_SLAVE, addr) < 0)
    return (errno == EBUSY) ? -1 : 0;

  if ((addr >= 0x30 && addr <= 0x37) || (addr >= 0x50 && addr <= 0x5F)) {
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args = {
      .read_write = I2C_SMBUS_READ,
      .command = 0,
      .size = I2C_SMBUS_BYTE,
      .data = &data
    };
    return (ioctl(fd, I2C_SMBUS, &args) >= 0) ? 1 : 0;
  }

  struct i2c_smbus_ioctl_data args = {
    .read_write = I2C_SMBUS_WRITE,
    .command = 0,
    .size = I2C_SMBUS_QUICK,
    .data = NULL
  };
  return (ioctl(fd, I2C_SMBUS, &args) >= 0) ? 1 : 0;
}

/* Helper to check if address belongs to standard EZO default range */
static bool is_potential_ezo_address(int addr)
{
  /* Standard EZO default range: 0x61 (DO), 0x62 (ORP), 0x63 (pH), 
     0x64 (EC), 0x65 (Hum), 0x66 (RTD), 0x67 (Pump), 0x68 (PRS), etc. */
  return (addr >= 0x61 && addr <= 0x6B);
}

// What this puts on the wire, compared with `i2cdetect -y 1`:
//   scan only      : identical per address (see i2c_probe_address_safe).
//   deepscan adds  : real data transactions (i2c_query_device_type, and the EZO
//                    "i" command), which i2cdetect never sends.  That is the part
//                    that can upset a device; aquachemd warns about it.
// The per-address locks only exclude other THREADS of this process.  `aquachemd
// scan` run from a shell is a second process, so it can still interleave with a
// running daemon, exactly as i2cdetect can.
void i2c_detect(bool deepscan, bool usesyslog)
{
  // addr = -1: one fd walks every address, so it takes no lock and no I2C_SLAVE
  // of its own; each probe takes that address's lock itself.
  int fd = i2c_open(I2C_GENERIC_BUS, -1);
  if (fd < 0) { perror("open i2c"); return; }

  DIAG_LOG(usesyslog, "\nScanning I2C bus %s...\n\n", I2C_GENERIC_BUS);
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    char line[64];
    int len = snprintf(line, sizeof(line), "%02x: ", row * 16);

    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) {
        len += snprintf(line + len, sizeof(line) - len, "   ");
        continue;
      }

      if (i2c_lock_addr(addr) != 0) {          // busy in another thread: skip it
        len += snprintf(line + len, sizeof(line) - len, "   ");
        continue;
      }

      int found = i2c_probe_address_safe(fd, addr);
      i2c_unlock_addr(addr);

      if (found < 0) {
        len += snprintf(line + len, sizeof(line) - len, "UU ");
      } else if (found == 0) {
        len += snprintf(line + len, sizeof(line) - len, "-- ");
      } else {
        len += snprintf(line + len, sizeof(line) - len, "%02x ", addr);
        detected[count++] = addr;
      }
    }
    DIAG_LOG(usesyslog, "%s\n", line);
  }
  i2c_close(fd);

  if (count == 0) { DIAG_LOG(usesyslog, "\nNo devices found.\n"); return; }

  DIAG_LOG(usesyslog, "\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known   = i2c_name_from_addr(addr);
    const char *queried = NULL;

    /* SAFETY FIX: Only query active device details if address matches EZO ranges 
       to avoid corrupting third-party sensors/RTCs/expanders */
    if (deepscan) {
      if (is_potential_ezo_address(addr)) {
        usleep(10000); // 10ms bus recovery pause before active query
        queried = ezo_query_device_type(addr);
      } else {
        usleep(10000); // 10ms bus recovery pause
        queried = i2c_query_device_type(addr);
      }
    }

    if (queried) {
      DIAG_LOG(usesyslog, "  0x%02x  confirmed: %-6s  (default addr for: %s)\n", addr, queried, known ? known : "unknown");
    } else if (known) {
      DIAG_LOG(usesyslog, "  0x%02x  likely:    %-6s  (by default address, unconfirmed)\n", addr, known);
    } else {
      DIAG_LOG(usesyslog, "  0x%02x  unknown device\n", addr);
    }
  }
}

/*
void i2c_generic_detect(const char *bus_path)
{
  int fd = open(bus_path, O_RDWR);
  if (fd < 0) { perror("open i2c"); return; }

  printf("\nScanning I2C bus %s (generic sensors)...\n\n", bus_path);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    printf("%02x: ", row * 16);
    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) { printf("   "); continue; }
      if (ioctl(fd, I2C_SLAVE, addr) < 0) { printf("   "); continue; }

      unsigned char buf;
      if (read(fd, &buf, 1) < 0)
        printf("-- ");
      else
      {
        printf("%02x ", addr);
        detected[count++] = addr;
      }
    }
    printf("\n");
  }
  close(fd);

  if (count == 0) { printf("\nNo devices found.\n"); return; }

  printf("\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known = i2c_name_from_addr(addr);
    if (known)
      printf("  0x%02x  likely: %s (by default address, unconfirmed)\n", addr, known);
    else
      printf("  0x%02x  unknown device (could be HSC/SSC — those have no fixed default)\n", addr);
  }
  printf("\n");
}
*/
// ─── Shared scaling helper ─────────────────────────────────────────────────
// Linear map from raw counts to engineering units — used by every sensor
// family below (PTE7300's ±16000 span, HSC/SSC's transfer function span,
// and both sensors' temperature outputs).

static float i2c_scale(long raw, long min_counts, long max_counts, float min_value, float max_value)
{
  if (max_counts == min_counts) return NAN;   // misconfigured sensor (e.g. init never ran) — never silently return ±inf
  return min_value + (float)(raw - min_counts) * (max_value - min_value) / (float)(max_counts - min_counts);
}



// ─── PTE7300 pressure sensor ──────────────────────────────────────────────────


// ─── CRC4/CRC8 (ported from Sensata's official PTE7300_I2C Arduino library) ──
// Bit-serial CRC exactly as in their reference implementation — translated
// line-for-line from PTE7300_I2C.cpp's calc_crc4()/calc_crc8(), not
// reverse-engineered, so the framing matches what the ASIC's CRC-protected
// command channel actually expects.

static unsigned char pte7300_crc4(unsigned char polynom, unsigned char init,
                                   const unsigned char *data, unsigned int len)
{
  unsigned char shifter = init;
  for (unsigned int i = 0; i < len; i++) {
    for (int j = 7; j >= 0; j--) {
      if (i >= len - 1 && j < 4) break;
      if (((shifter >> 3) & 0x01) != ((data[i] >> j) & 0x01))
        shifter = (unsigned char)((shifter << 1) ^ polynom);
      else
        shifter = (unsigned char)(shifter << 1);
      shifter &= 0x0F;
    }
  }
  return shifter & 0x0F;
}

static unsigned char pte7300_crc8(unsigned char polynom, unsigned char init,
                                   const unsigned char *data, unsigned int len)
{
  unsigned char shifter = init;
  for (unsigned int i = 0; i < len; i++) {
    for (int j = 7; j >= 0; j--) {
      if (((shifter >> 7) & 0x01) != ((data[i] >> j) & 0x01))
        shifter = (unsigned char)((shifter << 1) ^ polynom);
      else
        shifter = (unsigned char)(shifter << 1);
    }
  }
  return shifter;
}

// CRC-protected command write — mirrors writeRegisterCRC() from Sensata's
// library for the single-word command case (number=1). Their library
// defaults CRC mode ON for everything; our working hypothesis is the ASIC
// requires it specifically for command-channel writes, even though it's
// been accepting plain non-CRC reads fine.
static int pte7300_write_cmd_crc(i2c_sensor_t *s, unsigned int cmd)
{
  unsigned char lo, hi;
#if PTE7300_LITTLE_ENDIAN
  lo = (unsigned char)(cmd & 0xFF);
  hi = (unsigned char)((cmd >> 8) & 0xFF);
#else
  hi = (unsigned char)(cmd & 0xFF);
  lo = (unsigned char)((cmd >> 8) & 0xFF);
#endif

  // node byte: 7-bit address shifted into an 8-bit form with read=0, crc=1 —
  // this is where the datasheet's "0xDA including CRC" comes from: for
  // s->address=0x6C, this evaluates to exactly 0xDA.
  unsigned char node = (unsigned char)(((s->address << 1) & 0xFC) | 0x02);

  unsigned char header[2] = { PTE7300_REG_CMD, 0x10 };   // 0x10 = ((1 word * 2)-1)<<4
  unsigned char crc4 = pte7300_crc4(0x03, 0x0F, header, 2);
  unsigned char len_crc4_byte = (unsigned char)(0x10 | (crc4 & 0x0F));

  unsigned char all[5] = { node, PTE7300_REG_CMD, len_crc4_byte, lo, hi };
  unsigned char crc8 = pte7300_crc8(0xD5, 0xFF, all, sizeof(all));

  // Wire frame after the CRC-mode slave address (0x6D): [reg][len/crc4][lo][hi][crc8]
  unsigned char payload[4] = { len_crc4_byte, lo, hi, crc8 };

  // One chip, two addresses (0x6C plain, 0x6D CRC) but the locks are per ADDRESS,
  // so also hold the plain address while a command goes out on the CRC one;
  // otherwise it could interleave with a register read of the same chip.
  // (Skipped if the sensor is configured directly at the CRC address, which
  // i2c_write_reg() is about to lock itself.)
  int hold_plain = (s->address != PTE7300_ADDR_CRC);
  if (hold_plain && i2c_lock_addr(s->address) != 0) return I2C_ERROR;
  int rc = i2c_write_reg(I2C_GENERIC_BUS, PTE7300_ADDR_CRC, PTE7300_REG_CMD, payload, sizeof(payload));
  if (hold_plain) i2c_unlock_addr(s->address);
  return rc;
}



#define PTE7300_USE_CRC_COMMANDS 1   // set to 0 to fall back to plain (non-CRC) command writes

static int pte7300_write_cmd(i2c_sensor_t *s, unsigned int cmd)
{
#if PTE7300_USE_CRC_COMMANDS
  return pte7300_write_cmd_crc(s, cmd);
#else
  unsigned char data[2];
#if PTE7300_LITTLE_ENDIAN
  data[0] = (unsigned char)(cmd & 0xFF);
  data[1] = (unsigned char)((cmd >> 8) & 0xFF);
#else
  data[0] = (unsigned char)((cmd >> 8) & 0xFF);
  data[1] = (unsigned char)(cmd & 0xFF);
#endif
  return i2c_write_reg(I2C_GENERIC_BUS, s->address, PTE7300_REG_CMD, data, 2);
#endif
}

static int pte7300_read_i16(i2c_sensor_t *s, unsigned char reg, short *out)
{
  unsigned char data[2];
  int status = i2c_read_reg(I2C_GENERIC_BUS, s->address, reg, data, 2);
  if (status != I2C_SUCCESS) return status;

#if PTE7300_LITTLE_ENDIAN
  *out = (short)(data[0] | (data[1] << 8));
#else
  *out = (short)((data[0] << 8) | data[1]);
#endif
  return I2C_SUCCESS;
}

static int pte7300_read_u16(i2c_sensor_t *s, unsigned char reg, unsigned short *out)
{
  unsigned char data[2];
  int status = i2c_read_reg(I2C_GENERIC_BUS, s->address, reg, data, 2);
  if (status != I2C_SUCCESS) return status;

#if PTE7300_LITTLE_ENDIAN
  *out = (unsigned short)(data[0] | (data[1] << 8));
#else
  *out = (unsigned short)((data[0] << 8) | data[1]);
#endif
  return I2C_SUCCESS;
}

int pte7300_validate_status(unsigned short raw_status)
{

  // Empirically observed on this unit alongside independently-verified
  // correct pressure/temperature readings. NOT a decode of documented bit
  // semantics -- Sensata's bit table for 0x36 remains unconfirmed, and even
  // EPFL's hardware-tested driver doesn't attempt to interpret this register.
  // Treat this as "seen good before," not "understood."
  if (raw_status == 0xC00E || raw_status == 0xC01E) {
    return I2C_SUCCESS;
  }

  /* This below is simply a guess, I can;t find any actual documentation on it......
  // Extract the system layer and the device state layer
  uint8_t i2c_status_flags = (raw_status >> 14) & 0x03; // Top 2 bits
  uint8_t internal_diagnostic = raw_status & 0xFF;       // Lower 8 bits

  if (i2c_status_flags == 0x03) {
    // 0x03 (11b) is expected when reading register space 0x36
    if (internal_diagnostic == 0x0E || internal_diagnostic == 0x1E) {
        // Sensor is perfectly healthy.
        // 0x1E simply means an internal measurement cycle was active during the read.
        return I2C_SUCCESS;
    } else {
        // Handle unexpected lower-byte fault flags here if they occur
        return I2C_ERROR;
    }
  }*/
  return I2C_ERROR;
}

// Logging-only — see NOTE ON STATUS BITS in i2c.h. Nothing gates on this.
static void pte7300_decode_status(unsigned short status)
{
  LOG(LOG_DEBUG, "--- PTE7300 STATUS DECODER (Raw: 0x%04X) ---", status);

  if (status == 0x0000 || status == 0x0001) {
    LOG(LOG_INFO, "  [OK] System Normal - No diagnostic faults latched");
    return;
  }

  // --- High Byte System & DSP Diagnostics (Bits 15..8) ---
  if (status & 0x8000) LOG(LOG_ERR,   "  [!] Bit 15 (0x8000): GLOBAL DIAGNOSTIC FAULT - ASIC in error state");
  if (status & 0x4000) LOG(LOG_ERR,   "  [!] Bit 14 (0x4000): COMMAND ERROR - Invalid command or bad sequence");
  if (status & 0x2000) LOG(LOG_NOTICE,   "  [*] Bit 13 (0x2000): DSP BUSY - Conversion actively in progress");
  if (status & 0x1000) LOG(LOG_ERR,   "  [!] Bit 12 (0x1000): EEPROM/RAM FAULT - Memory CRC parity error");
  if (status & 0x0800) LOG(LOG_ERR,   "  [!] Bit 11 (0x0800): EXECUTION REJECTED - Command ignored by state machine");
  if (status & 0x0400) LOG(LOG_ERR,   "  [!] Bit 10 (0x0400): SUPPLY VOLTAGE FAULT - VDD under-voltage drop");
  if (status & 0x0200) LOG(LOG_ERR,   "  [!] Bit  9 (0x0200): THERMAL WARNING - Silicon temp exceeds operating limit");
  if (status & 0x0100) LOG(LOG_ERR,   "  [!] Bit  8 (0x0100): MATH FAULT - DSP raw count overflow/underflow");

  // --- Low Byte Analog Front-End & Sensor Element (Bits 7..0) ---
  if (status & 0x0080) LOG(LOG_ERR,   "  [!] Bit  7 (0x0080): INTERNAL DIAGNOSTIC - Reserved ASIC flag");
  if (status & 0x0040) LOG(LOG_ERR,   "  [!] Bit  6 (0x0040): BRIDGE DRIVE FAULT - Strain-gauge supply failure");
  if (status & 0x0020) LOG(LOG_ERR,   "  [!] Bit  5 (0x0020): SENSOR DIE FAULT - Open/Short circuit on element");
  if (status & 0x0010) LOG(LOG_ERR,   "  [!] Bit  4 (0x0010): PRESSURE AFE FAULT - Pressure ADC signal out-of-bounds");
  if (status & 0x0008) LOG(LOG_ERR,   "  [!] Bit  3 (0x0008): TEMP AFE FAULT - Diode ADC signal out-of-bounds");
  if (status & 0x0004) LOG(LOG_ERR,   "  [!] Bit  2 (0x0004): CALIBRATION FAULT - Factory EEPROM trim unreadable");
  if (status & 0x0002) LOG(LOG_NOTICE,   "  [*] Bit  1 (0x0002): MODE - Continuous/Cyclic mode active");
  if (status & 0x0001) LOG(LOG_NOTICE,   "  [*] Bit  0 (0x0001): STATE - Measurement ready / Run state active");

  LOG(LOG_DEBUG, "---------------------------------------------");
}

int i2c_sensor_init_pte7300(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  s->type       = I2C_SENSOR_PTE7300;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = -16000;   // fixed by hardware, not part-dependent
  s->max_counts =  16000;

  // RESET's own write result isn't checked here — the ASIC power-cycles
  // itself immediately on receiving it and may NACK the transaction's STOP
  // phase as a side effect of that, which isn't a real failure. The
  // meaningful check is whether the sensor responds afterward.
  pte7300_write_cmd(s, PTE7300_CMD_RESET);
  usleep(50000);   // boot delay: give the ASIC time to reload EEPROM trim data

  unsigned short status_reg = 0;
  int ret = pte7300_read_u16(s, PTE7300_REG_STATUS, &status_reg);

  if (ret != I2C_SUCCESS) {
    LOG(LOG_ERR, "PTE7300 init failed: sensor at 0x%02X not responding post-reset", s->address);
    return I2C_ERROR;
  }

  if ( pte7300_validate_status(status_reg) != I2C_SUCCESS ) {
    LOG(LOG_ERR, "PTE7300 init failed: sensor at 0x%02X returned error status post-reset: 0x%04X", s->address, status_reg);
    pte7300_decode_status(status_reg);
    return I2C_ERROR;
  }
  
  LOG(LOG_INFO, "PTE7300 init success at 0x%02X (post-reset status: 0x%04X)", s->address, status_reg);
  return I2C_SUCCESS;
  
}



static i2c_reading_t pte7300_get_reading(i2c_sensor_t *s)
{
  i2c_reading_t result = { 0.0f, 0.0f, I2C_ERROR };

  short raw_pressure = 0, raw_temp = 0;
  unsigned short raw_status = 0;

  int status = pte7300_read_i16(s, PTE7300_REG_PRESSURE, &raw_pressure);
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  status = pte7300_read_i16(s, PTE7300_REG_TEMP, &raw_temp);
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  pte7300_read_u16(s, PTE7300_REG_STATUS, &raw_status);   // best-effort, non-fatal

  if ( pte7300_validate_status(raw_status) != I2C_SUCCESS ) {
    LOG(LOG_WARNING, "PTE7300 at 0x%02X: status 0x%04X not in the accepted set (raw pressure=%d, temp=%d); "
                     "returning value 0 with status SUCCESS", s->address, raw_status, raw_pressure, raw_temp);
    pte7300_decode_status(raw_status);
    result.status = I2C_SUCCESS;
    return result;
  }

  result.value  = i2c_scale(raw_pressure, s->min_counts, s->max_counts, s->min_value, s->max_value);
  result.temp_c = i2c_scale(raw_temp, -16000, 16000, -40.0f, 125.0f);

  if (!isfinite(result.value) || !isfinite(result.temp_c)) {
    LOG(LOG_ERR, "PTE7300 at 0x%02X: computed non-finite value (pressure=%f temp=%f) — "
                 "sensor likely misconfigured (min_counts=%d max_counts=%d); check init call site",
        s->address, result.value, result.temp_c, s->min_counts, s->max_counts);
    result.status = I2C_ERROR;
    return result;
  }

  result.status = I2C_SUCCESS;
  return result;
}

static int pte7300_sleep(i2c_sensor_t *s)  { return pte7300_write_cmd(s, PTE7300_CMD_SLEEP); }
static int pte7300_reset(i2c_sensor_t *s)  { return pte7300_write_cmd(s, PTE7300_CMD_RESET); }

int i2c_sensor_idle_pte7300(i2c_sensor_t *s)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;
  return pte7300_write_cmd(s, PTE7300_CMD_IDLE);
}

int i2c_sensor_get_serial_pte7300(i2c_sensor_t *s, unsigned int *serial)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;

  unsigned short lo, hi;
  int status = pte7300_read_u16(s, PTE7300_REG_SERIAL_LO, &lo);
  if (status != I2C_SUCCESS) return status;
  status = pte7300_read_u16(s, PTE7300_REG_SERIAL_HI, &hi);
  if (status != I2C_SUCCESS) return status;

  *serial = ((unsigned int)hi << 16) | lo;
  return I2C_SUCCESS;
}

// ─── Honeywell HSC/SSC pressure sensor ───────────────────────────────────────

void i2c_sensor_init_hsc(i2c_sensor_t *s, int address, float min_value, float max_value,
                          int min_counts, int max_counts)
{
  s->type       = I2C_SENSOR_HSC;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = min_counts;
  s->max_counts = max_counts;
}

void i2c_sensor_init_hsc_default(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  i2c_sensor_init_hsc(s, address, min_value, max_value, HSC_COUNTS_MIN_DEFAULT, HSC_COUNTS_MAX_DEFAULT);
}

static i2c_reading_t hsc_get_reading(i2c_sensor_t *s)
{
  i2c_reading_t result = { 0.0f, 0.0f, I2C_ERROR };

  // No command needed — the device free-runs; just read the last conversion.
  // 4 bytes gets both pressure and temperature in one transaction.
  unsigned char buf[4];
  int status = i2c_read_bytes(I2C_GENERIC_BUS, s->address, buf, sizeof(buf));
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  unsigned char status_bits = buf[0] & HSC_STATUS_MASK;
  if (status_bits == HSC_STATUS_DIAGNOSTIC)   { result.status = I2C_ERROR;   return result; }
  if (status_bits == HSC_STATUS_COMMAND_MODE) { result.status = I2C_ERROR;   return result; }
  if (status_bits == HSC_STATUS_STALE)        { result.status = I2C_PENDING; /* fall through, value still returned below */ }

  unsigned int raw_pressure = ((buf[0] & 0x3F) << 8) | buf[1];                       // 14-bit
  unsigned int raw_temp     = ((unsigned int)buf[2] << 3) | (buf[3] >> 5);           // 11-bit

  result.value  = i2c_scale(raw_pressure, s->min_counts, s->max_counts, s->min_value, s->max_value);
  result.temp_c = i2c_scale(raw_temp, 0, HSC_TEMP_COUNTS_MAX, HSC_TEMP_MIN_C, HSC_TEMP_MAX_C);

  if (status_bits == HSC_STATUS_NORMAL) result.status = I2C_SUCCESS;
  return result;
}

// ─── Generic dispatch ─────────────────────────────────────────────────────────

int i2c_sensor_init(i2c_sensor_t *s, i2c_sensor_type_t type, int address, float min_value, float max_value)
{
  switch (type)
  {
    case I2C_SENSOR_PTE7300:
      LOG(LOG_DEBUG, "i2c_sensor_init: PTE7300 at 0x%02x, %.2f-%.2f", address, min_value, max_value);
      return i2c_sensor_init_pte7300(s, address, min_value, max_value);

    case I2C_SENSOR_HSC:
      LOG(LOG_DEBUG, "i2c_sensor_init: HSC at 0x%02x, %.2f-%.2f", address, min_value, max_value);
      i2c_sensor_init_hsc_default(s, address, min_value, max_value);
      return I2C_SUCCESS;

    default:
      return I2C_ERROR;
  }
}

int i2c_sensor_is_connected(i2c_sensor_t *s)
{
  // Same underlying check regardless of family — both just need an ACK.
  return i2c_probe_address(I2C_GENERIC_BUS, s->address);
}

i2c_reading_t i2c_sensor_get_reading(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_get_reading(s);
    case I2C_SENSOR_HSC:     return hsc_get_reading(s);
    default:                 return (i2c_reading_t){ 0.0f, 0.0f, I2C_ERROR };
  }
}

int i2c_sensor_sleep(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_sleep(s);
    case I2C_SENSOR_HSC:     return I2C_UNSUPPORTED;   // free-running, no sleep command over I2C
    default:                 return I2C_ERROR;
  }
}

int i2c_sensor_reset(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_reset(s);
    case I2C_SENSOR_HSC:     return I2C_UNSUPPORTED;   // no reset command over I2C
    default:                 return I2C_ERROR;
  }
}

#endif // ifndef DUMMY_SENSORS

// ─── Dummy sensor implementation ──────────────────────────────────────────────
// Compiled only when DUMMY_SENSORS is defined: make dummy
// Returns plausible values with small random drift, no hardware required.

#ifdef DUMMY_SENSORS

int i2c_bus_available(const char *bus_path) { (void)bus_path; return 1; }

void i2c_detect(bool deepscan, bool usesyslog)
{
  (void)deepscan;
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  DIAG_LOG(usesyslog, "00:                         -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "60: -- -- 62 63 -- -- 66 67 -- -- -- -- 6c 6d -- --\n");
  DIAG_LOG(usesyslog, "70: -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "\n");
  DIAG_LOG(usesyslog, "Detected devices:\n");
  DIAG_LOG(usesyslog, "  0x62  likely:    ORP     (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x63  likely:    pH      (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x66  likely:    RTD     (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x67  likely:    PUMP    (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x6c  likely:    PTE7300 pressure (non-CRC)  (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x6d  likely:    PTE7300 pressure (CRC)  (by default address, unconfirmed)\n");
}

void i2c_generic_detect(const char *bus_path)
{
  (void)bus_path;
  printf("\n[DUMMY] Simulated generic I2C bus scan\n\n");
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  printf("60:                          6c                    \n\n");
  printf("Detected devices:\n");
  printf("  0x6c  likely: PTE7300 pressure (non-CRC) (by default address, unconfirmed)\n\n");
}

int i2c_write_reg(const char *bus_path, int address, unsigned char reg,
                   const unsigned char *data, int len)
{
  (void)bus_path; (void)reg; (void)data; (void)len;
  return i2c_addr_valid(address) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_read_reg(const char *bus_path, int address, unsigned char reg,
                  unsigned char *data, int len)
{
  (void)bus_path; (void)reg;
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  memset(data, 0, len);
  return I2C_SUCCESS;
}

int i2c_read_bytes(const char *bus_path, int address, unsigned char *data, int len)
{
  (void)bus_path;
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  memset(data, 0, len);
  return I2C_SUCCESS;
}

int i2c_write_bytes(const char *bus_path, int address, const unsigned char *data, int len)
{
  (void)bus_path; (void)data; (void)len;
  return i2c_addr_valid(address) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_probe_address(const char *bus_path, int address)
{
  (void)bus_path;
  return i2c_addr_valid(address) && (address == PTE7300_ADDR_DEFAULT);
}

// ─── Sensor instance setup (dummy) — same as real, no hardware touched ──────

int i2c_sensor_init_pte7300(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  s->type       = I2C_SENSOR_PTE7300;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = -16000;
  s->max_counts =  16000;
  return I2C_SUCCESS;
}

void i2c_sensor_init_hsc(i2c_sensor_t *s, int address, float min_value, float max_value,
                          int min_counts, int max_counts)
{
  s->type       = I2C_SENSOR_HSC;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = min_counts;
  s->max_counts = max_counts;
}

void i2c_sensor_init_hsc_default(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  i2c_sensor_init_hsc(s, address, min_value, max_value, HSC_COUNTS_MIN_DEFAULT, HSC_COUNTS_MAX_DEFAULT);
}

int i2c_sensor_init(i2c_sensor_t *s, i2c_sensor_type_t type, int address, float min_value, float max_value)
{
  switch (type)
  {
    case I2C_SENSOR_PTE7300: return i2c_sensor_init_pte7300(s, address, min_value, max_value);
    case I2C_SENSOR_HSC:     i2c_sensor_init_hsc_default(s, address, min_value, max_value); return I2C_SUCCESS;
    default:                 return I2C_ERROR;
  }
}

int i2c_sensor_is_connected(i2c_sensor_t *s) { (void)s; return 1; }

i2c_reading_t i2c_sensor_get_reading(i2c_sensor_t *s)
{
  // Plausible clean pool filter pressure, same idea as prs_get_reading() in
  // ezo.c's dummy block — centered around 15 psi (or the unit's midpoint if
  // your span isn't psi-ish) with a little drift, clamped to the configured span.
  //float mid   = (s->min_value + s->max_value) / 2.0f;
  //float value = (s->max_value - s->min_value > 5.0f) ? 15.0f + dummy_drift(1.0f) : mid + dummy_drift((s->max_value - s->min_value) * 0.02f);
  //if (value < s->min_value) value = s->min_value;
  //if (value > s->max_value) value = s->max_value;

  // Center at 15.0 PSI with ±13.0 PSI drift gives range [2.0, 28.0] PSI
  float target_psi = 15.0f + dummy_drift(13.0f);

  // Convert PSI back to bar for the primary value field
  float value_bar = target_psi / 14.5037738f;

  return (i2c_reading_t){ value_bar, 25.0f + dummy_drift(1.0f), I2C_SUCCESS };
}

int i2c_sensor_sleep(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_reset(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_idle_pte7300(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_get_serial_pte7300(i2c_sensor_t *s, unsigned int *serial)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;
  *serial = 0xDEADBEEF;
  return I2C_SUCCESS;
}

#endif // DUMMY_SENSORS



#ifdef __OLD__

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <math.h>

#include "i2c.h"
#include "utils.h"
#include "ezo.h"
#include "i2c_bus.h"

const char* i2c_get_driver_name(i2c_sensor_type_t type)
{
  switch(type){
    case I2C_SENSOR_PTE7300:
      return "PTE7300";
    break;
    case I2C_SENSOR_HSC:
      return "HSC/SSC";
    break;
    default:
      return "";
    break;
  }
}

// ─── Real implementation ──────────────────────────────────────────────────────
// All code in this block is compiled only when DUMMY_SENSORS is NOT defined.
// To build with fake sensors: make dummy
// To build normally:          make

#ifndef DUMMY_SENSORS

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

// Set to 0 if bring-up shows PTE7300 registers are actually big-endian on
// your unit — see the header comment on PTE7300 byte order. Quick check
// with i2c-tools from the shell (adjust bus/addr as needed):
//   i2cset -y 1 0x6c 0x22 0x93 0x8b i   # write START, LSB-first as this driver does
//   i2cget -y 1 0x6c 0x2e w             # i2c_smbus word read is LSB-first too
// If the pressure reading only makes sense with the two data bytes swapped,
// flip this to 0.
#define PTE7300_LITTLE_ENDIAN 1

int pte7300_validate_status(unsigned short raw_status);

// ─── Generic transport ────────────────────────────────────────────────────────

int i2c_bus_available(const char *bus_path)
{
  int fd = open(bus_path, O_RDWR);
  if (fd < 0) return 0;
  close(fd);
  return 1;
}

int i2c_write_reg(const char *bus_path, int address, unsigned char reg,
                   const unsigned char *data, int len)
{
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  int fd = i2c_open(I2C_BUS, address);
  if (fd < 0) return I2C_ERROR;

  unsigned char buf[1 + 32];
  if (len > 32) { i2c_close(fd); return I2C_ERROR; }
  buf[0] = reg;
  if (len > 0) memcpy(&buf[1], data, len);

  struct i2c_msg msg = { .addr = (unsigned short)address, .flags = 0,
                          .len = (unsigned short)(1 + len), .buf = buf };
  struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };

  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  i2c_close(fd);
  return (ret < 0) ? I2C_ERROR : I2C_SUCCESS;
}

int i2c_read_reg(const char *bus_path, int address, unsigned char reg,
                  unsigned char *data, int len)
{
  //int fd = open(bus_path, O_RDWR);
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  int fd = i2c_open(I2C_BUS, address);

  if (fd < 0) return I2C_ERROR;

  struct i2c_msg msgs[2];
  msgs[0].addr  = (unsigned short)address;
  msgs[0].flags = 0;
  msgs[0].len   = 1;
  msgs[0].buf   = &reg;

  msgs[1].addr  = (unsigned short)address;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len   = (unsigned short)len;
  msgs[1].buf   = data;

  struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };

  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  i2c_close(fd);
  return (ret < 0) ? I2C_ERROR : I2C_SUCCESS;
}

int i2c_read_bytes(const char *bus_path, int address, unsigned char *data, int len)
{
  //int fd = open(bus_path, O_RDWR);
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  int fd = i2c_open(I2C_BUS, address);

  if (fd < 0) return I2C_ERROR;
  if (ioctl(fd, I2C_SLAVE, address) < 0) { i2c_close(fd); return I2C_ERROR; }

  int ret = read(fd, data, len);
  i2c_close(fd);
  return (ret == len) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_write_bytes(const char *bus_path, int address, const unsigned char *data, int len)
{
  //int fd = open(bus_path, O_RDWR);
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  int fd = i2c_open(I2C_BUS, address);

  if (fd < 0) return I2C_ERROR;
  if (ioctl(fd, I2C_SLAVE, address) < 0) { i2c_close(fd); return I2C_ERROR; }

  int ret = write(fd, data, len);
  i2c_close(fd);
  return (ret == len) ? I2C_SUCCESS : I2C_ERROR;
}

int i2c_probe_address(const char *bus_path, int address)
{
  //int fd = open(bus_path, O_RDWR);
  if (!i2c_addr_valid(address)) return I2C_ERROR;
  int fd = i2c_open(I2C_BUS, address);

  if (fd < 0) return 0;
  if (ioctl(fd, I2C_SLAVE, address) < 0) { i2c_close(fd); return 0; }

  unsigned char dummy;
  int ok = (read(fd, &dummy, 1) >= 0);
  i2c_close(fd);
  return ok;
}

// ─── Known-device table (extend this as you add more generic I2C sensors) ───
// Only sensors with a fixed/default factory address are worth listing here —
// HSC/SSC addresses are order-code specific, so they're deliberately absent.

typedef struct {
  int         addr;
  const char *name;
} i2c_addr_map_t;

static const i2c_addr_map_t i2c_known_devices[] = {
  { PTE7300_ADDR_DEFAULT, "PTE7300 pressure (non-CRC)" },
  { PTE7300_ADDR_CRC,     "PTE7300 pressure (CRC)" },
  { 0,                    NULL                          }
};

const char *i2c_name_from_addr(int addr)
{
  for (int i = 0; i2c_known_devices[i].name != NULL; i++)
    if (i2c_known_devices[i].addr == addr)
      return i2c_known_devices[i].name;

  // See if it's an ezo device
  return ezo_name_from_addr(addr);
}

int pte7300_safe_read_reg(const char *bus_path, unsigned char addr, unsigned char reg, unsigned char *buf, size_t len)
{
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data packets;

    //int fd = open(bus_path, O_RDWR);
    if (!i2c_addr_valid(addr)) return I2C_ERROR;
    int fd = i2c_open(I2C_BUS, addr);

    if (fd < 0) return I2C_ERROR;

    // Msg 1: Write register pointer (no STOP condition after)
    msgs[0].addr  = addr;
    msgs[0].flags = 0; // Write flag
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    // Msg 2: Read payload immediately using Repeated START
    msgs[1].addr  = addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    packets.msgs  = msgs;
    packets.nmsgs = 2;

    if (ioctl(fd, I2C_RDWR, &packets) < 0) {
      i2c_close(fd);
      return -1; // Transaction failed
    }

    i2c_close(fd);
    return 0; // Success
}

const char *i2c_query_device_type(int addr)
{
  // 1. Try PTE7300 probe: Read 2 bytes from STATUS register (0x36)
  unsigned char pte_buf[2];
  if (pte7300_safe_read_reg(I2C_BUS, addr, PTE7300_REG_STATUS, pte_buf, 2) == I2C_SUCCESS) {
    unsigned short status_reg;
#if PTE7300_LITTLE_ENDIAN
    status_reg = (unsigned short)(pte_buf[0] | (pte_buf[1] << 8));
#else
    status_reg = (unsigned short)((pte_buf[0] << 8) | pte_buf[1]);
#endif
    if (pte7300_validate_status(status_reg) == I2C_SUCCESS) {
      return "PTE7300";
    }
  }

  // 2. Try Honeywell HSC/SSC probe: Registerless 4-byte read
  unsigned char hsc_buf[4];
  if (i2c_read_bytes(I2C_BUS, addr, hsc_buf, 4) == I2C_SUCCESS) {
    unsigned char status_bits = hsc_buf[0] & HSC_STATUS_MASK; // 0xC0 mask
    if (status_bits == HSC_STATUS_NORMAL || status_bits == HSC_STATUS_STALE) {
      return "HSC/SSC";
    }
  }

  return NULL;
}

/* Helper mimicking linux i2cdetect probing logic */
static bool i2c_probe_address_safe(int fd, int addr)
{
  if (ioctl(fd, I2C_SLAVE, addr) < 0) {
    return false;
  }

  union i2c_smbus_data data;

  /* Linux i2cdetect uses SMBus Quick for 0x30-0x37 & 0x50-0x77, 
     and SMBus Read Byte for 0x08-0x2F & 0x38-0x4F */
  if ((addr >= 0x30 && addr <= 0x37) || (addr >= 0x50 && addr <= 0x77)) {
    // SMBus Quick Write
    struct i2c_smbus_ioctl_data args = {
      .read_write = I2C_SMBUS_WRITE,
      .command = 0,
      .size = I2C_SMBUS_QUICK,
      .data = NULL
    };
    return (ioctl(fd, I2C_SMBUS, &args) >= 0);
  } else {
    // SMBus Read Byte
    struct i2c_smbus_ioctl_data args = {
      .read_write = I2C_SMBUS_READ,
      .command = 0,
      .size = I2C_SMBUS_BYTE,
      .data = &data
    };
    return (ioctl(fd, I2C_SMBUS, &args) >= 0);
  }
}

/* Helper to check if address belongs to standard EZO default range */
static bool is_potential_ezo_address(int addr)
{
  /* Standard EZO default range: 0x61 (DO), 0x62 (ORP), 0x63 (pH), 
     0x64 (EC), 0x65 (Hum), 0x66 (RTD), 0x67 (Pump), 0x68 (PRS), etc. */
  return (addr >= 0x61 && addr <= 0x6B);
}

void i2c_detect(bool deepscan, bool usesyslog)
{
  //int fd = open(I2C_BUS, O_RDWR);
  
  int fd = i2c_open(I2C_BUS, -1);
  if (fd < 0) { perror("open i2c"); return; }

  /*
  unsigned long funcs;
  if (ioctl(fd, I2C_FUNCS, &funcs) >= 0) {
    DIAG_LOG(usesyslog, "SMBus Quick supported: %s\n", (funcs & I2C_FUNC_SMBUS_QUICK) ? "yes" : "no");
  }*/

  //DIAG_LOG(usesyslog, "=============================================\n");
  DIAG_LOG(usesyslog, "\nScanning I2C bus %s...\n\n", I2C_BUS);
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    char line[64];
    int len = snprintf(line, sizeof(line), "%02x: ", row * 16);

    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) {
        len += snprintf(line + len, sizeof(line) - len, "   ");
        continue;
      }

      if (i2c_lock_addr(addr) != 0) {
        len += snprintf(line + len, sizeof(line) - len, "   ");
        continue;
      }

      if (!i2c_probe_address_safe(fd, addr)) {
        len += snprintf(line + len, sizeof(line) - len, "-- ");
      } else {
        len += snprintf(line + len, sizeof(line) - len, "%02x ", addr);
        detected[count++] = addr;
      }

      i2c_unlock_addr(addr);
    }
    DIAG_LOG(usesyslog, "%s\n", line);
  }
  i2c_close(fd);

  if (count == 0) { DIAG_LOG(usesyslog, "\nNo devices found.\n"); return; }

  DIAG_LOG(usesyslog, "\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known   = i2c_name_from_addr(addr);
    const char *queried = NULL;

    /* SAFETY FIX: Only query active device details if address matches EZO ranges 
       to avoid corrupting third-party sensors/RTCs/expanders */
    if (deepscan) {
      if (is_potential_ezo_address(addr)) {
        usleep(10000); // 10ms bus recovery pause before active query
        queried = ezo_query_device_type(addr);
      } else {
        usleep(10000); // 10ms bus recovery pause
        queried = i2c_query_device_type(addr);
      }
    }

    if (queried) {
      DIAG_LOG(usesyslog, "  0x%02x  confirmed: %-6s  (default addr for: %s)\n", addr, queried, known ? known : "unknown");
    } else if (known) {
      DIAG_LOG(usesyslog, "  0x%02x  likely:    %-6s  (by default address, unconfirmed)\n", addr, known);
    } else {
      DIAG_LOG(usesyslog, "  0x%02x  unknown device\n", addr);
    }

    /*
    if (queried) {
      DIAG_LOG(usesyslog, "  0x%02x  confirmed: %-6s  (default addr for: %s)\n", addr, queried, known ? known : "unknown");
    } else if (known) {
      DIAG_LOG(usesyslog, "  0x%02x  likely:    %-6s  (by default address, unconfirmed)\n", addr, known);
    } else {
      const char *known2 = i2c_name_from_addr(addr);
      if (known2)
        DIAG_LOG(usesyslog, "  0x%02x  likely:    %s (by default address, unconfirmed)\n", addr, known2);
      else
        DIAG_LOG(usesyslog, "  0x%02x  unknown device\n", addr);
    }*/
  }
  //DIAG_LOG(usesyslog, "---------------------------------------------\n");
}

/*
void i2c_generic_detect(const char *bus_path)
{
  int fd = open(bus_path, O_RDWR);
  if (fd < 0) { perror("open i2c"); return; }

  printf("\nScanning I2C bus %s (generic sensors)...\n\n", bus_path);
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  int detected[128] = {0};
  int count = 0;

  for (int row = 0; row < 8; row++)
  {
    printf("%02x: ", row * 16);
    for (int col = 0; col < 16; col++)
    {
      int addr = row * 16 + col;
      if (addr < 0x08 || addr > 0x77) { printf("   "); continue; }
      if (ioctl(fd, I2C_SLAVE, addr) < 0) { printf("   "); continue; }

      unsigned char buf;
      if (read(fd, &buf, 1) < 0)
        printf("-- ");
      else
      {
        printf("%02x ", addr);
        detected[count++] = addr;
      }
    }
    printf("\n");
  }
  close(fd);

  if (count == 0) { printf("\nNo devices found.\n"); return; }

  printf("\nDetected devices:\n");
  for (int i = 0; i < count; i++)
  {
    int addr = detected[i];
    const char *known = i2c_name_from_addr(addr);
    if (known)
      printf("  0x%02x  likely: %s (by default address, unconfirmed)\n", addr, known);
    else
      printf("  0x%02x  unknown device (could be HSC/SSC — those have no fixed default)\n", addr);
  }
  printf("\n");
}
*/
// ─── Shared scaling helper ─────────────────────────────────────────────────
// Linear map from raw counts to engineering units — used by every sensor
// family below (PTE7300's ±16000 span, HSC/SSC's transfer function span,
// and both sensors' temperature outputs).

static float i2c_scale(long raw, long min_counts, long max_counts, float min_value, float max_value)
{
  if (max_counts == min_counts) return NAN;   // misconfigured sensor (e.g. init never ran) — never silently return ±inf
  return min_value + (float)(raw - min_counts) * (max_value - min_value) / (float)(max_counts - min_counts);
}



// ─── PTE7300 pressure sensor ──────────────────────────────────────────────────


// ─── CRC4/CRC8 (ported from Sensata's official PTE7300_I2C Arduino library) ──
// Bit-serial CRC exactly as in their reference implementation — translated
// line-for-line from PTE7300_I2C.cpp's calc_crc4()/calc_crc8(), not
// reverse-engineered, so the framing matches what the ASIC's CRC-protected
// command channel actually expects.

static unsigned char pte7300_crc4(unsigned char polynom, unsigned char init,
                                   const unsigned char *data, unsigned int len)
{
  unsigned char shifter = init;
  for (unsigned int i = 0; i < len; i++) {
    for (int j = 7; j >= 0; j--) {
      if (i >= len - 1 && j < 4) break;
      if (((shifter >> 3) & 0x01) != ((data[i] >> j) & 0x01))
        shifter = (unsigned char)((shifter << 1) ^ polynom);
      else
        shifter = (unsigned char)(shifter << 1);
      shifter &= 0x0F;
    }
  }
  return shifter & 0x0F;
}

static unsigned char pte7300_crc8(unsigned char polynom, unsigned char init,
                                   const unsigned char *data, unsigned int len)
{
  unsigned char shifter = init;
  for (unsigned int i = 0; i < len; i++) {
    for (int j = 7; j >= 0; j--) {
      if (((shifter >> 7) & 0x01) != ((data[i] >> j) & 0x01))
        shifter = (unsigned char)((shifter << 1) ^ polynom);
      else
        shifter = (unsigned char)(shifter << 1);
    }
  }
  return shifter;
}

// CRC-protected command write — mirrors writeRegisterCRC() from Sensata's
// library for the single-word command case (number=1). Their library
// defaults CRC mode ON for everything; our working hypothesis is the ASIC
// requires it specifically for command-channel writes, even though it's
// been accepting plain non-CRC reads fine.
static int pte7300_write_cmd_crc(i2c_sensor_t *s, unsigned int cmd)
{
  unsigned char lo, hi;
#if PTE7300_LITTLE_ENDIAN
  lo = (unsigned char)(cmd & 0xFF);
  hi = (unsigned char)((cmd >> 8) & 0xFF);
#else
  hi = (unsigned char)(cmd & 0xFF);
  lo = (unsigned char)((cmd >> 8) & 0xFF);
#endif

  // node byte: 7-bit address shifted into an 8-bit form with read=0, crc=1 —
  // this is where the datasheet's "0xDA including CRC" comes from: for
  // s->address=0x6C, this evaluates to exactly 0xDA.
  unsigned char node = (unsigned char)(((s->address << 1) & 0xFC) | 0x02);

  unsigned char header[2] = { PTE7300_REG_CMD, 0x10 };   // 0x10 = ((1 word * 2)-1)<<4
  unsigned char crc4 = pte7300_crc4(0x03, 0x0F, header, 2);
  unsigned char len_crc4_byte = (unsigned char)(0x10 | (crc4 & 0x0F));

  unsigned char all[5] = { node, PTE7300_REG_CMD, len_crc4_byte, lo, hi };
  unsigned char crc8 = pte7300_crc8(0xD5, 0xFF, all, sizeof(all));

  // Wire frame after the CRC-mode slave address (0x6D): [reg][len/crc4][lo][hi][crc8]
  unsigned char payload[4] = { len_crc4_byte, lo, hi, crc8 };

  return i2c_write_reg(I2C_GENERIC_BUS, PTE7300_ADDR_CRC, PTE7300_REG_CMD, payload, sizeof(payload));
}



#define PTE7300_USE_CRC_COMMANDS 1   // set to 0 to fall back to plain (non-CRC) command writes

static int pte7300_write_cmd(i2c_sensor_t *s, unsigned int cmd)
{
#if PTE7300_USE_CRC_COMMANDS
  return pte7300_write_cmd_crc(s, cmd);
#else
  unsigned char data[2];
#if PTE7300_LITTLE_ENDIAN
  data[0] = (unsigned char)(cmd & 0xFF);
  data[1] = (unsigned char)((cmd >> 8) & 0xFF);
#else
  data[0] = (unsigned char)((cmd >> 8) & 0xFF);
  data[1] = (unsigned char)(cmd & 0xFF);
#endif
  return i2c_write_reg(I2C_GENERIC_BUS, s->address, PTE7300_REG_CMD, data, 2);
#endif
}static int pte7300_read_i16(i2c_sensor_t *s, unsigned char reg, short *out)
{
  unsigned char data[2];
  int status = i2c_read_reg(I2C_GENERIC_BUS, s->address, reg, data, 2);
  if (status != I2C_SUCCESS) return status;

#if PTE7300_LITTLE_ENDIAN
  *out = (short)(data[0] | (data[1] << 8));
#else
  *out = (short)((data[0] << 8) | data[1]);
#endif
  return I2C_SUCCESS;
}

static int pte7300_read_u16(i2c_sensor_t *s, unsigned char reg, unsigned short *out)
{
  unsigned char data[2];
  int status = i2c_read_reg(I2C_GENERIC_BUS, s->address, reg, data, 2);
  if (status != I2C_SUCCESS) return status;

#if PTE7300_LITTLE_ENDIAN
  *out = (unsigned short)(data[0] | (data[1] << 8));
#else
  *out = (unsigned short)((data[0] << 8) | data[1]);
#endif
  return I2C_SUCCESS;
}

int pte7300_validate_status(unsigned short raw_status)
{

  // Empirically observed on this unit alongside independently-verified
  // correct pressure/temperature readings. NOT a decode of documented bit
  // semantics -- Sensata's bit table for 0x36 remains unconfirmed, and even
  // EPFL's hardware-tested driver doesn't attempt to interpret this register.
  // Treat this as "seen good before," not "understood."
  if (raw_status == 0xC00E || raw_status == 0xC01E) {
    return I2C_SUCCESS;
  }

  /* This below is simply a guess, I can;t find any actual documentation on it......
  // Extract the system layer and the device state layer
  uint8_t i2c_status_flags = (raw_status >> 14) & 0x03; // Top 2 bits
  uint8_t internal_diagnostic = raw_status & 0xFF;       // Lower 8 bits

  if (i2c_status_flags == 0x03) {
    // 0x03 (11b) is expected when reading register space 0x36
    if (internal_diagnostic == 0x0E || internal_diagnostic == 0x1E) {
        // Sensor is perfectly healthy.
        // 0x1E simply means an internal measurement cycle was active during the read.
        return I2C_SUCCESS;
    } else {
        // Handle unexpected lower-byte fault flags here if they occur
        return I2C_ERROR;
    }
  }*/
  return I2C_ERROR;
}

// Logging-only — see NOTE ON STATUS BITS in i2c.h. Nothing gates on this.
static void pte7300_decode_status(unsigned short status)
{
  LOG(LOG_DEBUG, "--- PTE7300 STATUS DECODER (Raw: 0x%04X) ---", status);

  if (status == 0x0000 || status == 0x0001) {
    LOG(LOG_INFO, "  [OK] System Normal - No diagnostic faults latched");
    return;
  }

  // --- High Byte System & DSP Diagnostics (Bits 15..8) ---
  if (status & 0x8000) LOG(LOG_ERR,   "  [!] Bit 15 (0x8000): GLOBAL DIAGNOSTIC FAULT - ASIC in error state");
  if (status & 0x4000) LOG(LOG_ERR,   "  [!] Bit 14 (0x4000): COMMAND ERROR - Invalid command or bad sequence");
  if (status & 0x2000) LOG(LOG_NOTICE,   "  [*] Bit 13 (0x2000): DSP BUSY - Conversion actively in progress");
  if (status & 0x1000) LOG(LOG_ERR,   "  [!] Bit 12 (0x1000): EEPROM/RAM FAULT - Memory CRC parity error");
  if (status & 0x0800) LOG(LOG_ERR,   "  [!] Bit 11 (0x0800): EXECUTION REJECTED - Command ignored by state machine");
  if (status & 0x0400) LOG(LOG_ERR,   "  [!] Bit 10 (0x0400): SUPPLY VOLTAGE FAULT - VDD under-voltage drop");
  if (status & 0x0200) LOG(LOG_ERR,   "  [!] Bit  9 (0x0200): THERMAL WARNING - Silicon temp exceeds operating limit");
  if (status & 0x0100) LOG(LOG_ERR,   "  [!] Bit  8 (0x0100): MATH FAULT - DSP raw count overflow/underflow");

  // --- Low Byte Analog Front-End & Sensor Element (Bits 7..0) ---
  if (status & 0x0080) LOG(LOG_ERR,   "  [!] Bit  7 (0x0080): INTERNAL DIAGNOSTIC - Reserved ASIC flag");
  if (status & 0x0040) LOG(LOG_ERR,   "  [!] Bit  6 (0x0040): BRIDGE DRIVE FAULT - Strain-gauge supply failure");
  if (status & 0x0020) LOG(LOG_ERR,   "  [!] Bit  5 (0x0020): SENSOR DIE FAULT - Open/Short circuit on element");
  if (status & 0x0010) LOG(LOG_ERR,   "  [!] Bit  4 (0x0010): PRESSURE AFE FAULT - Pressure ADC signal out-of-bounds");
  if (status & 0x0008) LOG(LOG_ERR,   "  [!] Bit  3 (0x0008): TEMP AFE FAULT - Diode ADC signal out-of-bounds");
  if (status & 0x0004) LOG(LOG_ERR,   "  [!] Bit  2 (0x0004): CALIBRATION FAULT - Factory EEPROM trim unreadable");
  if (status & 0x0002) LOG(LOG_NOTICE,   "  [*] Bit  1 (0x0002): MODE - Continuous/Cyclic mode active");
  if (status & 0x0001) LOG(LOG_NOTICE,   "  [*] Bit  0 (0x0001): STATE - Measurement ready / Run state active");

  LOG(LOG_DEBUG, "---------------------------------------------");
}

int i2c_sensor_init_pte7300(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  s->type       = I2C_SENSOR_PTE7300;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = -16000;   // fixed by hardware, not part-dependent
  s->max_counts =  16000;

  // RESET's own write result isn't checked here — the ASIC power-cycles
  // itself immediately on receiving it and may NACK the transaction's STOP
  // phase as a side effect of that, which isn't a real failure. The
  // meaningful check is whether the sensor responds afterward.
  pte7300_write_cmd(s, PTE7300_CMD_RESET);
  usleep(50000);   // boot delay: give the ASIC time to reload EEPROM trim data

  unsigned short status_reg = 0;
  int ret = pte7300_read_u16(s, PTE7300_REG_STATUS, &status_reg);

  if (ret != I2C_SUCCESS) {
    LOG(LOG_ERR, "PTE7300 init failed: sensor at 0x%02X not responding post-reset", s->address);
    return I2C_ERROR;
  }

  if ( pte7300_validate_status(status_reg) != I2C_SUCCESS ) {
    LOG(LOG_ERR, "PTE7300 init failed: sensor at 0x%02X returned error status post-reset: 0x%04X", s->address, status_reg);
    pte7300_decode_status(status_reg);
    return I2C_ERROR;
  }
  
  LOG(LOG_INFO, "PTE7300 init success at 0x%02X (post-reset status: 0x%04X)", s->address, status_reg);
  return I2C_SUCCESS;
  
}



static i2c_reading_t pte7300_get_reading(i2c_sensor_t *s)
{
  i2c_reading_t result = { 0.0f, 0.0f, I2C_ERROR };

  short raw_pressure = 0, raw_temp = 0;
  unsigned short raw_status = 0;

  int status = pte7300_read_i16(s, PTE7300_REG_PRESSURE, &raw_pressure);
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  status = pte7300_read_i16(s, PTE7300_REG_TEMP, &raw_temp);
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  pte7300_read_u16(s, PTE7300_REG_STATUS, &raw_status);   // best-effort, non-fatal

  if ( pte7300_validate_status(raw_status) != I2C_SUCCESS ) {
    pte7300_decode_status(raw_status);
    result.status = I2C_SUCCESS;
    return result;
  }

  result.value  = i2c_scale(raw_pressure, s->min_counts, s->max_counts, s->min_value, s->max_value);
  result.temp_c = i2c_scale(raw_temp, -16000, 16000, -40.0f, 125.0f);

  if (!isfinite(result.value) || !isfinite(result.temp_c)) {
    LOG(LOG_ERR, "PTE7300 at 0x%02X: computed non-finite value (pressure=%f temp=%f) — "
                 "sensor likely misconfigured (min_counts=%d max_counts=%d); check init call site",
        s->address, result.value, result.temp_c, s->min_counts, s->max_counts);
    result.status = I2C_ERROR;
    return result;
  }

  result.status = I2C_SUCCESS;
  return result;
}

static int pte7300_sleep(i2c_sensor_t *s)  { return pte7300_write_cmd(s, PTE7300_CMD_SLEEP); }
static int pte7300_reset(i2c_sensor_t *s)  { return pte7300_write_cmd(s, PTE7300_CMD_RESET); }

int i2c_sensor_idle_pte7300(i2c_sensor_t *s)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;
  return pte7300_write_cmd(s, PTE7300_CMD_IDLE);
}

int i2c_sensor_get_serial_pte7300(i2c_sensor_t *s, unsigned int *serial)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;

  unsigned short lo, hi;
  int status = pte7300_read_u16(s, PTE7300_REG_SERIAL_LO, &lo);
  if (status != I2C_SUCCESS) return status;
  status = pte7300_read_u16(s, PTE7300_REG_SERIAL_HI, &hi);
  if (status != I2C_SUCCESS) return status;

  *serial = ((unsigned int)hi << 16) | lo;
  return I2C_SUCCESS;
}

// ─── Honeywell HSC/SSC pressure sensor ───────────────────────────────────────

void i2c_sensor_init_hsc(i2c_sensor_t *s, int address, float min_value, float max_value,
                          int min_counts, int max_counts)
{
  s->type       = I2C_SENSOR_HSC;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = min_counts;
  s->max_counts = max_counts;
}

void i2c_sensor_init_hsc_default(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  i2c_sensor_init_hsc(s, address, min_value, max_value, HSC_COUNTS_MIN_DEFAULT, HSC_COUNTS_MAX_DEFAULT);
}

static i2c_reading_t hsc_get_reading(i2c_sensor_t *s)
{
  i2c_reading_t result = { 0.0f, 0.0f, I2C_ERROR };

  // No command needed — the device free-runs; just read the last conversion.
  // 4 bytes gets both pressure and temperature in one transaction.
  unsigned char buf[4];
  int status = i2c_read_bytes(I2C_GENERIC_BUS, s->address, buf, sizeof(buf));
  if (status != I2C_SUCCESS) { result.status = status; return result; }

  unsigned char status_bits = buf[0] & HSC_STATUS_MASK;
  if (status_bits == HSC_STATUS_DIAGNOSTIC)   { result.status = I2C_ERROR;   return result; }
  if (status_bits == HSC_STATUS_COMMAND_MODE) { result.status = I2C_ERROR;   return result; }
  if (status_bits == HSC_STATUS_STALE)        { result.status = I2C_PENDING; /* fall through, value still returned below */ }

  unsigned int raw_pressure = ((buf[0] & 0x3F) << 8) | buf[1];                       // 14-bit
  unsigned int raw_temp     = ((unsigned int)buf[2] << 3) | (buf[3] >> 5);           // 11-bit

  result.value  = i2c_scale(raw_pressure, s->min_counts, s->max_counts, s->min_value, s->max_value);
  result.temp_c = i2c_scale(raw_temp, 0, HSC_TEMP_COUNTS_MAX, HSC_TEMP_MIN_C, HSC_TEMP_MAX_C);

  if (status_bits == HSC_STATUS_NORMAL) result.status = I2C_SUCCESS;
  return result;
}

// ─── Generic dispatch ─────────────────────────────────────────────────────────

int i2c_sensor_init(i2c_sensor_t *s, i2c_sensor_type_t type, int address, float min_value, float max_value)
{
  switch (type)
  {
    case I2C_SENSOR_PTE7300:
      LOG(LOG_DEBUG, "i2c_sensor_init: PTE7300 at 0x%02x, %.2f-%.2f", address, min_value, max_value);
      return i2c_sensor_init_pte7300(s, address, min_value, max_value);

    case I2C_SENSOR_HSC:
      LOG(LOG_DEBUG, "i2c_sensor_init: HSC at 0x%02x, %.2f-%.2f", address, min_value, max_value);
      i2c_sensor_init_hsc_default(s, address, min_value, max_value);
      return I2C_SUCCESS;

    default:
      return I2C_ERROR;
  }
}

int i2c_sensor_is_connected(i2c_sensor_t *s)
{
  // Same underlying check regardless of family — both just need an ACK.
  return i2c_probe_address(I2C_GENERIC_BUS, s->address);
}

i2c_reading_t i2c_sensor_get_reading(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_get_reading(s);
    case I2C_SENSOR_HSC:     return hsc_get_reading(s);
    default:                 return (i2c_reading_t){ 0.0f, 0.0f, I2C_ERROR };
  }
}

int i2c_sensor_sleep(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_sleep(s);
    case I2C_SENSOR_HSC:     return I2C_UNSUPPORTED;   // free-running, no sleep command over I2C
    default:                 return I2C_ERROR;
  }
}

int i2c_sensor_reset(i2c_sensor_t *s)
{
  switch (s->type)
  {
    case I2C_SENSOR_PTE7300: return pte7300_reset(s);
    case I2C_SENSOR_HSC:     return I2C_UNSUPPORTED;   // no reset command over I2C
    default:                 return I2C_ERROR;
  }
}

#endif // ifndef DUMMY_SENSORS

// ─── Dummy sensor implementation ──────────────────────────────────────────────
// Compiled only when DUMMY_SENSORS is defined: make dummy
// Returns plausible values with small random drift, no hardware required.

#ifdef DUMMY_SENSORS

int i2c_bus_available(const char *bus_path) { (void)bus_path; return 1; }

void i2c_detect(bool deepscan, bool usesyslog)
{
  DIAG_LOG(usesyslog, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  DIAG_LOG(usesyslog, "00:                         -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "60: -- -- 62 63 -- -- 66 67 -- -- -- -- 6c 6d -- --\n");
  DIAG_LOG(usesyslog, "70: -- -- -- -- -- -- -- --\n");
  DIAG_LOG(usesyslog, "\n");
  DIAG_LOG(usesyslog, "Detected devices:\n");
  DIAG_LOG(usesyslog, "  0x62  likely:    ORP     (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x63  likely:    pH      (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x66  likely:    RTD     (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x67  likely:    PUMP    (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x6c  likely:    PTE7300 pressure (non-CRC)  (by default address, unconfirmed)\n");
  DIAG_LOG(usesyslog, "  0x6d  likely:    PTE7300 pressure (CRC)  (by default address, unconfirmed)\n");
}

void i2c_generic_detect(const char *bus_path)
{
  (void)bus_path;
  printf("\n[DUMMY] Simulated generic I2C bus scan\n\n");
  printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  printf("60:                          6c                    \n\n");
  printf("Detected devices:\n");
  printf("  0x6c  likely: PTE7300 pressure (non-CRC) (by default address, unconfirmed)\n\n");
}

int i2c_write_reg(const char *bus_path, int address, unsigned char reg,
                   const unsigned char *data, int len)
{
  (void)bus_path; (void)address; (void)reg; (void)data; (void)len;
  return I2C_SUCCESS;
}

int i2c_read_reg(const char *bus_path, int address, unsigned char reg,
                  unsigned char *data, int len)
{
  (void)bus_path; (void)address; (void)reg;
  memset(data, 0, len);
  return I2C_SUCCESS;
}

int i2c_read_bytes(const char *bus_path, int address, unsigned char *data, int len)
{
  (void)bus_path; (void)address;
  memset(data, 0, len);
  return I2C_SUCCESS;
}

int i2c_write_bytes(const char *bus_path, int address, const unsigned char *data, int len)
{
  (void)bus_path; (void)address; (void)data; (void)len;
  return I2C_SUCCESS;
}

int i2c_probe_address(const char *bus_path, int address)
{
  (void)bus_path;
  return (address == PTE7300_ADDR_DEFAULT);
}

// ─── Sensor instance setup (dummy) — same as real, no hardware touched ──────

int i2c_sensor_init_pte7300(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  s->type       = I2C_SENSOR_PTE7300;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = -16000;
  s->max_counts =  16000;
  return I2C_SUCCESS;
}

void i2c_sensor_init_hsc(i2c_sensor_t *s, int address, float min_value, float max_value,
                          int min_counts, int max_counts)
{
  s->type       = I2C_SENSOR_HSC;
  s->address    = address;
  s->min_value  = min_value;
  s->max_value  = max_value;
  s->min_counts = min_counts;
  s->max_counts = max_counts;
}

void i2c_sensor_init_hsc_default(i2c_sensor_t *s, int address, float min_value, float max_value)
{
  i2c_sensor_init_hsc(s, address, min_value, max_value, HSC_COUNTS_MIN_DEFAULT, HSC_COUNTS_MAX_DEFAULT);
}

int i2c_sensor_init(i2c_sensor_t *s, i2c_sensor_type_t type, int address, float min_value, float max_value)
{
  switch (type)
  {
    case I2C_SENSOR_PTE7300: return i2c_sensor_init_pte7300(s, address, min_value, max_value);
    case I2C_SENSOR_HSC:     i2c_sensor_init_hsc_default(s, address, min_value, max_value); return I2C_SUCCESS;
    default:                 return I2C_ERROR;
  }
}

int i2c_sensor_is_connected(i2c_sensor_t *s) { (void)s; return 1; }

i2c_reading_t i2c_sensor_get_reading(i2c_sensor_t *s)
{
  // Plausible clean pool filter pressure, same idea as prs_get_reading() in
  // ezo.c's dummy block — centered around 15 psi (or the unit's midpoint if
  // your span isn't psi-ish) with a little drift, clamped to the configured span.
  //float mid   = (s->min_value + s->max_value) / 2.0f;
  //float value = (s->max_value - s->min_value > 5.0f) ? 15.0f + dummy_drift(1.0f) : mid + dummy_drift((s->max_value - s->min_value) * 0.02f);
  //if (value < s->min_value) value = s->min_value;
  //if (value > s->max_value) value = s->max_value;

  // Center at 15.0 PSI with ±13.0 PSI drift gives range [2.0, 28.0] PSI
  float target_psi = 15.0f + dummy_drift(13.0f);

  // Convert PSI back to bar for the primary value field
  float value_bar = target_psi / 14.5037738f;

  return (i2c_reading_t){ value_bar, 25.0f + dummy_drift(1.0f), I2C_SUCCESS };
}

int i2c_sensor_sleep(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_reset(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_idle_pte7300(i2c_sensor_t *s)
{
  return (s->type == I2C_SENSOR_PTE7300) ? I2C_SUCCESS : I2C_UNSUPPORTED;
}

int i2c_sensor_get_serial_pte7300(i2c_sensor_t *s, unsigned int *serial)
{
  if (s->type != I2C_SENSOR_PTE7300) return I2C_UNSUPPORTED;
  *serial = 0xDEADBEEF;
  return I2C_SUCCESS;
}

#endif // DUMMY_SENSORS


#endif //__OLD__