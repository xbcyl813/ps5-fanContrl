// SPDX-License-Identifier: GPL-3.0-only
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <unistd.h>

//
// 1. 基础宏定义与配置结构体
//
#define DATA_CONFIG_PATH "/data/fan_control.ini"
#define FAN_DEVICE "/dev/icc_fan"

#define ICC_FAN_SET_AUTOSERVO 0xC01C8F07UL
#define ICC_FAN_GET_AUTOSERVO 0xC01C8F08UL
#define AUTOSERVO_BUF_SIZE    28
#define AUTOSERVO_SELECTOR_0  0
#define TARGET_TEMP_OFFSET    5

#define DEFAULT_THRESHOLD 80
#define MIN_THRESHOLD 30
#define MAX_THRESHOLD 90
#define USB_MAX_COUNT 8
#define PROCESS_NAME "ps5-fancontrl.elf"
#define DEFAULT_INTERVAL_SECONDS 15
#define MIN_INTERVAL_SECONDS 1
#define MAX_INTERVAL_SECONDS 3600
#define MAX_SOC_SENSORS 16
#define MAX_CONSECUTIVE_FAILURES 5

typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;

typedef struct {
    int threshold;
    int interval;
    bool show_status;
} fan_config_t;

typedef enum {
    CONFIG_MISSING,
    CONFIG_INVALID,
    CONFIG_VALID
} config_read_result_t;

//
// 2. PS5 原生系统函数外部链接声明与通知函数
//
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelGetCpuTemperature(int *out_celsius);
int sceKernelGetSocSensorTemperature(int sensor_id, int *out_celsius);

static void notify(const char *fmt, ...) {
    notify_request_t req;
    va_list args;
    memset(&req, 0, sizeof(req));
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

//
// 3. 单实例守护进程清理
//
static pid_t find_pid_by_name(const char *name) {
    int mib[4] = {1, 14, 8, 0}; // KERN_PROC_ALL
    pid_t mypid = getpid();
    pid_t found = -1;
    size_t buf_size = 0;
    uint8_t *buf;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0) {
        return -1;
    }
    buf = malloc(buf_size);
    if (buf == NULL) {
        return -1;
    }
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
        free(buf);
        return -1;
    }

    for (uint8_t *ptr = buf; ptr < buf + buf_size;) {
        int ki_structsize = *(int *)ptr;
        if (ki_structsize <= 0) {
            break;
        }
        pid_t ki_pid = *(pid_t *)(ptr + 72);
        char *ki_tdname = (char *)(ptr + 447);

        if (ki_pid != mypid && ki_pid > 0 && strcmp(name, ki_tdname) == 0) {
            found = ki_pid;
            break;
        }
        ptr += ki_structsize;
    }
    free(buf);
    return found;
}

static bool stop_previous_instances(void) {
    pid_t pid;
    while ((pid = find_pid_by_name(PROCESS_NAME)) > 0) {
        if (kill(pid, SIGKILL) != 0) {
            notify("[fan-control] Failed to kill old daemon pid %d", pid);
            return false;
        }
        sleep(1);
    }
    return true;
}

//
// 4. 配置文件解析逻辑
//
static void trim(char *s) {
    char *start = s;
    size_t len;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static bool parse_int_value(const char *s, int *out) {
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_config_line(char *line, fan_config_t *config, bool *has_threshold, bool *invalid) {
    char *eq, *key, *value;
    int parsed_value;

    trim(line);
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';' || line[0] == '[') {
        return false;
    }
    eq = strchr(line, '=');
    if (eq == NULL) {
        return false;
    }
    *eq = '\0';
    key = line;
    value = eq + 1;
    trim(key);
    trim(value);

    if (strcmp(key, "target_temperature") == 0) {
        if (!parse_int_value(value, &parsed_value) || parsed_value < MIN_THRESHOLD || parsed_value > MAX_THRESHOLD) {
            *invalid = true;
            return false;
        }
        config->threshold = parsed_value;
        *has_threshold = true;
        return true;
    }
    if (strcmp(key, "show_status") == 0) {
        if (!parse_int_value(value, &parsed_value) || (parsed_value != 0 && parsed_value != 1)) {
            *invalid = true;
            return false;
        }
        config->show_status = (parsed_value == 1);
        return true;
    }
    if (strcmp(key, "interval") == 0) {
        if (!parse_int_value(value, &parsed_value) || parsed_value < MIN_INTERVAL_SECONDS || parsed_value > MAX_INTERVAL_SECONDS) {
            *invalid = true;
            return false;
        }
        config->interval = parsed_value;
        return true;
    }
    return false;
}

static config_read_result_t read_config_from_path(const char *path, fan_config_t *config_out) {
    FILE *fp = fopen(path, "r");
    char line[256];
    fan_config_t config;
    bool has_threshold = false;
    bool invalid = false;

    if (fp == NULL) {
        return CONFIG_MISSING;
    }
    config.threshold = DEFAULT_THRESHOLD;
    config.interval = DEFAULT_INTERVAL_SECONDS;
    config.show_status = true;

    while (fgets(line, sizeof(line), fp) != NULL) {
        parse_config_line(line, &config, &has_threshold, &invalid);
        if (invalid) {
            break;
        }
    }
    fclose(fp);
    if (!has_threshold || invalid) {
        return CONFIG_INVALID;
    }
    *config_out = config;
    return CONFIG_VALID;
}

static bool read_config(fan_config_t *config_out) {
    char path[64];
    config_read_result_t result;
    bool found_config_file = false;

    // 1. 优先读取主机内置存储 (/data/fan_control.ini)
    result = read_config_from_path(DATA_CONFIG_PATH, config_out);
    if (result == CONFIG_VALID) return true;
    if (result == CONFIG_INVALID) found_config_file = true;

    // 2. 其次读取 U 盘根目录 (/mnt/usb0/fan_control.ini ~ /mnt/usb7/fan_control.ini)
    for (int i = 0; i < USB_MAX_COUNT; i++) {
        snprintf(path, sizeof(path), "/mnt/usb%d/fan_control.ini", i);
        result = read_config_from_path(path, config_out);
        if (result == CONFIG_VALID) return true;
        if (result == CONFIG_INVALID) found_config_file = true;
    }

    if (found_config_file) {
        return false;
    }

    // 3. 均未找到时使用默认参数
    config_out->threshold = DEFAULT_THRESHOLD;
    config_out->interval = DEFAULT_INTERVAL_SECONDS;
    config_out->show_status = true;
    return true;
}
//
// 5. 自动伺服安全读写与校验
//
static int get_autoservo_blob(int fd, uint8_t cfg[AUTOSERVO_BUF_SIZE]) {
    memset(cfg, 0, AUTOSERVO_BUF_SIZE);
    cfg[2] = AUTOSERVO_SELECTOR_0;
    return ioctl(fd, ICC_FAN_GET_AUTOSERVO, cfg);
}

static int ensure_fan_target(uint8_t desired_target_c) {
    uint8_t current[AUTOSERVO_BUF_SIZE];
    uint8_t verify[AUTOSERVO_BUF_SIZE];

    int fd = open(FAN_DEVICE, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    if (get_autoservo_blob(fd, current) < 0) {
        close(fd);
        return -1;
    }

    if (current[TARGET_TEMP_OFFSET] == desired_target_c) {
        close(fd);
        return 0;
    }

    current[TARGET_TEMP_OFFSET] = desired_target_c;

    if (ioctl(fd, ICC_FAN_SET_AUTOSERVO, current) < 0) {
        close(fd);
        return -1;
    }

    if (get_autoservo_blob(fd, verify) < 0) {
        close(fd);
        return -1;
    }

    close(fd);

    if (verify[TARGET_TEMP_OFFSET] != desired_target_c) {
        return -1;
    }

    return 1;
}

//
// 6. 动态枚举温度传感器
//
static void notify_status(int threshold) {
    int cpu_c = -1;
    int max_soc_c = -1;
    char soc_str[16];
    char cpu_str[16];

    bool cpu_ok = (sceKernelGetCpuTemperature(&cpu_c) == 0 && cpu_c >= 0 && cpu_c <= 130);

    for (int id = 0; id < MAX_SOC_SENSORS; id++) {
        int temp = -1;
        if (sceKernelGetSocSensorTemperature(id, &temp) != 0) {
            break;
        }
        if (temp >= 0 && temp <= 130 && temp > max_soc_c) {
            max_soc_c = temp;
        }
    }

    snprintf(cpu_str, sizeof(cpu_str), cpu_ok ? "%d °C" : "-- °C", cpu_c);
    snprintf(soc_str, sizeof(soc_str), (max_soc_c >= 0) ? "%d °C" : "-- °C", max_soc_c);

    notify("[fan-control]\nCPU：%s\nSoC(Max)：%s\nTARGET：%d °C", cpu_str, soc_str, threshold);
}

//
// 7. 主循环：寄存器级持久化目标守护
//
int main(void) {
    fan_config_t config;
    int sleep_seconds = DEFAULT_INTERVAL_SECONDS;
    int consecutive_failures = 0;

    syscall(SYS_thr_set_name, -1, PROCESS_NAME);

    if (!stop_previous_instances()) {
        return 1;
    }

    if (!read_config(&config)) {
        notify("[fan-control] Invalid config file, exiting safely.");
        return 1;
    }

    for (;;) {
        fan_config_t new_config;
        if (read_config(&new_config)) {
            config = new_config;
            sleep_seconds = config.interval;
        }

        int status = ensure_fan_target((uint8_t)config.threshold);

        if (status == 1) {
            consecutive_failures = 0;
            if (config.show_status) {
                notify_status(config.threshold);
            }
        } else if (status == 0) {
            consecutive_failures = 0;
        } else {
            consecutive_failures++;
            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                notify("[fan-control] Hardware IO error x%d, reverting to stock control.", consecutive_failures);
                return 1;
            }
        }

        sleep(sleep_seconds);
    }

    return 0;
}
