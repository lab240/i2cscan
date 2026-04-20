#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#define MAX_BUSES 64
#define SCAN_TIMEOUT_SEC 2

static void print_help(const char *prog) {
    printf("Usage:\n");
    printf("  %s --bus <n>\n", prog);
    printf("  %s --bus-all\n", prog);
    printf("  %s --help\n", prog);
    printf("\n");
}

static int list_i2c_buses(int *buses) {
    DIR *dir;
    struct dirent *ent;
    int count = 0;

    dir = opendir("/dev");
    if (!dir)
        return -1;

    while ((ent = readdir(dir)) != NULL) {
        int bus;
        if (sscanf(ent->d_name, "i2c-%d", &bus) == 1) {
            if (count < MAX_BUSES)
                buses[count++] = bus;
        }
    }

    closedir(dir);

    /* sort ascending */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (buses[i] > buses[j]) {
                int tmp = buses[i];
                buses[i] = buses[j];
                buses[j] = tmp;
            }

    return count;
}

/*
 * Probe whether a device is present at addr.
 *
 * Uses the same strategy as i2cdetect:
 *   - 0x30..0x37, 0x50..0x5F: SMBus quick write (safe for EEPROMs)
 *   - everything else: SMBus read byte
 */
static int probe_addr(int fd, int addr) {
    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;

    /* Try I2C_SLAVE first; if the address is already claimed by a
     * kernel driver (shows as UU in i2cdetect), fall back to
     * I2C_SLAVE_FORCE so we can still probe it. */
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        if (errno == EBUSY) {
            if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0)
                return -1;
        } else {
            return -1;
        }
    }

    memset(&data, 0, sizeof(data));

    if ((addr >= 0x30 && addr <= 0x37) ||
        (addr >= 0x50 && addr <= 0x5F)) {
        /* SMBus quick write */
        args.read_write = 0;            /* write */
        args.command    = 0;
        args.size       = 0;            /* I2C_SMBUS_QUICK */
        args.data       = NULL;
    } else {
        /* SMBus read byte */
        args.read_write = 1;            /* read */
        args.command    = 0;
        args.size       = 1;            /* I2C_SMBUS_BYTE */
        args.data       = &data;
    }

    return ioctl(fd, I2C_SMBUS, &args);
}

/*
 * Try to read a single register from the device.
 * Returns 0 on success, -1 on failure.
 */
static int read_reg8(int fd, int addr, uint8_t reg, uint8_t *val) {
    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        if (errno == EBUSY) {
            if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0)
                return -1;
        } else {
            return -1;
        }
    }

    memset(&data, 0, sizeof(data));
    data.byte = 0;

    args.read_write = 1;                /* read */
    args.command    = reg;
    args.size       = 2;                /* I2C_SMBUS_BYTE_DATA */
    args.data       = &data;

    if (ioctl(fd, I2C_SMBUS, &args) < 0)
        return -1;

    *val = data.byte & 0xFF;
    return 0;
}

/*
 * Known chip identification table.
 * Each entry: read register `reg` and compare with `id`.
 */
struct chip_id {
    uint8_t     reg;
    uint8_t     id;
    const char *name;
};

static const struct chip_id known_chips[] = {
    /* Bosch environmental sensors (WHO_AM_I at 0xD0) */
    { 0xD0, 0x60, "BME280" },
    { 0xD0, 0x58, "BMP280" },
    { 0xD0, 0x55, "BMP180" },
    { 0xD0, 0x61, "BME680" },

    /* InvenSense IMUs (WHO_AM_I at 0x75) */
    { 0x75, 0x68, "MPU6050" },
    { 0x75, 0x70, "MPU6500" },
    { 0x75, 0x71, "MPU9250" },
    { 0x75, 0x98, "ICM-20689" },

    /* ST sensors (WHO_AM_I at 0x0F) */
    { 0x0F, 0x33, "LIS3DH" },
    { 0x0F, 0x6A, "LSM6DS3" },
    { 0x0F, 0x69, "LSM6DSO" },
    { 0x0F, 0xBB, "LIS3MDL" },

    /* Honeywell compass (ID reg A at 0x0A) */
    { 0x0A, 0x48, "HMC5883L" },

    /* ADS1115 config reg won't help, but AHT20 etc. are tricky */
    { 0, 0, NULL }
};

/*
 * Well-known fixed-address devices that don't have a readable chip ID.
 */
struct addr_id {
    uint8_t     addr;
    const char *name;
};

static const struct addr_id known_addrs[] = {
    { 0x23, "BH1750 (light sensor)" },
    { 0x5C, "BH1750 (light sensor, ADDR=HIGH)" },
    { 0x27, "PCF8574 (LCD/GPIO)" },
    { 0x38, "AHT20 (temp/hum)" },
    { 0x3C, "SSD1306 (OLED)" },
    { 0x3D, "SSD1306 (OLED)" },
    { 0x44, "SHT3x (temp/hum)" },
    { 0x45, "SHT3x (temp/hum, ADDR=HIGH)" },
    { 0x48, "ADS1115 / TMP102 / PCF8591" },
    { 0x49, "ADS1115 / TMP102" },
    { 0x4A, "ADS1115" },
    { 0x4B, "ADS1115" },
    { 0x50, "EEPROM (24Cxx)" },
    { 0x51, "PCF8563 (RTC) / EEPROM" },
    { 0x57, "MAX30102 (pulse oximeter) / EEPROM" },
    { 0x68, "DS1307 / DS3231 (RTC) / MPU6050 / PCF8523" },
    { 0x69, "MPU6050 (alt) / ICM-20948" },
    { 0x76, "BME280 / BMP280 / MS5611" },
    { 0x77, "BME280 / BMP280 / BMP180" },
    { 0, NULL }
};

static const char *identify_chip(int fd, int addr) {
    uint8_t val;

    /* try register-based identification first */
    for (const struct chip_id *c = known_chips; c->name; c++) {
        if (read_reg8(fd, addr, c->reg, &val) == 0 && val == c->id)
            return c->name;
    }

    /* fall back to address-based guess */
    for (const struct addr_id *a = known_addrs; a->name; a++) {
        if (a->addr == addr)
            return a->name;
    }

    return NULL;
}

static void write_str(int fd, const char *s) {
    (void)write(fd, s, strlen(s));
}

static void scan_bus_child(int bus, int outfd) {
    char dev[64];
    char line[256];
    int fd;
    int found = 0;

    snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        snprintf(line, sizeof(line), "/dev/i2c-%d:\n  error: %s\n", bus, strerror(errno));
        write_str(outfd, line);
        _exit(1);
    }

    snprintf(line, sizeof(line), "/dev/i2c-%d:\n", bus);
    write_str(outfd, line);

    for (int addr = 0x03; addr <= 0x77; addr++) {
        if (probe_addr(fd, addr) < 0)
            continue;

        found++;
        const char *name = identify_chip(fd, addr);

        if (name)
            snprintf(line, sizeof(line), "  0x%02X  %s\n", addr, name);
        else
            snprintf(line, sizeof(line), "  0x%02X  (unknown device)\n", addr);

        write_str(outfd, line);
    }

    if (!found) {
        write_str(outfd, "  (no devices found)\n");
    }

    close(fd);
    _exit(0);
}

static void scan_bus_with_timeout(int bus) {
    int pipefd[2];
    pid_t pid;
    int status;
    int waited = 0;
    char buf[512];
    ssize_t n;

    if (pipe(pipefd) < 0) {
        printf("/dev/i2c-%d:\n  error: pipe failed\n\n", bus);
        return;
    }

    pid = fork();
    if (pid < 0) {
        printf("/dev/i2c-%d:\n  error: fork failed\n\n", bus);
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        scan_bus_child(bus, pipefd[1]);
    }

    close(pipefd[1]);

    while (waited < SCAN_TIMEOUT_SEC * 10) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        usleep(100000);
        waited++;
    }

    if (waited >= SCAN_TIMEOUT_SEC * 10) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pipefd[0]);
        printf("/dev/i2c-%d:\n  timeout: scan timed out after %ds\n\n", bus, SCAN_TIMEOUT_SEC);
        return;
    }

    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        (void)write(1, buf, n);
    }

    close(pipefd[0]);
    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        print_help(argv[0]);
        return 0;
    }

    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        print_help(argv[0]);
        return 0;
    }

    printf("I2C scan\n");
    printf("========\n\n");

    if (!strcmp(argv[1], "--bus")) {
        int bus;
        if (argc < 3) {
            fprintf(stderr, "Error: --bus requires number\n");
            return 1;
        }
        bus = atoi(argv[2]);
        scan_bus_with_timeout(bus);
        return 0;
    }

    if (!strcmp(argv[1], "--bus-all")) {
        int buses[MAX_BUSES];
        int count;

        count = list_i2c_buses(buses);
        if (count <= 0) {
            fprintf(stderr, "No i2c buses found\n");
            return 1;
        }

        for (int i = 0; i < count; i++)
            scan_bus_with_timeout(buses[i]);

        return 0;
    }

    print_help(argv[0]);
    return 0;
}
