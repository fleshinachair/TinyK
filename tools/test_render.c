/*
 * Headless renderer for the TinyK DSP engine.
 *
 *   test_render <preset 0-127> <midi note[,note...]> <hold seconds> <total seconds> <out.wav>
 *
 * Plays the note(s) together for <hold> seconds, releases it, and writes <total> seconds of
 * 44.1 kHz stereo 16-bit PCM. Build natively (from the repo root):
 *
 *   zig cc -O2 -Isrc/dsp tools/test_render.c src/dsp/dsp.c -lm -o tools/test_render
 *   (or: gcc -O2 -Isrc/dsp tools/test_render.c src/dsp/dsp.c -lm -o tools/test_render)
 */
#include "dsp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static synth_engine_t synth;

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_wav(const char *path, const int16_t *lr, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t data_bytes = (uint32_t)frames * 4;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16); put_u16(f, 1); put_u16(f, 2);
    put_u32(f, MOVE_SAMPLE_RATE); put_u32(f, MOVE_SAMPLE_RATE * 4); put_u16(f, 4); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
    fwrite(lr, 4, (size_t)frames, f);
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <preset> <note> <hold_s> <total_s> <out.wav>\n", argv[0]);
        return 2;
    }
    int preset = atoi(argv[1]);
    int notes[8], num_notes = 0;
    for (char *tok = strtok(argv[2], ","); tok && num_notes < 8; tok = strtok(NULL, ",")) {
        notes[num_notes++] = atoi(tok);
    }
    double hold_s = atof(argv[3]);
    double total_s = atof(argv[4]);
    int notes_ok = num_notes > 0;
    for (int i = 0; i < num_notes; i++) notes_ok = notes_ok && notes[i] >= 0 && notes[i] <= 127;
    if (preset < 0 || preset > 127 || !notes_ok || hold_s < 0 || total_s <= 0) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    int total = (int)(total_s * MOVE_SAMPLE_RATE);
    int hold = (int)(hold_s * MOVE_SAMPLE_RATE);
    int16_t *out = calloc((size_t)total * 2, sizeof(int16_t));
    if (!out) return 1;

    synth_init(&synth);
    synth_load_preset(&synth, preset);
    for (int i = 0; i < num_notes; i++) synth_note_on(&synth, (uint8_t)notes[i], 100);

    int done = 0;
    int released = 0;
    while (done < total) {
        if (!released && done >= hold) {
            for (int i = 0; i < num_notes; i++) synth_note_off(&synth, (uint8_t)notes[i]);
            released = 1;
        }
        int n = MOVE_FRAMES_PER_BLOCK;
        if (done + n > total) n = total - done;
        if (!released && done + n > hold) n = hold - done; /* land exactly on the note-off */
        if (n <= 0) n = 1;
        synth_render(&synth, out + done * 2, n);
        done += n;
    }

    int ok = write_wav(argv[5], out, total);
    free(out);
    if (!ok) {
        fprintf(stderr, "cannot write %s\n", argv[5]);
        return 1;
    }
    printf("preset %d (%d note%s): %.2fs held of %.2fs -> %s\n", preset, num_notes,
           num_notes == 1 ? "" : "s", hold_s, total_s, argv[5]);
    return 0;
}
