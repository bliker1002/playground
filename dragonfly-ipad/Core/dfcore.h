/*
 * dfcore.h — USB protocol + spectrum decoding for OtO Photonics Dragonfly (DF) spectrometers.
 *
 * The Dragonfly enumerates as USB VID 0x0451 / PID 0x4200 and speaks the HID protocol of the
 * TI DLP NIRscan Nano reference design (Tiva microcontroller + DLPC150 + DMD). This module is
 * transport-agnostic: give it two callbacks that send/receive raw 64-byte HID reports and it
 * handles packet framing, commands, the scan sequence and decoding of the scan blob into a
 * wavelength/intensity spectrum (via TI's DLP Spectrum Library, "dlpspec").
 *
 * Plain C so it can be unit-tested on any machine and called from Swift.
 */
#ifndef DFCORE_H
#define DFCORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DF_USB_VID          0x0451
#define DF_USB_PID          0x4200
#define DF_PACKET_SIZE      64
#define DF_MAX_POINTS       864
#define DF_MAX_SECTIONS     5

/* Error codes (negative). */
enum {
    DF_OK            =  0,
    DF_ERR_IO        = -1,   /* transport failure / device unplugged */
    DF_ERR_TIMEOUT   = -2,   /* no answer within the timeout */
    DF_ERR_NACK      = -3,   /* device rejected the command */
    DF_ERR_BUSY      = -4,   /* device kept answering "busy" */
    DF_ERR_DECODE    = -5,   /* could not interpret the scan / config blob */
    DF_ERR_NOMEM     = -6,
    DF_ERR_CANCELLED = -7,
    DF_ERR_ARG       = -8,
};

const char *df_strerror(int err);

/* ---- Transport ------------------------------------------------------------------------- */

/* Send one 64-byte output report (no report-ID byte). Return 0 on success, <0 on error. */
typedef int (*df_write_fn)(void *ctx, const uint8_t packet[DF_PACKET_SIZE]);
/* Receive one 64-byte input report. Return number of bytes (>0), 0 on timeout, <0 on error. */
typedef int (*df_read_fn)(void *ctx, uint8_t packet[DF_PACKET_SIZE], uint32_t timeout_ms);

typedef struct {
    void        *ctx;
    df_write_fn  write;
    df_read_fn   read;
    uint8_t      seq;
    uint32_t     timeout_ms;      /* per-packet read timeout, default 3000 */
} df_device;

void df_device_init(df_device *dev, void *ctx, df_write_fn write, df_read_fn read);

/* ---- Low level --------------------------------------------------------------------------- */

/* Write command (group, cmd) with payload; returns >=0 (the device's 4-byte return value when
 * it sends one, else 0) or a DF_ERR_*. */
int df_cmd_write(df_device *dev, uint8_t group, uint8_t cmd, const uint8_t *data, size_t len);
/* Read command with optional 32-bit parameter. Response payload copied to out (up to out_cap);
 * returns payload length or a DF_ERR_*. */
int df_cmd_read(df_device *dev, uint8_t group, uint8_t cmd, const uint32_t *param,
                uint8_t *out, size_t out_cap);

/* ---- Device information ------------------------------------------------------------------ */

typedef struct {
    uint32_t tiva_sw;        /* MAJOR.MINOR.PATCH in bytes 2,1,0 */
    uint32_t dlpc_sw;
    uint32_t dlpc_flash;
    uint32_t spec_lib;
    uint32_t cal_data;
    uint32_t ref_cal_data;
    uint32_t cfg_data;
} df_version;

int df_get_version(df_device *dev, df_version *out);
int df_get_serial_number(df_device *dev, char out[9]);
int df_get_model_name(df_device *dev, char out[17]);
int df_read_device_status(df_device *dev, uint32_t *status);   /* bit 0x02 = scan in progress */
int df_reset_error_status(df_device *dev);

/* ---- Scan configuration ------------------------------------------------------------------ */

typedef struct {
    uint8_t  scan_type;           /* 0 column, 1 hadamard (section type for slew) */
    uint8_t  width_px;
    uint16_t wavelength_start_nm;
    uint16_t wavelength_end_nm;
    uint16_t num_patterns;
    uint16_t exposure_time;       /* EXP_TIME enum: 0=0.635ms 1=1.27 2=2.45 3=5.08 4=15.24 5=30.48 6=60.96 */
} df_section;

typedef struct {
    uint8_t    scan_type;         /* 0 column, 1 hadamard, 2 slew */
    uint16_t   config_index;
    char       serial_number[9];
    char       name[41];
    uint16_t   num_repeats;
    uint8_t    num_sections;      /* 1 for column / hadamard */
    df_section sections[DF_MAX_SECTIONS];
} df_config;

int df_get_num_configs(df_device *dev);                 /* >=0 count or DF_ERR_* */
int df_get_active_config(df_device *dev);               /* >=0 index or DF_ERR_* */
int df_set_active_config(df_device *dev, uint8_t index);
int df_read_config(df_device *dev, uint8_t index, df_config *out);
/* Apply a temporary (not stored) column / hadamard config. Uses sections[0] only.
 * Returns the number of patterns the device generated, or DF_ERR_*. */
int df_apply_config(df_device *dev, const df_config *cfg);

/* Pure helpers (no device) — used by df_read_config / df_apply_config and by the tests. */
int df_decode_config(const uint8_t *blob, size_t size, df_config *out);
int df_encode_config(const df_config *cfg, uint8_t *buf, size_t cap, size_t *out_len);

/* ---- Acquisition settings ---------------------------------------------------------------- */

int df_set_num_repeats(df_device *dev, uint16_t repeats);
/* fixed=false -> automatic gain. Valid fixed gains: 1,2,4,8,16,32,64. */
int df_set_pga_gain(df_device *dev, bool fixed, uint8_t gain);
int df_get_pga_gain(df_device *dev);
int df_get_estimated_scan_time_ms(df_device *dev);

/* ---- Scanning ---------------------------------------------------------------------------- */

typedef struct {
    int      length;                          /* number of valid points */
    double   wavelength_nm[DF_MAX_POINTS];
    int32_t  intensity[DF_MAX_POINTS];

    char     scan_name[21];
    char     serial_number[9];
    char     config_name[41];
    uint8_t  scan_type;
    uint16_t num_repeats;
    uint8_t  pga;
    double   system_temp_c;
    double   detector_temp_c;
    double   humidity_pct;
    uint16_t lamp_pd;
} df_scan_result;

/* Optional progress callback: phase 0 = scanning, 1 = downloading (fraction 0..1). */
typedef void (*df_progress_fn)(void *ctx, int phase, double fraction);

/* Start a scan with the active config, wait for it, download the raw blob (malloc'd; caller
 * frees). cancel may be NULL. */
int df_scan_raw(df_device *dev, volatile int *cancel, df_progress_fn progress, void *progress_ctx,
                uint8_t **blob_out, size_t *size_out);

/* Interpret a raw scan blob (as returned by df_scan_raw) into a spectrum. */
int df_decode_scan(const uint8_t *blob, size_t size, df_scan_result *out);

/* Convenience: df_scan_raw + df_decode_scan. */
int df_scan(df_device *dev, volatile int *cancel, df_progress_fn progress, void *progress_ctx,
            df_scan_result *out);

#ifdef __cplusplus
}
#endif

#endif /* DFCORE_H */
