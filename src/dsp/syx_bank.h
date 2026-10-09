/*
 * Runtime microKORG bank loading (included once, by dsp.c, after presets.h).
 *
 * At instance creation the module scans <module_dir>/banks/ for *.syx bank dumps: ALL PROGRAM DATA (function
 * 4C, 128 programs) or ALL DATA (function 50, programs + global data), with the microKORG header F0 42 3g 58
 * or the microKORG S header F0 42 3g 00 01 40. It decodes up to SYX_MAX_BANKS of them in alphabetical
 * order into static storage and offers them next to the built-in bank (presets.h). All file I/O and
 * decoding happen there, never on the audio path; selecting a bank later only swaps a pointer.
 *
 * The decoder is a field-for-field port of tools/extracts_presets.py (parse_program / parse_timbre), so a
 * dump decoded here gives the same floats as the generated presets.h. Keep the two in step.
 */
#ifndef TINYK_SYX_BANK_H
#define TINYK_SYX_BANK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

#define SYX_MAX_BANKS      16
#define SYX_NUM_PROGRAMS   128
#define SYX_PROGRAM_SIZE   254
#define SYX_LABEL_LEN      24
#define SYX_BANK_NAME_LEN  48
#define SYX_MAX_FILE_BYTES 65536

typedef struct {
    char name[SYX_BANK_NAME_LEN];                   /* file name without .syx */
    struct Preset presets[SYX_NUM_PROGRAMS];
    char labels[SYX_NUM_PROGRAMS][SYX_LABEL_LEN];   /* presets[i].label points here */
} syx_bank_t;

static syx_bank_t g_syx_banks[SYX_MAX_BANKS];
static int g_syx_bank_count = 0;
static uint8_t g_syx_file_buf[SYX_MAX_FILE_BYTES];
static uint8_t g_syx_unpacked[SYX_MAX_FILE_BYTES];

static double syx_clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
static double syx_unit(int raw) { return syx_clamp01(raw / 127.0); }
static double syx_bipolar(int raw) { return syx_clamp01(0.5 + (raw - 64) / 126.0); }

/* Vocoder programs have no synth-timbre data; they get this generic carrier (extracts_presets.VOCODER_CARRIER) */
static const struct TimbreParams SYX_VOCODER_CARRIER = {
    .wave1 = 0.0f, .pulse_width = 0.0f, .wave2 = 0.0f, .detune = (float)(0.5 + 0.12 / 48.0), .sync_ring = 0.0f,
    .osc_mix = 0.5f, .sub_level = 0.0f, .noise_level = 0.0f, .level = 0.9f, .portamento = 0.0f, .transpose = 0.5f,
    .dwgs = 0.0f, .cutoff = 0.72f, .resonance = 0.12f, .filter_type = 0.0f, .keytrack = 0.75f,
    .env_int = 0.5f, .drive = 0.0f,
    .attack1 = 0.2f, .decay1 = 0.5f, .sustain1 = 1.0f, .release1 = 0.45f,
    .attack2 = 0.25f, .decay2 = 0.5f, .sustain2 = 1.0f, .release2 = 0.45f,
    .lfo1_wave = 0.0f, .lfo1_rate = 0.5f, .lfo1_keysync = 0.0f, .lfo1_sync_note = 0.0f,
    .lfo2_wave = 0.0f, .lfo2_rate = 0.5f, .lfo2_keysync = 0.0f, .lfo2_sync_note = 0.0f,
    .patch1_int = 0.5f, .patch2_int = 0.5f, .patch3_int = 0.5f, .patch4_int = 0.5f,
    .osc1_ctrl1 = 0.0f, .osc1_ctrl2 = 0.0f,
    .assign = 0.5f, .unison_detune = 0.0f, .pan = 0.5f, .trigger_multi = 0.0f,
    .osc1_level = 1.0f, .osc2_level = 1.0f / 128.0f, .amp_level = 1.0f,
};

/* Korg 7-to-8 decode: each 8-byte group is [MSB bits][7 data bytes]. Returns bytes written. */
static size_t syx_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o < cap; i += 8) {
        uint8_t msbs = in[i];
        for (size_t j = 0; j < 7 && i + 1 + j < n && o < cap; j++) {
            uint8_t b = in[i + 1 + j];
            out[o++] = (msbs & (1u << j)) ? (uint8_t)(b | 0x80) : b;
        }
    }
    return o;
}

static void syx_parse_timbre(const uint8_t *p, int t, struct TimbreParams *o) {
    static const int MODSEL_TO_ENGINE[4] = { 0, 2, 1, 3 }; /* hw off, ring, sync, both -> engine off, sync, ring, both */
    int osc1_wave = p[t + 7] & 0x07;
    int wave1 = osc1_wave > 6 ? 0 : osc1_wave;
    int osc2_wave = p[t + 12] & 0x03;
    if (osc2_wave > 2) osc2_wave = 2;
    int mod_select = MODSEL_TO_ENGINE[(p[t + 12] >> 4) & 0x03];
    double osc2_semis = (p[t + 13] - 64) + (p[t + 14] - 64) / 63.0 * 0.5;
    /* transpose + tune, and the program's keyboard octave (byte 37, signed -3..+3) folded in */
    int kbd_octave = p[37] >= 128 ? (int)p[37] - 256 : (int)p[37];
    double transpose_semis = (p[t + 5] - 64) + (p[t + 3] - 64) / 100.0 + 12.0 * (kbd_octave < -3 ? -3 : (kbd_octave > 3 ? 3 : kbd_octave));

    double osc1_lvl = p[t + 16] / 127.0, osc2_lvl = p[t + 17] / 127.0;
    double loudest = osc1_lvl > osc2_lvl ? osc1_lvl : osc2_lvl;
    double osc_mix;
    if (loudest <= 0.0) osc_mix = 0.5;
    else if (osc1_lvl >= osc2_lvl) osc_mix = 0.5 * osc2_lvl / osc1_lvl;
    else osc_mix = 1.0 - 0.5 * osc1_lvl / osc2_lvl;

    o->wave1 = (float)(wave1 / 6.0);
    o->pulse_width = (float)syx_unit(p[t + 8]);
    o->osc1_ctrl1 = (float)syx_unit(p[t + 8]);  /* Osc 1 Control 1 / 2: what they drive depends on the wave */
    o->osc1_ctrl2 = (float)syx_unit(p[t + 9]);
    int assign = (p[t + 1] >> 6) & 0x03;                 /* 0 mono, 1 poly, 2 unison */
    o->assign = (float)((assign > 2 ? 2 : assign) / 2.0);
    o->unison_detune = (float)syx_unit(p[t + 2]);        /* cents */
    o->pan = (float)syx_bipolar(p[t + 26]);
    o->trigger_multi = (p[t + 1] & 0x08) ? 1.0f : 0.0f;
    o->wave2 = (float)(osc2_wave / 2.0);
    o->detune = (float)syx_clamp01(0.5 + osc2_semis / 48.0);
    o->sync_ring = (float)(mod_select / 3.0);
    o->osc_mix = (float)syx_clamp01(osc_mix);
    o->sub_level = 0.0f;
    o->noise_level = (float)syx_unit(p[t + 18]);
    o->level = (float)syx_clamp01(loudest * syx_unit(p[t + 25]));
    o->portamento = (float)syx_unit(p[t + 15]);
    o->transpose = (float)syx_clamp01(0.5 + transpose_semis / 48.0);
    o->dwgs = (float)syx_clamp01((p[t + 10] < 63 ? p[t + 10] : 63) / 63.0);
    o->cutoff = (float)syx_unit(p[t + 20]);
    o->resonance = (float)syx_unit(p[t + 21]);
    o->filter_type = (float)((p[t + 19] & 0x03) / 3.0);
    o->keytrack = (float)syx_bipolar(p[t + 24]);
    o->env_int = (float)syx_bipolar(p[t + 22]);
    o->drive = (p[t + 27] & 1) ? 0.5f : 0.0f;
    /* the three level knobs themselves, (raw + 1) / 128 (0 = not recorded): see tools/extracts_presets.py */
    o->osc1_level = (float)(((p[t + 16] > 127 ? 127 : p[t + 16]) + 1) / 128.0);
    o->osc2_level = (float)(((p[t + 17] > 127 ? 127 : p[t + 17]) + 1) / 128.0);
    o->amp_level = (float)(((p[t + 25] > 127 ? 127 : p[t + 25]) + 1) / 128.0);
    o->attack1 = (float)syx_unit(p[t + 30]);
    o->decay1 = (float)syx_unit(p[t + 31]);
    o->sustain1 = (float)syx_unit(p[t + 32]);
    o->release1 = (float)syx_unit(p[t + 33]);
    o->attack2 = (float)syx_unit(p[t + 34]);
    o->decay2 = (float)syx_unit(p[t + 35]);
    o->sustain2 = (float)syx_unit(p[t + 36]);
    o->release2 = (float)syx_unit(p[t + 37]);

    float *lfo_wave[2] = { &o->lfo1_wave, &o->lfo2_wave }, *lfo_rate[2] = { &o->lfo1_rate, &o->lfo2_rate };
    float *lfo_key[2] = { &o->lfo1_keysync, &o->lfo2_keysync }, *lfo_note[2] = { &o->lfo1_sync_note, &o->lfo2_sync_note };
    for (int n = 0; n < 2; n++) {
        int base = t + 38 + 3 * n;
        int sync = p[base + 2];
        int ks = (p[base] >> 4) & 0x03;
        int note = sync & 0x1F;
        *lfo_wave[n] = (float)((p[base] & 0x03) / 3.0);
        *lfo_key[n] = (float)((ks > 2 ? 2 : ks) / 2.0);
        *lfo_rate[n] = (float)syx_unit(p[base + 1]);
        *lfo_note[n] = (sync & 0x80) ? (float)(((note > 14 ? 14 : note) + 1) / 15.0) : 0.0f;
    }
    float *src[4] = { &o->patch1_src, &o->patch2_src, &o->patch3_src, &o->patch4_src };
    float *dst[4] = { &o->patch1_dst, &o->patch2_dst, &o->patch3_dst, &o->patch4_dst };
    float *amt[4] = { &o->patch1_int, &o->patch2_int, &o->patch3_int, &o->patch4_int };
    for (int n = 0; n < 4; n++) {
        int route = p[t + 44 + 2 * n];
        int s = route & 0x0F, d = (route >> 4) & 0x0F;
        *src[n] = (float)((s > 7 ? 7 : s) / 7.0);
        *dst[n] = (float)((d > 7 ? 7 : d) / 7.0);
        *amt[n] = (float)syx_bipolar(p[t + 45 + 2 * n]);
    }
}

/* Label: the stored 12-character name when there is one ("A.11 Trancey" style names already carry the
 * code), otherwise just the matrix code ("A.11"). */
static void syx_make_label(const uint8_t *p, int idx, char *out) {
    char code[8], name[16];
    snprintf(code, sizeof code, "%c.%d%d", idx < 64 ? 'A' : 'B', (idx % 64) / 8 + 1, idx % 8 + 1);
    int n = 0;
    for (int i = 0; i < 12; i++) {
        if (p[i] >= 32 && p[i] <= 126) name[n++] = (char)p[i];
    }
    while (n > 0 && name[n - 1] == ' ') n--;
    name[n] = '\0';
    const char *s = name;
    while (*s == ' ') s++;
    int has_code = (s[0] == 'A' || s[0] == 'B') && s[1] == '.' && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]);
    if (!*s) snprintf(out, SYX_LABEL_LEN, "%s", code);
    else if (has_code) snprintf(out, SYX_LABEL_LEN, "%s", s);
    else snprintf(out, SYX_LABEL_LEN, "%s %s", code, s);
}

static void syx_parse_program(const uint8_t *p, int idx, struct Preset *out, char *label) {
    int mode_bits = (p[16] >> 4) & 0x03;
    double delay_fb = syx_unit(p[21]);
    syx_make_label(p, idx, label);
    out->label = label;
    out->voice_mode = (mode_bits == 3) ? 2 : ((mode_bits == 2) ? 1 : 0); /* 2 = vocoder, see presets.h */
    out->chorus_mix = (float)syx_unit(p[24]);
    out->delay_time = (float)syx_unit(p[20]);
    out->delay_feedback = (float)delay_fb;
    out->delay_mix = (float)delay_fb;
    /* byte 19: bit 7 = delay tempo sync, bits 0-3 = time base (1/32 .. 1/1); stored as (index + 1) / 15 */
    out->delay_sync = (p[19] & 0x80) ? (float)(((p[19] & 0x0F) > 14 ? 14 : (p[19] & 0x0F)) + 1) / 15.0f : 0.0f;
    /* Mod FX: 23 LFO speed, 24 depth (chorus_mix), 25 type (0 Chorus/Flanger, 1 Ensemble, 2 Phaser) */
    out->modfx_speed = (float)syx_unit(p[23]);
    out->modfx_type = (float)((p[25] > 2 ? 2 : p[25]) / 2.0);
    out->delay_type = (float)((p[22] > 2 ? 2 : p[22]) / 2.0); /* 0 Stereo, 1 Cross, 2 L/R */
    /* EQ: 26 Hi frequency (0..29), 27 Hi gain (64 +- 12 dB), 28 Low frequency (0..29), 29 Low gain */
    int hi_gain = (int)p[27] - 64, low_gain = (int)p[29] - 64;
    out->eq.hi_freq = (float)((p[26] > 29 ? 29 : p[26]) / 29.0);
    out->eq.hi_gain = (float)(0.5 + (hi_gain < -12 ? -12 : (hi_gain > 12 ? 12 : hi_gain)) / 24.0);
    out->eq.low_freq = (float)((p[28] > 29 ? 29 : p[28]) / 29.0);
    out->eq.low_gain = (float)(0.5 + (low_gain < -12 ? -12 : (low_gain > 12 ? 12 : low_gain)) / 24.0);
    /* arpeggiator: 14 length - 1, 15 pattern (bit set = rest), 32 on / latch / target / key sync, 33 type / range,
     * 34 gate, 35 resolution, 36 swing (signed); normalized as extracts_presets.ARP_FIELDS */
    int target = (p[32] >> 4) & 0x03, type = p[33] & 0x0F, range = p[33] >> 4, swing = p[36] >= 128 ? p[36] - 256 : p[36];
    out->arp.on = (p[32] & 0x80) ? 1.0f : 0.0f;
    out->arp.latch = (p[32] & 0x40) ? 1.0f : 0.0f;
    out->arp.key_sync = (p[32] & 0x01) ? 1.0f : 0.0f;
    out->arp.target = (float)((target > 2 ? 2 : target) / 2.0);
    out->arp.type = (float)((type > 5 ? 5 : type) / 5.0);
    out->arp.range = (float)((range > 3 ? 3 : range) / 3.0);
    out->arp.gate = (float)((p[34] > 100 ? 100 : p[34]) / 100.0);
    out->arp.resolution = (float)((p[35] > 5 ? 5 : p[35]) / 5.0);
    out->arp.swing = (float)syx_clamp01(0.5 + (swing < -100 ? -100 : (swing > 100 ? 100 : swing)) / 200.0);
    out->arp.length = (float)((p[14] > 7 ? 7 : p[14]) / 7.0);
    out->arp.pattern = (float)(p[15] / 255.0);
    if (mode_bits == 3) { /* vocoder: no engine equivalent, generic carrier, plays as Single */
        out->t1 = SYX_VOCODER_CARRIER;
        out->t2 = SYX_VOCODER_CARRIER;
    } else {
        syx_parse_timbre(p, 38, &out->t1);
        syx_parse_timbre(p, 146, &out->t2);
    }
}

/* Decodes a 128-program bank dump in memory. Returns 0 on success, -1 if it is not one (wrong header,
 * single-program dump, truncated or oversized data): such files are skipped, never half-loaded. */
static int syx_decode_message(const uint8_t *d, size_t s, size_t e, syx_bank_t *bank) {
    /* d[s] = F0, d[e] = F7 */
    if (e < s + 8 || d[s + 1] != 0x42 || (d[s + 2] & 0xF0) != 0x30) return -1;
    size_t func;
    if (d[s + 3] == 0x58) func = s + 4;                                              /* microKORG */
    else if (d[s + 3] == 0x00 && d[s + 4] == 0x01 && d[s + 5] == 0x40) func = s + 6; /* microKORG S */
    else return -1;
    if (d[func] != 0x4C && d[func] != 0x50) return -1; /* ALL PROGRAM DATA / ALL DATA dump; 0x40 (one program) is skipped */
    size_t n = syx_unpack(d + func + 1, e - (func + 1), g_syx_unpacked, sizeof g_syx_unpacked);
    if (n < (size_t)SYX_NUM_PROGRAMS * SYX_PROGRAM_SIZE) return -1;
    for (int i = 0; i < SYX_NUM_PROGRAMS; i++) {
        syx_parse_program(g_syx_unpacked + (size_t)i * SYX_PROGRAM_SIZE, i, &bank->presets[i], bank->labels[i]);
    }
    return 0;
}

/* Decodes the first 128-program bank dump among the SysEx messages in a file. Returns 0 on success, -1 if
 * there is none (wrong header, single-program dump, truncated data, not SysEx at all): such files are
 * skipped, never half-loaded. */
static int syx_decode_dump(const uint8_t *d, size_t len, syx_bank_t *bank) {
    size_t i = 0;
    while (i < len) {
        while (i < len && d[i] != 0xF0) i++;
        size_t e = i + 1;
        while (e < len && d[e] != 0xF7 && d[e] != 0xF0) e++;
        if (e >= len) return -1;
        if (d[e] == 0xF7 && syx_decode_message(d, i, e, bank) == 0) return 0;
        i = e; /* next message (an F0 without its F7 restarts the search there) */
    }
    return -1;
}

/* Loads one file into the next free bank slot. Returns the bank number (1..N) or -1. */
static int syx_load_file(const char *path, const char *display_name) {
    if (g_syx_bank_count >= SYX_MAX_BANKS) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t len = fread(g_syx_file_buf, 1, sizeof g_syx_file_buf, f);
    fclose(f);
    syx_bank_t *bank = &g_syx_banks[g_syx_bank_count];
    if (syx_decode_dump(g_syx_file_buf, len, bank) != 0) return -1;
    snprintf(bank->name, sizeof bank->name, "%s", display_name);
    char *dot = strrchr(bank->name, '.');
    if (dot) *dot = '\0';
    return ++g_syx_bank_count;
}

static int syx_has_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char *x = name + n - 4;
    return x[0] == '.' && tolower((unsigned char)x[1]) == 's' && tolower((unsigned char)x[2]) == 'y' &&
           tolower((unsigned char)x[3]) == 'x';
}

static int syx_name_cmp(const void *a, const void *b) {
    const char *x = (const char *)a, *y = (const char *)b;
    for (; *x && *y; x++, y++) {
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
        if (cx != cy) return cx - cy;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

/* Scans dir for *.syx, decodes them in alphabetical (case-insensitive) order. Returns banks loaded. */
static int syx_scan_dir(const char *dir) {
    static char names[64][256];
    int count = 0;
#if defined(_WIN32)
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && syx_has_ext(fd.cFileName) && count < 64)
            snprintf(names[count++], sizeof names[0], "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] != '.' && syx_has_ext(de->d_name) && count < 64)
            snprintf(names[count++], sizeof names[0], "%s", de->d_name);
    }
    closedir(dp);
#endif
    qsort(names, (size_t)count, sizeof names[0], syx_name_cmp);
    int loaded = 0;
    for (int i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (syx_load_file(path, names[i]) > 0) loaded++;
    }
    return loaded;
}

#endif /* TINYK_SYX_BANK_H */
