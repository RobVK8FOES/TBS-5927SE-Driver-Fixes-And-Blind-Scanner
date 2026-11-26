// SPDX-License-Identifier: MIT
/*
 * Standalone Linux CLI hardware blind scanner for TBS5927SE (AVL62X1)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/dvb/frontend.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BS_CTRL_PATH   "/proc/avl62x1_bs/bs_ctrl"
#define BS_INFO_PATH   "/proc/avl62x1_bs/bs_info"
#define BS_FEC_S2_BASE 100U

struct scan_pass {
	const char *label;
	enum fe_sec_voltage voltage;
	enum fe_sec_tone_mode tone;
	uint32_t lo_khz;
	bool inverted_lo; /* true for C-band (LO - IF) */
	uint32_t start_if_mhz;
	uint32_t stop_if_mhz;
};

struct bs_result {
	uint32_t rf_khz;
	uint32_t if_khz;
	uint32_t sr_hz;
	uint32_t sys;
	uint32_t mod;
	uint32_t fec;
	uint32_t pilot;
	uint32_t rolloff;
	uint32_t inv;
	int32_t isi;
	uint32_t pls;
	int32_t snr_x100;
	uint32_t stream_type;
	uint32_t ccm_acm;
	uint32_t sis_mis;
	bool vertical;
};

static volatile sig_atomic_t g_abort;

static void on_signal(int sig)
{
	(void)sig;
	g_abort = 1;
}

static int proc_write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, s, strlen(s));
	close(fd);
	return (n < 0) ? -1 : 0;
}

static int proc_read_str(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sz - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

static const char *mod_str(uint32_t m, bool dvbv5)
{
	switch (m) {
	case QPSK:    return "QPSK";
	case PSK_8:   return dvbv5 ? "PSK/8" : "8PSK";
	case APSK_16: return dvbv5 ? "APSK/16" : "16APSK";
	case APSK_32: return dvbv5 ? "APSK/32" : "32APSK";
	default:      return "QPSK";
	}
}

static const char *fec_str(uint32_t f, bool dvbv5)
{
	static const char *const s2x_cr[] = {
		"1/4",    "1/3",    "2/5",    "1/2",    "3/5",
		"2/3",    "3/4",    "4/5",    "5/6",    "8/9",
		"9/10",   "2/9",    "13/45",  "9/20",   "90/180",
		"96/180", "11/20",  "100/180","104/180","26/45",
		"18/30",  "28/45",  "23/36",  "116/180","20/30",
		"124/180","25/36",  "128/180","13/18",  "132/180",
		"22/30",  "135/180","140/180","7/9",    "154/180",
		"11/45",  "4/15",   "14/45",  "7/15",   "8/15",
		"32/45"
	};

	if (f >= BS_FEC_S2_BASE) {
		uint32_t idx = f - BS_FEC_S2_BASE;

		if (dvbv5)
			return "AUTO";
		if (idx < sizeof(s2x_cr) / sizeof(s2x_cr[0]))
			return s2x_cr[idx];
		return "AUTO";
	}

	switch (f) {
	case FEC_1_2:  return "1/2";
	case FEC_2_3:  return "2/3";
	case FEC_3_4:  return "3/4";
	case FEC_4_5:  return "4/5";
	case FEC_5_6:  return "5/6";
	case FEC_6_7:  return "6/7";
	case FEC_7_8:  return "7/8";
	case FEC_8_9:  return "8/9";
	case FEC_3_5:  return "3/5";
	case FEC_9_10: return "9/10";
	case FEC_2_5:  return "2/5";
	default:       return "AUTO";
	}
}

static const char *stream_type_str(uint32_t st)
{
	switch (st) {
	case 0:  return "GS-PKT";
	case 1:  return "GS-CONT";
	case 2:  return "GSE-HEM";
	case 3:  return "TS";
	case 5:  return "GSE-LITE";
	case 6:  return "GSE-HML";
	case 7:  return "T2MI";
	default: return "UNK";
	}
}

static int cmp_result(const void *a, const void *b)
{
	const struct bs_result *ra = a;
	const struct bs_result *rb = b;

	if (ra->rf_khz != rb->rf_khz)
		return (ra->rf_khz < rb->rf_khz) ? -1 : 1;
	if (ra->sr_hz != rb->sr_hz)
		return (ra->sr_hz < rb->sr_hz) ? -1 : 1;
	if (ra->isi != rb->isi)
		return (ra->isi < rb->isi) ? -1 : 1;
	return 0;
}

static bool same_carrier(const struct bs_result *a, const struct bs_result *b)
{
	uint32_t df = (a->rf_khz > b->rf_khz) ? (a->rf_khz - b->rf_khz)
					      : (b->rf_khz - a->rf_khz);
	uint32_t ds = (a->sr_hz > b->sr_hz) ? (a->sr_hz - b->sr_hz)
					    : (b->sr_hz - a->sr_hz);

	return (a->vertical == b->vertical) && (df <= 500) && (ds <= 50000);
}

static void write_conf_entry(FILE *conf_fp, const struct bs_result *r)
{
	if (!conf_fp)
		return;

	if (r->isi >= 0)
		fprintf(conf_fp, "[%u_%c_%u_ISI%d]\n",
			(r->rf_khz + 500) / 1000,
			r->vertical ? 'V' : 'H',
			(r->sr_hz + 500) / 1000,
			r->isi);
	else
		fprintf(conf_fp, "[%u_%c_%u]\n",
			(r->rf_khz + 500) / 1000,
			r->vertical ? 'V' : 'H',
			(r->sr_hz + 500) / 1000);

	fprintf(conf_fp, "\tDELIVERY_SYSTEM = %s\n",
		(r->sys == SYS_DVBS) ? "DVBS" : "DVBS2");
	fprintf(conf_fp, "\tFREQUENCY = %u\n", r->rf_khz);
	fprintf(conf_fp, "\tSYMBOL_RATE = %u\n", r->sr_hz);
	fprintf(conf_fp, "\tPOLARIZATION = %s\n",
		r->vertical ? "VERTICAL" : "HORIZONTAL");
	fprintf(conf_fp, "\tMODULATION = %s\n", mod_str(r->mod, true));
	fprintf(conf_fp, "\tINNER_FEC = %s\n", fec_str(r->fec, true));
	if (r->isi >= 0)
		fprintf(conf_fp, "\tSTREAM_ID = %d\n", r->isi);
	if (r->pls > 0)
		fprintf(conf_fp, "\tSCRAMBLING_SEQUENCE_INDEX = %u\n", r->pls);
	fprintf(conf_fp, "\n");
	fflush(conf_fp);
}

static int setup_lnb(int fe_fd, enum fe_sec_voltage volt,
		     enum fe_sec_tone_mode tone, int diseqc_port)
{
	if (ioctl(fe_fd, FE_SET_TONE, SEC_TONE_OFF) < 0) {
		perror("FE_SET_TONE(OFF)");
		return -1;
	}
	if (ioctl(fe_fd, FE_SET_VOLTAGE, volt) < 0) {
		perror("FE_SET_VOLTAGE");
		return -1;
	}
	usleep(20000);

	if (diseqc_port >= 1 && diseqc_port <= 4) {
		struct dvb_diseqc_master_cmd cmd;
		uint8_t flags = (uint8_t)(((diseqc_port - 1) << 2) |
					  ((volt == SEC_VOLTAGE_18) ? 2 : 0) |
					  ((tone == SEC_TONE_ON) ? 1 : 0));

		cmd.msg_len = 4;
		cmd.msg[0] = 0xe0;
		cmd.msg[1] = 0x10;
		cmd.msg[2] = 0x38;
		cmd.msg[3] = 0xf0 | flags;

		if (ioctl(fe_fd, FE_DISEQC_SEND_MASTER_CMD, &cmd) < 0) {
			perror("FE_DISEQC_SEND_MASTER_CMD");
			return -1;
		}
		usleep(20000);
		ioctl(fe_fd, FE_DISEQC_SEND_BURST,
		      ((diseqc_port - 1) & 1) ? SEC_MINI_B : SEC_MINI_A);
		usleep(20000);
	}

	if (ioctl(fe_fd, FE_SET_TONE, tone) < 0) {
		perror("FE_SET_TONE");
		return -1;
	}
	usleep(50000);
	return 0;
}

static int run_pass(int fe_fd, const struct scan_pass *pass, int diseqc_port,
		    uint32_t min_sr_khz, uint32_t max_sr_khz,
		    bool expand_mis, FILE *conf_fp)
{
	char buf[256];
	int running = 1, count = 0, progress = 0;
	struct bs_result *res = NULL;
	int nres = 0, i;

	fprintf(stderr, "\n=== Scanning %s (IF %u-%u MHz, LO %.1f MHz, SR %u-%u kS/s) ===\n",
		pass->label, pass->start_if_mhz, pass->stop_if_mhz,
		pass->lo_khz / 1000.0, min_sr_khz, max_sr_khz);

	if (setup_lnb(fe_fd, pass->voltage, pass->tone, diseqc_port) < 0)
		return -1;

	snprintf(buf, sizeof(buf), "1 %u %u %u %u\n",
		 pass->start_if_mhz, pass->stop_if_mhz, min_sr_khz, max_sr_khz);
	if (proc_write_str(BS_CTRL_PATH, buf) < 0) {
		perror("write " BS_CTRL_PATH);
		return -1;
	}

	while (!g_abort) {
		usleep(200000);
		if (proc_read_str(BS_CTRL_PATH, buf, sizeof(buf)) < 0) {
			perror("read " BS_CTRL_PATH);
			return -1;
		}
		if (sscanf(buf, "%d %d %d", &running, &count, &progress) == 3) {
			fprintf(stderr, "\r  Progress: %3d%%  Entries found: %d   ",
				progress, count);
			fflush(stderr);
			if (!running)
				break;
		}
	}
	fprintf(stderr, "\n");

	if (g_abort) {
		proc_write_str(BS_CTRL_PATH, "0 0 0 0 0\n");
		return 0;
	}

	if (count <= 0)
		return 0;

	res = calloc((size_t)count, sizeof(*res));
	if (!res)
		return -1;

	for (i = 0; i < count && !g_abort; i++) {
		struct bs_result *r = &res[nres];
		int idx;

		snprintf(buf, sizeof(buf), "%d\n", i);
		if (proc_write_str(BS_INFO_PATH, buf) < 0)
			continue;
		if (proc_read_str(BS_INFO_PATH, buf, sizeof(buf)) <= 0)
			continue;

		if (sscanf(buf, "%d %u %u %u %u %u %u %u %u %d %u %d %u %u %u",
			   &idx, &r->if_khz, &r->sr_hz, &r->sys, &r->mod, &r->fec,
			   &r->pilot, &r->rolloff, &r->inv, &r->isi, &r->pls,
			   &r->snr_x100, &r->stream_type, &r->ccm_acm, &r->sis_mis) != 15)
			continue;

		r->vertical = (pass->voltage == SEC_VOLTAGE_13);
		r->rf_khz = pass->inverted_lo ?
			    (pass->lo_khz > r->if_khz ? pass->lo_khz - r->if_khz : 0) :
			    (pass->lo_khz + r->if_khz);
		nres++;
	}

	qsort(res, (size_t)nres, sizeof(*res), cmp_result);

	for (i = 0; i < nres && !g_abort; ) {
		int end = i + 1;
		char isi_buf[32];

		if (!expand_mis) {
			while (end < nres && same_carrier(&res[i], &res[end]))
				end++;
		}

		if (end - i == 1) {
			if (res[i].isi < 0)
				snprintf(isi_buf, sizeof(isi_buf), "-");
			else
				snprintf(isi_buf, sizeof(isi_buf), "%d", res[i].isi);
		} else if (end - i <= 4) {
			int k, pos = 0;

			for (k = i; k < end; k++)
				pos += snprintf(isi_buf + pos,
						sizeof(isi_buf) - (size_t)pos,
						"%s%d", (k > i) ? "," : "",
						res[k].isi);
		} else {
			snprintf(isi_buf, sizeof(isi_buf), "%d..%d(%d)",
				 res[i].isi, res[end - 1].isi, end - i);
		}

		printf("%9.3f  %c  %9.3f  %-5s  %-6s  %-7s  %-3s  %-3s  %-8s  %-13s  %6.2f\n",
		       res[i].rf_khz / 1000.0,
		       res[i].vertical ? 'V' : 'H',
		       res[i].sr_hz / 1000.0,
		       (res[i].sys == SYS_DVBS) ? "DVBS" : "DVBS2",
		       mod_str(res[i].mod, false),
		       fec_str(res[i].fec, false),
		       (res[i].pilot == PILOT_ON) ? "ON" : "OFF",
		       res[i].ccm_acm ? "ACM" : "CCM",
		       stream_type_str(res[i].stream_type),
		       isi_buf,
		       res[i].snr_x100 / 100.0);
		fflush(stdout);

		if (conf_fp) {
			if (expand_mis || (end - i) <= 4) {
				int k;

				for (k = i; k < end; k++)
					write_conf_entry(conf_fp, &res[k]);
			} else {
				fprintf(conf_fp, "# %d ISIs detected (%d..%d); run with -m to export all\n",
					end - i, res[i].isi, res[end - 1].isi);
				write_conf_entry(conf_fp, &res[i]);
			}
		}

		i = end;
	}

	free(res);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -a <num>       DVB adapter index (default: 0)\n"
		"  -f <num>       DVB frontend index (default: 0)\n"
		"  -b <band>      low | high | full | c (default: full, Universal Ku)\n"
		"  -l <mhz>       Custom LNB LO in MHz (e.g. 10700 for single-LO Ku)\n"
		"  -i             Inverted LO (RF = LO - IF, for C-band custom LO)\n"
		"  -t <on|off>    22 kHz tone override for custom LO (default: off)\n"
		"  -p <v|h|vh>    Polarization to scan (default: vh)\n"
		"  -d <1..4>      DiSEqC 1.0 switch port A..D (default: none)\n"
		"  -s <mhz>       Start IF frequency in MHz (850..2300)\n"
		"  -e <mhz>       Stop IF frequency in MHz (850..2300)\n"
		"  -n <khz>       Min symbol rate in kS/s (default: 500, min: 200)\n"
		"  -x <khz>       Max symbol rate in kS/s (default: 60000)\n"
		"  -m             Expand all MIS ISIs onto separate lines\n"
		"  -o <file>      Export dvbv5-zap channels.conf file\n"
		"  -h             Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	int adapter = 0, frontend = 0, diseqc_port = 0;
	const char *band = "full";
	const char *pol = "vh";
	const char *out_path = NULL;
	uint32_t custom_lo_mhz = 0;
	bool inverted_lo = false;
	bool expand_mis = false;
	enum fe_sec_tone_mode custom_tone = SEC_TONE_OFF;
	uint32_t start_if = 0, stop_if = 0;
	uint32_t min_sr_khz = 500, max_sr_khz = 60000;
	struct scan_pass passes[4];
	int num_passes = 0;
	char fe_dev[64];
	int fe_fd, opt, p;
	FILE *conf_fp = NULL;

	while ((opt = getopt(argc, argv, "a:f:b:l:it:p:d:s:e:n:x:mo:h")) != -1) {
		switch (opt) {
		case 'a': adapter = atoi(optarg); break;
		case 'f': frontend = atoi(optarg); break;
		case 'b': band = optarg; break;
		case 'l': custom_lo_mhz = (uint32_t)atoi(optarg); break;
		case 'i': inverted_lo = true; break;
		case 't':
			custom_tone = (strcmp(optarg, "on") == 0) ?
				      SEC_TONE_ON : SEC_TONE_OFF;
			break;
		case 'p': pol = optarg; break;
		case 'd': diseqc_port = atoi(optarg); break;
		case 's': start_if = (uint32_t)atoi(optarg); break;
		case 'e': stop_if = (uint32_t)atoi(optarg); break;
		case 'n': min_sr_khz = (uint32_t)atoi(optarg); break;
		case 'x': max_sr_khz = (uint32_t)atoi(optarg); break;
		case 'm': expand_mis = true; break;
		case 'o': out_path = optarg; break;
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (access(BS_CTRL_PATH, R_OK | W_OK) != 0) {
		fprintf(stderr, "Error: %s not accessible (is patched avl62x1 loaded?)\n",
			BS_CTRL_PATH);
		return 1;
	}

	bool do_v = (strchr(pol, 'v') || strchr(pol, 'V'));
	bool do_h = (strchr(pol, 'h') || strchr(pol, 'H'));
	if (!do_v && !do_h) {
		fprintf(stderr, "Invalid polarization '%s' (use v, h, or vh)\n", pol);
		return 1;
	}

	if (custom_lo_mhz > 0) {
		uint32_t s_if = start_if ? start_if : 950;
		uint32_t e_if = stop_if ? stop_if : 2150;
		if (do_v)
			passes[num_passes++] = (struct scan_pass){
				"Custom-V", SEC_VOLTAGE_13, custom_tone,
				custom_lo_mhz * 1000U, inverted_lo, s_if, e_if
			};
		if (do_h)
			passes[num_passes++] = (struct scan_pass){
				"Custom-H", SEC_VOLTAGE_18, custom_tone,
				custom_lo_mhz * 1000U, inverted_lo, s_if, e_if
			};
	} else if (strcmp(band, "c") == 0) {
		uint32_t s_if = start_if ? start_if : 950;
		uint32_t e_if = stop_if ? stop_if : 1750;
		if (do_v)
			passes[num_passes++] = (struct scan_pass){
				"C-Band-V", SEC_VOLTAGE_13, SEC_TONE_OFF,
				5150000U, true, s_if, e_if
			};
		if (do_h)
			passes[num_passes++] = (struct scan_pass){
				"C-Band-H", SEC_VOLTAGE_18, SEC_TONE_OFF,
				5150000U, true, s_if, e_if
			};
	} else {
		bool do_low = (strcmp(band, "low") == 0 || strcmp(band, "full") == 0);
		bool do_high = (strcmp(band, "high") == 0 || strcmp(band, "full") == 0);
		if (!do_low && !do_high) {
			fprintf(stderr, "Invalid band '%s'\n", band);
			return 1;
		}
		if (do_low) {
			uint32_t s_if = start_if ? start_if : 950;
			uint32_t e_if = stop_if ? stop_if : 1950;
			if (do_v)
				passes[num_passes++] = (struct scan_pass){
					"Ku-Low-V", SEC_VOLTAGE_13, SEC_TONE_OFF,
					9750000U, false, s_if, e_if
				};
			if (do_h)
				passes[num_passes++] = (struct scan_pass){
					"Ku-Low-H", SEC_VOLTAGE_18, SEC_TONE_OFF,
					9750000U, false, s_if, e_if
				};
		}
		if (do_high) {
			uint32_t s_if = start_if ? start_if : 1100;
			uint32_t e_if = stop_if ? stop_if : 2150;
			if (do_v)
				passes[num_passes++] = (struct scan_pass){
					"Ku-High-V", SEC_VOLTAGE_13, SEC_TONE_ON,
					10600000U, false, s_if, e_if
				};
			if (do_h)
				passes[num_passes++] = (struct scan_pass){
					"Ku-High-H", SEC_VOLTAGE_18, SEC_TONE_ON,
					10600000U, false, s_if, e_if
				};
		}
	}

	snprintf(fe_dev, sizeof(fe_dev), "/dev/dvb/adapter%d/frontend%d",
		 adapter, frontend);
	fe_fd = open(fe_dev, O_RDWR);
	if (fe_fd < 0) {
		perror(fe_dev);
		return 1;
	}

	if (out_path) {
		conf_fp = fopen(out_path, "w");
		if (!conf_fp) {
			perror(out_path);
			close(fe_fd);
			return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	printf("%9s  %s  %9s  %-5s  %-6s  %-7s  %-3s  %-3s  %-8s  %-13s  %6s\n",
	       "RF(MHz)", "P", "SR(kS/s)", "Std", "Mod", "FEC",
	       "Plt", "ACM", "Stream", "ISI", "SNR");
	printf("----------------------------------------------------------------------------------------------\n");

	for (p = 0; p < num_passes && !g_abort; p++)
		run_pass(fe_fd, &passes[p], diseqc_port, min_sr_khz, max_sr_khz,
			 expand_mis, conf_fp);

	ioctl(fe_fd, FE_SET_TONE, SEC_TONE_OFF);
	ioctl(fe_fd, FE_SET_VOLTAGE, SEC_VOLTAGE_OFF);
	if (conf_fp)
		fclose(conf_fp);
	close(fe_fd);
	return 0;
}
