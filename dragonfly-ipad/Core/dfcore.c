/*
 * dfcore.c — see dfcore.h.
 *
 * Wire format (TI NIRscan Nano HID protocol, all multi-byte fields little-endian):
 *
 *   byte 0      flags: bit7 = read(1)/write(0), bit6 = reply wanted,
 *                      bits5..4 = response status from device (0 ok, 1 error, 2 busy),
 *                      bits1..0 = destination (0 = command handler)
 *   byte 1      sequence number
 *   byte 2..3   length of everything after this 4-byte header
 *   byte 4      command byte  (CMD3)
 *   byte 5      group byte    (CMD2)
 *   byte 6..    command data
 *
 * Messages longer than one 64-byte report continue in following reports (raw continuation,
 * no extra header). Device responses use the same 4-byte header followed directly by data.
 */
#include "dfcore.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dlpspec.h"          /* TI DLP Spectrum Library (fetched by fetch_dlpspec.sh) */

#define FLAG_READ        0x80
#define FLAG_REPLY       0x40
#define RESP_OK          0
#define RESP_ERROR       1
#define RESP_BUSY        2
#define HEADER_SIZE      4
#define MAX_MSG_DATA     512
#define BUSY_RETRIES     50
#define BUSY_DELAY_MS    100

/* Command codes: (group, command) */
#define G_FILE 0x00
#define G_SCAN 0x02
#define G_SYS  0x03
#define G_STAT 0x04

#define C_FILE_GET_READSIZE   0x2D
#define C_FILE_GET_DATA       0x2E
#define C_TIVA_VER            0x16
#define C_PERFORM_SCAN        0x18
#define C_SCAN_GET_STATUS     0x19
#define C_SCAN_CFG_APPLY      0x1E
#define C_SCAN_CFG_READ       0x20
#define C_SCAN_CFG_NUM        0x22
#define C_SCAN_GET_ACT_CFG    0x23
#define C_SCAN_SET_ACT_CFG    0x24
#define C_GET_PGA             0x28
#define C_SCAN_NUM_REPEATS    0x2E
#define C_SERIAL_NUMBER_READ  0x33
#define C_READ_SCAN_TIME      0x37
#define C_MODEL_NAME_READ     0x3C
#define C_READ_DEVICE_STATUS  0x03
#define C_RESET_ERROR_STATUS  0x05
#define C_SET_FIXED_PGA       0x0C

#define FILE_SCAN_DATA        0
#define STATUS_SCAN_IN_PROGRESS 0x00000002u

/* ------------------------------------------------------------------------------------------ */

const char *df_strerror(int err)
{
    switch (err) {
    case DF_OK:            return "OK";
    case DF_ERR_IO:        return "USB communication error";
    case DF_ERR_TIMEOUT:   return "The spectrometer did not answer in time";
    case DF_ERR_NACK:      return "The spectrometer rejected the command";
    case DF_ERR_BUSY:      return "The spectrometer is busy";
    case DF_ERR_DECODE:    return "Could not interpret the data from the spectrometer";
    case DF_ERR_NOMEM:     return "Out of memory";
    case DF_ERR_CANCELLED: return "Cancelled";
    case DF_ERR_ARG:       return "Invalid argument";
    default:               return "Unknown error";
    }
}

static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void df_device_init(df_device *dev, void *ctx, df_write_fn write, df_read_fn read)
{
    memset(dev, 0, sizeof(*dev));
    dev->ctx = ctx;
    dev->write = write;
    dev->read = read;
    dev->timeout_ms = 3000;
}

/* Send a complete message (header + payload) split into 64-byte reports. */
static int send_message(df_device *dev, const uint8_t *msg, size_t total)
{
    uint8_t pkt[DF_PACKET_SIZE];
    size_t off = 0;
    while (off < total) {
        size_t n = total - off > DF_PACKET_SIZE ? DF_PACKET_SIZE : total - off;
        memset(pkt, 0, sizeof pkt);
        memcpy(pkt, msg + off, n);
        if (dev->write(dev->ctx, pkt) < 0)
            return DF_ERR_IO;
        off += n;
    }
    return DF_OK;
}

/* Read one response message. On success returns payload length (copied to out, truncated to
 * out_cap). */
static int receive_message(df_device *dev, uint8_t *out, size_t out_cap)
{
    uint8_t pkt[DF_PACKET_SIZE];
    int r = dev->read(dev->ctx, pkt, dev->timeout_ms);
    if (r < 0) return DF_ERR_IO;
    if (r == 0) return DF_ERR_TIMEOUT;

    int resp = (pkt[0] >> 4) & 0x3;
    size_t length = (size_t)pkt[2] | (size_t)pkt[3] << 8;
    if (resp == RESP_BUSY) return DF_ERR_BUSY;
    if (resp == RESP_ERROR) return DF_ERR_NACK;

    size_t got = 0;
    size_t first = length < DF_PACKET_SIZE - HEADER_SIZE ? length : DF_PACKET_SIZE - HEADER_SIZE;
    if (out && out_cap) memcpy(out, pkt + HEADER_SIZE, first < out_cap ? first : out_cap);
    got = first;

    while (got < length) {
        r = dev->read(dev->ctx, pkt, dev->timeout_ms);
        if (r < 0) return DF_ERR_IO;
        if (r == 0) return DF_ERR_TIMEOUT;
        size_t n = length - got < DF_PACKET_SIZE ? length - got : DF_PACKET_SIZE;
        if (out && got < out_cap)
            memcpy(out + got, pkt, (got + n <= out_cap) ? n : out_cap - got);
        got += n;
    }
    return (int)length;
}

static int write_once(df_device *dev, uint8_t group, uint8_t cmd, const uint8_t *data, size_t len)
{
    uint8_t msg[HEADER_SIZE + 2 + MAX_MSG_DATA];
    if (len > MAX_MSG_DATA) return DF_ERR_ARG;
    size_t plen = len + 2;
    msg[0] = FLAG_REPLY;                 /* write, reply wanted */
    msg[1] = dev->seq++;
    msg[2] = (uint8_t)(plen & 0xFF);
    msg[3] = (uint8_t)(plen >> 8);
    msg[4] = cmd;
    msg[5] = group;
    if (len) memcpy(msg + 6, data, len);
    int r = send_message(dev, msg, HEADER_SIZE + plen);
    if (r < 0) return r;

    uint8_t ack[8] = {0};
    r = receive_message(dev, ack, sizeof ack);
    if (r < 0) return r;
    if (r == 4) return (int)le32(ack);
    return DF_OK;
}

int df_cmd_write(df_device *dev, uint8_t group, uint8_t cmd, const uint8_t *data, size_t len)
{
    int r = DF_ERR_BUSY;
    for (int i = 0; i < BUSY_RETRIES; i++) {
        r = write_once(dev, group, cmd, data, len);
        if (r != DF_ERR_BUSY) return r;
        sleep_ms(BUSY_DELAY_MS);
    }
    return r;
}

static int read_once(df_device *dev, uint8_t group, uint8_t cmd, const uint32_t *param,
                     uint8_t *out, size_t out_cap)
{
    uint8_t msg[HEADER_SIZE + 6];
    size_t plen = param ? 6 : 2;
    msg[0] = FLAG_READ | FLAG_REPLY;
    msg[1] = 0;
    msg[2] = (uint8_t)plen;
    msg[3] = 0;
    msg[4] = cmd;
    msg[5] = group;
    if (param) {
        msg[6] = (uint8_t)(*param);
        msg[7] = (uint8_t)(*param >> 8);
        msg[8] = (uint8_t)(*param >> 16);
        msg[9] = (uint8_t)(*param >> 24);
    }
    int r = send_message(dev, msg, HEADER_SIZE + plen);
    if (r < 0) return r;
    r = receive_message(dev, out, out_cap);
    if (r == 0) return DF_ERR_NACK;      /* zero-length reply means NACK in this protocol */
    return r;
}

int df_cmd_read(df_device *dev, uint8_t group, uint8_t cmd, const uint32_t *param,
                uint8_t *out, size_t out_cap)
{
    int r = DF_ERR_BUSY;
    for (int i = 0; i < BUSY_RETRIES; i++) {
        r = read_once(dev, group, cmd, param, out, out_cap);
        if (r != DF_ERR_BUSY) return r;
        sleep_ms(BUSY_DELAY_MS);
    }
    return r;
}

/* ---- Device information ------------------------------------------------------------------ */

int df_get_version(df_device *dev, df_version *out)
{
    uint8_t buf[64] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_TIVA_VER, NULL, buf, sizeof buf);
    if (r < 0) return r;
    out->tiva_sw      = le32(buf + 0);
    out->dlpc_sw      = le32(buf + 4);
    out->dlpc_flash   = le32(buf + 8);
    out->spec_lib     = le32(buf + 12);
    out->cal_data     = le32(buf + 16);
    out->ref_cal_data = le32(buf + 20);
    out->cfg_data     = le32(buf + 24);
    return DF_OK;
}

static void copy_cstr(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap && src[i]; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[i] = 0;
}

int df_get_serial_number(df_device *dev, char out[9])
{
    uint8_t buf[64] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_SERIAL_NUMBER_READ, NULL, buf, sizeof buf);
    if (r < 0) return r;
    copy_cstr(out, 9, buf, 8);
    return DF_OK;
}

int df_get_model_name(df_device *dev, char out[17])
{
    uint8_t buf[64] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_MODEL_NAME_READ, NULL, buf, sizeof buf);
    if (r < 0) return r;
    copy_cstr(out, 17, buf, 16);
    return DF_OK;
}

int df_read_device_status(df_device *dev, uint32_t *status)
{
    uint8_t buf[16] = {0};
    int r = df_cmd_read(dev, G_STAT, C_READ_DEVICE_STATUS, NULL, buf, sizeof buf);
    if (r < 0) return r;
    *status = le32(buf);
    return DF_OK;
}

int df_reset_error_status(df_device *dev)
{
    int r = df_cmd_write(dev, G_STAT, C_RESET_ERROR_STATUS, NULL, 0);
    return r < 0 ? r : DF_OK;
}

/* ---- Scan configuration ------------------------------------------------------------------ */

int df_get_num_configs(df_device *dev)
{
    uint8_t buf[16] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_SCAN_CFG_NUM, NULL, buf, sizeof buf);
    return r < 0 ? r : buf[0];
}

int df_get_active_config(df_device *dev)
{
    uint8_t buf[16] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_SCAN_GET_ACT_CFG, NULL, buf, sizeof buf);
    return r < 0 ? r : buf[0];
}

int df_set_active_config(df_device *dev, uint8_t index)
{
    int r = df_cmd_write(dev, G_SCAN, C_SCAN_SET_ACT_CFG, &index, 1);
    return r < 0 ? r : DF_OK;
}

int df_decode_config(const uint8_t *blob, size_t size, df_config *out)
{
    size_t cap = size + sizeof(uScanConfig) * 2 + 256;
    uint8_t *buf = calloc(1, cap);
    if (!buf) return DF_ERR_NOMEM;
    memcpy(buf, blob, size);

    if (dlpspec_scan_read_configuration(buf, cap) != DLPSPEC_PASS) {
        free(buf);
        return DF_ERR_DECODE;
    }
    const uScanConfig *c = (const uScanConfig *)buf;
    memset(out, 0, sizeof *out);
    out->scan_type = c->scanCfg.scan_type;
    out->config_index = c->scanCfg.scanConfigIndex;
    copy_cstr(out->serial_number, sizeof out->serial_number,
              (const uint8_t *)c->scanCfg.ScanConfig_serial_number, NANO_SER_NUM_LEN);
    copy_cstr(out->name, sizeof out->name, (const uint8_t *)c->scanCfg.config_name,
              SCAN_CFG_FILENAME_LEN);

    if (c->scanCfg.scan_type == SLEW_TYPE) {
        const slewScanConfig *s = &c->slewScanCfg;
        out->num_repeats = s->head.num_repeats;
        out->num_sections = s->head.num_sections > DF_MAX_SECTIONS ? DF_MAX_SECTIONS
                                                                   : s->head.num_sections;
        for (int i = 0; i < out->num_sections; i++) {
            out->sections[i].scan_type = s->section[i].section_scan_type;
            out->sections[i].width_px = s->section[i].width_px;
            out->sections[i].wavelength_start_nm = s->section[i].wavelength_start_nm;
            out->sections[i].wavelength_end_nm = s->section[i].wavelength_end_nm;
            out->sections[i].num_patterns = s->section[i].num_patterns;
            out->sections[i].exposure_time = s->section[i].exposure_time;
        }
    } else {
        const scanConfig *s = &c->scanCfg;
        out->num_repeats = s->num_repeats;
        out->num_sections = 1;
        out->sections[0].scan_type = s->scan_type;
        out->sections[0].width_px = s->width_px;
        out->sections[0].wavelength_start_nm = s->wavelength_start_nm;
        out->sections[0].wavelength_end_nm = s->wavelength_end_nm;
        out->sections[0].num_patterns = s->num_patterns;
        out->sections[0].exposure_time = 0;
    }
    free(buf);
    return DF_OK;
}

int df_read_config(df_device *dev, uint8_t index, df_config *out)
{
    uint8_t buf[MAX_MSG_DATA] = {0};
    uint32_t param = index;
    int r = df_cmd_read(dev, G_SCAN, C_SCAN_CFG_READ, &param, buf, sizeof buf);
    if (r < 0) return r;
    if (r < 4) return DF_ERR_DECODE;
    uint32_t size = le32(buf);
    if (size == 0 || size > (uint32_t)r - 4) return DF_ERR_DECODE;
    return df_decode_config(buf + 4, size, out);
}

int df_encode_config(const df_config *cfg, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (cfg->scan_type != COLUMN_TYPE && cfg->scan_type != HADAMARD_TYPE) return DF_ERR_ARG;
    uScanConfig c;
    memset(&c, 0, sizeof c);
    c.scanCfg.scan_type = cfg->scan_type;
    c.scanCfg.scanConfigIndex = cfg->config_index;
    memcpy(c.scanCfg.ScanConfig_serial_number, cfg->serial_number, NANO_SER_NUM_LEN);
    strncpy(c.scanCfg.config_name, cfg->name, SCAN_CFG_FILENAME_LEN - 1);
    c.scanCfg.wavelength_start_nm = cfg->sections[0].wavelength_start_nm;
    c.scanCfg.wavelength_end_nm = cfg->sections[0].wavelength_end_nm;
    c.scanCfg.width_px = cfg->sections[0].width_px;
    c.scanCfg.num_patterns = cfg->sections[0].num_patterns;
    c.scanCfg.num_repeats = cfg->num_repeats ? cfg->num_repeats : 1;

    size_t need = 0;
    if (dlpspec_get_scan_config_dump_size(&c, &need) != DLPSPEC_PASS) return DF_ERR_DECODE;
    if (need > cap) return DF_ERR_NOMEM;
    if (dlpspec_scan_write_configuration(&c, buf, need) != DLPSPEC_PASS) return DF_ERR_DECODE;
    *out_len = need;
    return DF_OK;
}

int df_apply_config(df_device *dev, const df_config *cfg)
{
    /* The TI host software always sends a fixed 2*sizeof(scanConfig) byte payload. */
    uint8_t payload[sizeof(scanConfig) * 2];
    size_t len = 0;
    memset(payload, 0, sizeof payload);
    int r = df_encode_config(cfg, payload, sizeof payload, &len);
    if (r < 0) return r;
    return df_cmd_write(dev, G_SCAN, C_SCAN_CFG_APPLY, payload, sizeof payload);
}

/* ---- Acquisition settings ---------------------------------------------------------------- */

int df_set_num_repeats(df_device *dev, uint16_t repeats)
{
    uint8_t d[2] = { (uint8_t)repeats, (uint8_t)(repeats >> 8) };
    int r = df_cmd_write(dev, G_SCAN, C_SCAN_NUM_REPEATS, d, 2);
    return r < 0 ? r : DF_OK;
}

int df_set_pga_gain(df_device *dev, bool fixed, uint8_t gain)
{
    uint8_t d[2] = { fixed ? 1 : 0, gain };
    int r = df_cmd_write(dev, G_STAT, C_SET_FIXED_PGA, d, 2);
    return r < 0 ? r : DF_OK;
}

int df_get_pga_gain(df_device *dev)
{
    uint8_t buf[16] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_GET_PGA, NULL, buf, sizeof buf);
    return r < 0 ? r : buf[0];
}

int df_get_estimated_scan_time_ms(df_device *dev)
{
    uint8_t buf[16] = {0};
    int r = df_cmd_read(dev, G_SCAN, C_READ_SCAN_TIME, NULL, buf, sizeof buf);
    return r < 0 ? r : (int)le32(buf);
}

/* ---- Scanning ---------------------------------------------------------------------------- */

int df_scan_raw(df_device *dev, volatile int *cancel, df_progress_fn progress, void *pctx,
                uint8_t **blob_out, size_t *size_out)
{
    *blob_out = NULL;
    *size_out = 0;

    df_reset_error_status(dev);                       /* not supported by all firmware */
    int est = df_get_estimated_scan_time_ms(dev);
    if (est <= 0 || est > 600000) est = 10000;

    uint8_t store_on_sd = 0;
    int r = df_cmd_write(dev, G_SCAN, C_PERFORM_SCAN, &store_on_sd, 1);
    if (r < 0) return r;

    int legacy_status = 0;
    uint64_t start = now_ms();
    uint64_t limit = (uint64_t)est * 3 + 10000;
    for (;;) {
        sleep_ms(50);
        if (cancel && *cancel) return DF_ERR_CANCELLED;
        uint32_t status = 0;
        if (!legacy_status) {
            r = df_read_device_status(dev, &status);
            if (r == DF_ERR_NACK) legacy_status = 1;   /* older firmware: use SCAN_GET_STATUS */
            else if (r == DF_OK && !(status & STATUS_SCAN_IN_PROGRESS)) break;
            else if (r < 0 && r != DF_ERR_BUSY && r != DF_ERR_TIMEOUT) return r;
        }
        if (legacy_status) {
            uint8_t done[8] = {0};
            r = df_cmd_read(dev, G_SCAN, C_SCAN_GET_STATUS, NULL, done, sizeof done);
            if (r > 0 && done[0]) break;
            if (r < 0 && r != DF_ERR_BUSY && r != DF_ERR_TIMEOUT) return r;
        }
        uint64_t elapsed = now_ms() - start;
        if (elapsed > limit) return DF_ERR_TIMEOUT;
        if (progress) {
            double f = (double)elapsed / (double)est;
            progress(pctx, 0, f > 0.99 ? 0.99 : f);
        }
    }
    if (progress) progress(pctx, 0, 1.0);

    uint8_t sz[16] = {0};
    uint32_t file = FILE_SCAN_DATA;
    r = df_cmd_read(dev, G_FILE, C_FILE_GET_READSIZE, &file, sz, sizeof sz);
    if (r < 0) return r;
    uint32_t total = le32(sz);
    if (total == 0 || total > 16u * 1024u * 1024u) return DF_ERR_DECODE;

    uint8_t *blob = malloc(total);
    if (!blob) return DF_ERR_NOMEM;
    uint8_t chunk[4096];
    size_t got = 0;
    while (got < total) {
        if (cancel && *cancel) { free(blob); return DF_ERR_CANCELLED; }
        r = df_cmd_read(dev, G_FILE, C_FILE_GET_DATA, NULL, chunk, sizeof chunk);
        if (r < 0) { free(blob); return r; }
        if ((size_t)r > sizeof chunk) { free(blob); return DF_ERR_DECODE; }
        size_t n = (size_t)r;
        if (got + n > total) n = total - got;
        memcpy(blob + got, chunk, n);
        got += n;
        if (progress) progress(pctx, 1, (double)got / (double)total);
    }
    *blob_out = blob;
    *size_out = total;
    return DF_OK;
}

int df_decode_scan(const uint8_t *blob, size_t size, df_scan_result *out)
{
    size_t cap = size > SCAN_DATA_BLOB_SIZE ? size : SCAN_DATA_BLOB_SIZE;
    uint8_t *buf = calloc(1, cap);
    scanResults *res = calloc(1, sizeof(scanResults));
    if (!buf || !res) { free(buf); free(res); return DF_ERR_NOMEM; }
    memcpy(buf, blob, size);

    int r = DF_OK;
    if (dlpspec_scan_interpret(buf, cap, res) != DLPSPEC_PASS) {
        r = DF_ERR_DECODE;
        goto done;
    }

    memset(out, 0, sizeof *out);
    int n = res->length;
    if (n < 0) n = 0;
    if (n > DF_MAX_POINTS) n = DF_MAX_POINTS;
    out->length = n;
    for (int i = 0; i < n; i++) {
        out->wavelength_nm[i] = res->wavelength[i];
        out->intensity[i] = res->intensity[i];
    }
    copy_cstr(out->scan_name, sizeof out->scan_name, (const uint8_t *)res->scan_name, SCAN_NAME_LEN);
    copy_cstr(out->serial_number, sizeof out->serial_number, (const uint8_t *)res->serial_number,
              NANO_SER_NUM_LEN);
    copy_cstr(out->config_name, sizeof out->config_name, (const uint8_t *)res->cfg.head.config_name,
              SCAN_CFG_FILENAME_LEN);
    out->scan_type = res->cfg.head.scan_type;
    out->num_repeats = res->cfg.head.num_repeats;
    out->pga = res->pga;
    out->system_temp_c = res->system_temp_hundredths / 100.0;
    out->detector_temp_c = res->detector_temp_hundredths / 100.0;
    out->humidity_pct = res->humidity_hundredths / 100.0;
    out->lamp_pd = res->lamp_pd;

done:
    free(buf);
    free(res);
    return r;
}

int df_scan(df_device *dev, volatile int *cancel, df_progress_fn progress, void *pctx,
            df_scan_result *out)
{
    uint8_t *blob = NULL;
    size_t size = 0;
    int r = df_scan_raw(dev, cancel, progress, pctx, &blob, &size);
    if (r < 0) return r;
    r = df_decode_scan(blob, size, out);
    free(blob);
    return r;
}
