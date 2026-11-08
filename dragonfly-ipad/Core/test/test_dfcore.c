/*
 * Host-side test of dfcore against a simulated Dragonfly / NIRscan Nano.
 *
 * The mock implements the HID framing and the handful of commands the app uses, and serves a
 * genuine dlpspec-serialized scan blob (a fake Gaussian-ish lamp spectrum), so this exercises
 * packet splitting, multi-report responses, busy handling, the scan sequence and decoding.
 *
 *   make -C Core test        (after ./fetch_dlpspec.sh)
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dfcore.h"
#include "dlpspec.h"

/* ---------------------------------------------------------------- mock device ---------- */

typedef struct {
    uint8_t  rx[4096];      /* message being assembled from host reports */
    size_t   rx_len, rx_need;
    uint8_t  tx[8192];      /* queued reports to host */
    size_t   tx_len, tx_pos;

    uint8_t *scan_blob;
    size_t   scan_size, file_pos;
    uint8_t  cfg_blob[512];
    size_t   cfg_size;

    int      status_polls_left;   /* scan "in progress" for this many polls */
    int      busy_once;           /* answer the next read with BUSY */
    int      active_cfg;
    uint16_t repeats;
    uint8_t  pga_fixed, pga;
    int      applied_patterns;
    int      writes_seen;
} mock;

static void queue_response(mock *m, int resp, const uint8_t *data, size_t len)
{
    uint8_t msg[4096];
    msg[0] = (uint8_t)(resp << 4);
    msg[1] = 0;
    msg[2] = (uint8_t)len;
    msg[3] = (uint8_t)(len >> 8);
    memcpy(msg + 4, data, len);
    size_t total = len + 4, off = 0;
    m->tx_len = m->tx_pos = 0;
    while (off < total) {
        size_t n = total - off > 64 ? 64 : total - off;
        memset(m->tx + m->tx_len, 0, 64);
        memcpy(m->tx + m->tx_len, msg + off, n);
        m->tx_len += 64;
        off += n;
    }
}

static void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

static void handle(mock *m)
{
    uint8_t flags = m->rx[0];
    uint8_t cmd = m->rx[4], grp = m->rx[5];
    const uint8_t *d = m->rx + 6;
    int is_read = flags & 0x80;
    uint8_t out[4096];

    if (is_read && m->busy_once) { m->busy_once = 0; queue_response(m, 2, NULL, 0); return; }

#define KEY(g, c) (((g) << 8) | (c))
    switch (KEY(grp, cmd)) {
    case KEY(0x02, 0x16): /* version */
        memset(out, 0, 28); put32(out, 0x020103); queue_response(m, 0, out, 28); break;
    case KEY(0x02, 0x33): memcpy(out, "DF190042", 8); queue_response(m, 0, out, 8); break;
    case KEY(0x02, 0x3C): memset(out, 0, 16); memcpy(out, "DF1914", 6); queue_response(m, 0, out, 16); break;
    case KEY(0x02, 0x22): out[0] = 3; queue_response(m, 0, out, 1); break;
    case KEY(0x02, 0x23): out[0] = (uint8_t)m->active_cfg; queue_response(m, 0, out, 1); break;
    case KEY(0x02, 0x24): m->active_cfg = d[0]; queue_response(m, 0, NULL, 0); break;
    case KEY(0x02, 0x20): /* cfg read: [size][blob] */
        assert(m->rx[2] == 6 && d[0] == 1);
        put32(out, (uint32_t)m->cfg_size); memcpy(out + 4, m->cfg_blob, m->cfg_size);
        queue_response(m, 0, out, 4 + m->cfg_size); break;
    case KEY(0x02, 0x1E): { /* apply cfg: decode what we got */
        size_t len = (size_t)m->rx[2] | (size_t)m->rx[3] << 8;
        assert(len == 2 + 2 * sizeof(scanConfig));
        df_config c;
        assert(df_decode_config(d, len - 2, &c) == DF_OK);
        m->applied_patterns = c.sections[0].num_patterns;
        put32(out, (uint32_t)m->applied_patterns); queue_response(m, 0, out, 4); break; }
    case KEY(0x02, 0x2E): m->repeats = (uint16_t)(d[0] | d[1] << 8); queue_response(m, 0, NULL, 0); break;
    case KEY(0x04, 0x0C): m->pga_fixed = d[0]; m->pga = d[1]; queue_response(m, 0, NULL, 0); break;
    case KEY(0x02, 0x28): out[0] = m->pga; queue_response(m, 0, out, 1); break;
    case KEY(0x02, 0x37): put32(out, 400); queue_response(m, 0, out, 4); break;
    case KEY(0x04, 0x05): queue_response(m, 0, NULL, 0); break;
    case KEY(0x02, 0x18): assert(d[0] == 0); m->status_polls_left = 3; m->file_pos = 0;
                          queue_response(m, 0, NULL, 0); break;
    case KEY(0x04, 0x03): put32(out, 0x1 | (m->status_polls_left > 0 ? 0x2 : 0));
                          if (m->status_polls_left > 0) m->status_polls_left--;
                          if (m->status_polls_left == 1) m->busy_once = 1;
                          queue_response(m, 0, out, 4); break;
    case KEY(0x00, 0x2D): assert(d[0] == 0); put32(out, (uint32_t)m->scan_size);
                          queue_response(m, 0, out, 4); break;
    case KEY(0x00, 0x2E): { size_t n = m->scan_size - m->file_pos; if (n > 300) n = 300;
                          queue_response(m, 0, m->scan_blob + m->file_pos, n); m->file_pos += n; break; }
    default:
        fprintf(stderr, "mock: unknown command %02x %02x\n", grp, cmd);
        queue_response(m, 1, NULL, 0);
    }
}

static int mock_write(void *ctx, const uint8_t p[64])
{
    mock *m = ctx;
    m->writes_seen++;
    if (m->rx_len == 0) {
        size_t len = (size_t)p[2] | (size_t)p[3] << 8;
        m->rx_need = len + 4;
    }
    memcpy(m->rx + m->rx_len, p, 64);
    m->rx_len += 64;
    if (m->rx_len >= m->rx_need) { handle(m); m->rx_len = 0; }
    return 0;
}

static int mock_read(void *ctx, uint8_t p[64], uint32_t timeout)
{
    (void)timeout;
    mock *m = ctx;
    if (m->tx_pos >= m->tx_len) return 0;  /* timeout */
    memcpy(p, m->tx + m->tx_pos, 64);
    m->tx_pos += 64;
    return 64;
}

/* ---------------------------------------------------------------- fixtures -------------- */

static void make_scan_blob(mock *m)
{
    uScanData *u = calloc(1, sizeof *u);
    scanData *s = &u->data;
    s->header_version = 1;
    strcpy(s->scan_name, "test");
    s->system_temp_hundredths = 2512;
    s->detector_temp_hundredths = -1950;
    s->humidity_hundredths = 4033;
    /* Plausible DF1914-ish calibration: ~1.17 nm per DMD column. Pixel->nm is a quadratic. */
    s->calibration_coeffs.PixelToWavelengthCoeffs[0] = 1750.0;
    s->calibration_coeffs.PixelToWavelengthCoeffs[1] = -1.17;
    s->calibration_coeffs.PixelToWavelengthCoeffs[2] = 0.0;
    memcpy(s->serial_number, "DF190042", 8);
    s->pga = 64;
    s->scan_type = COLUMN_TYPE;
    strcpy(s->config_name, "Column 900-1700");
    s->wavelength_start_nm = 900;
    s->wavelength_end_nm = 1700;
    s->width_px = 6;
    s->num_patterns = 228;
    s->num_repeats = 6;
    s->black_pattern_first = 0;
    s->black_pattern_period = 24;
    /* ADC samples: patterns interleaved with a black pattern every 24. */
    int n = s->num_patterns + (s->num_patterns + 23) / 24;
    s->adc_data_length = (uint16_t)n;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i % 24 == 0) { s->adc_data[i] = 1000; continue; }
        double x = (double)k++ / s->num_patterns;
        s->adc_data[i] = 1000 + (int32_t)(250000 * exp(-pow((x - 0.55) / 0.3, 2)));
    }
    size_t need = 0;
    assert(dlpspec_get_scan_data_dump_size(u, &need) == DLPSPEC_PASS);
    m->scan_blob = calloc(1, need);
    assert(dlpspec_scan_write_data(u, m->scan_blob, need) == DLPSPEC_PASS);
    m->scan_size = need;
    free(u);
}

static void make_cfg_blob(mock *m)
{
    df_config c = {0};
    c.scan_type = 1;
    strcpy(c.name, "Hadamard full");
    memcpy(c.serial_number, "DF190042", 8);
    c.num_repeats = 10;
    c.num_sections = 1;
    c.sections[0] = (df_section){ .scan_type = 1, .width_px = 7, .wavelength_start_nm = 950,
                                  .wavelength_end_nm = 1650, .num_patterns = 200 };
    assert(df_encode_config(&c, m->cfg_blob, sizeof m->cfg_blob, &m->cfg_size) == DF_OK);
}

/* ---------------------------------------------------------------- tests ----------------- */

int main(void)
{
    mock m = {0};
    make_scan_blob(&m);
    make_cfg_blob(&m);
    printf("scan blob: %zu bytes, config blob: %zu bytes (apply payload %zu)\n",
           m.scan_size, m.cfg_size, 2 * sizeof(scanConfig));
    assert(m.cfg_size <= 2 * sizeof(scanConfig));

    df_device dev;
    df_device_init(&dev, &m, mock_write, mock_read);

    df_version v;
    assert(df_get_version(&dev, &v) == DF_OK && v.tiva_sw == 0x020103);
    char sn[9], model[17];
    assert(df_get_serial_number(&dev, sn) == DF_OK && strcmp(sn, "DF190042") == 0);
    assert(df_get_model_name(&dev, model) == DF_OK && strcmp(model, "DF1914") == 0);

    assert(df_get_num_configs(&dev) == 3);
    assert(df_set_active_config(&dev, 2) == DF_OK && df_get_active_config(&dev) == 2);

    df_config cfg;
    assert(df_read_config(&dev, 1, &cfg) == DF_OK);
    assert(strcmp(cfg.name, "Hadamard full") == 0 && cfg.scan_type == 1);
    assert(cfg.sections[0].wavelength_start_nm == 950 && cfg.sections[0].num_patterns == 200);
    assert(cfg.num_repeats == 10);

    /* Applying a config is a multi-report write (126 byte payload). */
    int before = m.writes_seen;
    cfg.sections[0].num_patterns = 150;
    assert(df_apply_config(&dev, &cfg) == 150);
    assert(m.writes_seen - before == 3);

    assert(df_set_num_repeats(&dev, 12) == DF_OK && m.repeats == 12);
    assert(df_set_pga_gain(&dev, true, 16) == DF_OK && m.pga_fixed == 1);
    assert(df_get_pga_gain(&dev) == 16);

    df_scan_result *r = calloc(1, sizeof *r);
    int rc = df_scan(&dev, NULL, NULL, NULL, r);
    if (rc != DF_OK) { fprintf(stderr, "scan failed: %s\n", df_strerror(rc)); return 1; }
    printf("decoded %d points, %.1f..%.1f nm, config '%s', temp %.2f C, hum %.2f %%\n",
           r->length, r->wavelength_nm[0], r->wavelength_nm[r->length - 1], r->config_name,
           r->system_temp_c, r->humidity_pct);
    assert(r->length == 228);
    assert(fabs(r->system_temp_c - 25.12) < 1e-9 && fabs(r->detector_temp_c + 19.5) < 1e-9);
    assert(strcmp(r->serial_number, "DF190042") == 0);
    int peak = 0;
    for (int i = 1; i < r->length; i++) if (r->intensity[i] > r->intensity[peak]) peak = i;
    printf("peak %d counts at %.1f nm\n", r->intensity[peak], r->wavelength_nm[peak]);
    assert(r->wavelength_nm[0] >= 890 && r->wavelength_nm[r->length - 1] <= 1710);
    assert(r->intensity[peak] > 200000);

    /* Garbage must not decode. */
    uint8_t junk[300] = {1, 2, 3};
    assert(df_decode_scan(junk, sizeof junk, r) == DF_ERR_DECODE);

    free(r);
    puts("all tests passed");
    return 0;
}
