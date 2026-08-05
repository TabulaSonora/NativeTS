/*
 * Renders one note from an exported SoundFont, for A/B against the engine.
 *
 * The other half of tools/compare_soundfont.py. It links spessasynth_core, which is NOT a
 * dependency of this project -- the harness compiles it on demand and the build does not know it
 * exists. That is deliberate: the exporter must not acquire a build dependency on the reader it
 * happens to target.
 *
 * Output is raw interleaved float32 stereo, the same shape `tabula-sonora render-note` writes, so
 * the two can be compared directly.
 *
 *   cc -O2 -I <spessasynth>/include tools/sf2_note_render.c \
 *      -L <spessasynth>/build -lspessasynth -Wl,-rpath,<spessasynth>/build -lm \
 *      -o sf2_note_render
 *
 *   sf2_note_render <bank.sf2> <out.f32> --program N --note N --velocity N --hold S
 *                   [--bank-msb N] [--bank-lsb N] [--drum] [--sflist list.json]
 *                   [--rate HZ] [--tail S]
 */

#include "spessasynth/sflist/sflist.h"
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/synthesizer/synth.h"
#include "spessasynth/utils/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The reader asks to be driven in whole chunks; finer timing goes through the event timestamps. */
#define CHUNK 128

static char *read_all(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if(!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(n < 0) { fclose(f); return NULL; }
	char *buf = malloc((size_t)n + 1);
	if(!buf) { fclose(f); return NULL; }
	if(n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	buf[n] = 0;
	fclose(f);
	*size = (size_t)n;
	return buf;
}

int main(int argc, char **argv) {
	if(argc < 3) {
		fprintf(stderr, "usage: sf2_note_render <bank.sf2> <out.f32> [options]\n");
		return 2;
	}
	const char *bank_path = argv[1];
	const char *out_path = argv[2];

	int program = 0, note = 60, velocity = 100;
	int bank_msb = 0, bank_lsb = 0, drum = 0;
	double hold = 1.0, tail = 1.8;
	unsigned rate = 32000;
	const char *sflist_path = NULL;
	const char *base_path = ".";

	for(int i = 3; i < argc; i++) {
		if(!strcmp(argv[i], "--program") && i + 1 < argc) program = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--note") && i + 1 < argc) note = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--velocity") && i + 1 < argc) velocity = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--bank-msb") && i + 1 < argc) bank_msb = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--bank-lsb") && i + 1 < argc) bank_lsb = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atof(argv[++i]);
		else if(!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
		else if(!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
		else if(!strcmp(argv[i], "--sflist") && i + 1 < argc) sflist_path = argv[++i];
		else if(!strcmp(argv[i], "--base") && i + 1 < argc) base_path = argv[++i];
		else if(!strcmp(argv[i], "--drum")) drum = 1;
		else { fprintf(stderr, "unknown option '%s'\n", argv[i]); return 2; }
	}

	SS_Processor *proc = ss_processor_create(rate, NULL);
	if(!proc) { fprintf(stderr, "cannot create processor\n"); return 1; }

	/* An sflist is the whole point on a remapped bank: without it the ROM-aligned slots are what
	 * the program change addresses, which is not what a vintage map says. */
	if(sflist_path) {
		size_t n = 0;
		char *text = read_all(sflist_path, &n);
		if(!text) { fprintf(stderr, "cannot read '%s'\n", sflist_path); return 1; }
		char error[sflist_max_error];
		error[0] = 0;
		SS_FilteredBanks *fb = sflist_load(text, n, base_path, error);
		free(text);
		if(!fb) { fprintf(stderr, "sflist: %s\n", error[0] ? error : "(no message)"); return 1; }
		if(!ss_processor_load_filtered_banks(proc, fb, "bank", true)) {
			fprintf(stderr, "cannot register filtered banks\n");
			return 1;
		}
	} else {
		SS_File *f = ss_file_open_from_file(bank_path);
		if(!f) { fprintf(stderr, "cannot open '%s'\n", bank_path); return 1; }
		SS_SoundBank *bank = ss_soundbank_load(f);
		ss_file_close(f);
		if(!bank) { fprintf(stderr, "cannot load '%s'\n", bank_path); return 1; }
		if(!ss_processor_load_soundbank(proc, bank, "bank", 0, true)) {
			fprintf(stderr, "cannot register bank\n");
			return 1;
		}
	}

	const int channel = drum ? 9 : 0;
	ss_processor_control_change(proc, channel, 0, bank_msb, 0.0);
	ss_processor_control_change(proc, channel, 32, bank_lsb, 0.0);
	ss_processor_program_change(proc, channel, program, 0.0);

	/* Report what the program change actually resolved to, so a silent render is distinguishable
	 * from a wrong one. */
	SS_BasicPreset *preset = ss_processor_resolve_preset(
	proc, channel, (uint8_t)program, (uint16_t)bank_msb, (uint16_t)bank_lsb, drum != 0);
	fprintf(stderr, "preset: %s\n", preset ? preset->name : "(unresolved)");

	ss_processor_note_on(proc, channel, note, velocity, 0.0);

	const uint32_t total = (uint32_t)((hold + tail) * rate);
	const uint32_t note_off_at = (uint32_t)(hold * rate);

	float *out = malloc((size_t)total * 2 * sizeof(float));
	if(!out) { fprintf(stderr, "out of memory\n"); return 1; }

	int released = 0;
	for(uint32_t done = 0; done < total; done += CHUNK) {
		uint32_t n = total - done < CHUNK ? total - done : CHUNK;
		if(!released && done + n > note_off_at) {
			ss_processor_note_off(proc, channel, note, (double)note_off_at / rate);
			released = 1;
		}
		ss_processor_render_interleaved(proc, out + (size_t)done * 2, n);
	}

	FILE *f = fopen(out_path, "wb");
	if(!f) { fprintf(stderr, "cannot write '%s'\n", out_path); return 1; }
	fwrite(out, sizeof(float), (size_t)total * 2, f);
	fclose(f);

	fprintf(stderr, "%u frames at %u Hz\n", total, rate);
	free(out);
	ss_processor_free(proc);
	return 0;
}
