/* $Id: oprofiled.c,v 1.65 2002/01/14 07:02:02 movement Exp $ */
/* COPYRIGHT (C) 2000 THE VICTORIA UNIVERSITY OF MANCHESTER and John Levon
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "oprofiled.h"
#include "opd_proc.h"
#include "../util/op_popt.h"

uint op_nr_counters = 2;
int verbose;
op_cpu cpu_type;
int separate_samples;
char *smpdir="/var/opd/samples/";
char *vmlinux;
int kernel_only;
unsigned long opd_stats[OPD_MAX_STATS] = { 0, };

 
static int showvers;
static u32 ctr_enabled[OP_MAX_COUNTERS];
/* Unfortunately popt does not have, on many versions, the POPT_ARG_DOUBLE type
 * so I must first store it as a string. */
static const char *cpu_speed_str;
static int ignore_myself;
static int opd_buf_size=OP_DEFAULT_BUF_SIZE;
static int opd_note_buf_size=OP_DEFAULT_NOTE_SIZE;
static char *opd_dir="/var/opd/";
static char *logfilename="oprofiled.log";
static char *devfilename="opdev";
static char *notedevfilename="opnotedev";
static char *devhashmapfilename="ophashmapdev";
static char *systemmapfilename;
static pid_t mypid;
static sigset_t maskset;
static fd_t devfd;
static fd_t notedevfd;

static void opd_sighup(int val);
static void opd_open_logfile(void);

static struct poptOption options[] = {
	{ "ignore-myself", 'm', POPT_ARG_INT, &ignore_myself, 0, "ignore samples of oprofile driver", "[0|1]"},
	{ "log-file", 'l', POPT_ARG_STRING, &logfilename, 0, "log file", "file", },
	{ "base-dir", 'd', POPT_ARG_STRING, &opd_dir, 0, "base directory of daemon", "dir", },
	{ "samples-dir", 's', POPT_ARG_STRING, &smpdir, 0, "output samples dir", "file", },
	{ "device-file", 'd', POPT_ARG_STRING, &devfilename, 0, "profile device file", "file", },
	{ "note-device-file", 'p', POPT_ARG_STRING, &notedevfilename, 0, "note device file", "file", },
	{ "hash-map-device-file", 'h', POPT_ARG_STRING, &devhashmapfilename, 0, "profile hashmap device file", "file", },
	{ "map-file", 'f', POPT_ARG_STRING, &systemmapfilename, 0, "System.map for running kernel file", "file", },
	{ "vmlinux", 'k', POPT_ARG_STRING, &vmlinux, 0, "vmlinux kernel image", "file", },
	{ "cpu-speed", 0, POPT_ARG_STRING, &cpu_speed_str, 0, "cpu speed (MHz)", "cpu_mhz", },
	{ "separate-samples", 0, POPT_ARG_INT, &separate_samples, 0, "separate samples for each distinct application", "[0|1]", },
	{ "version", 'v', POPT_ARG_NONE, &showvers, 0, "show version", NULL, },
	{ "verbose", 'V', POPT_ARG_NONE, &verbose, 0, "be verbose in log file", NULL, },
	POPT_AUTOHELP
	{ NULL, 0, 0, NULL, 0, NULL, NULL, },
};

/**
 * opd_open_logfile - open the log file
 *
 * Open the logfile on stdout and stderr. This function
 * assumes that 1 and 2 are the lowest close()d file
 * descriptors. Failure to open on either descriptor is
 * a fatal error.
 */
static void opd_open_logfile(void)
{
	if (open(logfilename, O_WRONLY|O_CREAT|O_NOCTTY|O_APPEND, 0755) == -1) {
		perror("oprofiled: couldn't re-open stdout: ");
		exit(1);
	}

	if (dup2(1,2) == -1) {
		perror("oprofiled: couldn't dup stdout to stderr: ");
		exit(1);
	}
}

/**
 * opd_open_files - open necessary files
 *
 * Open the device files and the log file,
 * and mmap() the hash map. Also read the System.map
 * file.
 */
static void opd_open_files(void)
{
	fd_t hashmapdevfd;

	hashmapdevfd = opd_open_device(devhashmapfilename, 0);
	if (hashmapdevfd == -1) {
		perror("Failed to open hash map device: ");
		exit(1);
	}
 
	notedevfd = opd_open_device(notedevfilename, 0);
	if (notedevfd == -1) {
		if (errno == EINVAL)
			fprintf(stderr, "Failed to open note device. Possibly you have passed incorrect\n"
				"parameters. Check /var/log/messages.");
		else
			perror("Failed to open note device: ");
		exit(1);
	}
 
	devfd = opd_open_device(devfilename, 0);
	if (devfd == -1) {
		if (errno == EINVAL)
			fprintf(stderr, "Failed to open device. Possibly you have passed incorrect\n"
				"parameters. Check /var/log/messages.");
		else
			perror("Failed to open profile device: ");
		exit(1);
	} 
 
	hashmap = mmap(0, OP_HASH_MAP_SIZE, PROT_READ, MAP_SHARED, hashmapdevfd, 0);
	if ((long)hashmap == -1) {
		perror("oprofiled: couldn't mmap hash map: ");
		exit(1);
	}

	/* give output before re-opening stdout as the logfile */
	printf("Using log file \"%s\"\n", logfilename);
 
	/* set up logfile */
	close(0);
	close(1);

	if (open("/dev/null",O_RDONLY) == -1) {
		perror("oprofiled: couldn't re-open stdin as /dev/null: ");
		exit(1);
	}

	opd_open_logfile();

	opd_read_system_map(systemmapfilename);
	printf("oprofiled started %s", opd_get_time());
	fflush(stdout);
}

/**
 * opd_backup_samples_files - back up all the samples file
 *
 * move all files in dir @smpdir to directory
 * @smpdir/session-#nr
 */
static void opd_backup_samples_files(void)
{
	char * dir_name;
	int gen = 0;
	struct stat stat_buf;
	DIR *dir;
	struct dirent *dirent;

	dir_name = xmalloc(strlen(smpdir) + strlen("session-") + 10);
	strcpy(dir_name, smpdir);

	do {
		sprintf(dir_name + strlen(smpdir), "/session-%d", ++gen);
	} while (stat(dir_name, &stat_buf) == 0);

	if (mkdir(dir_name, 0755)) {
		/* That's a severe problem: if we continue we can overwrite
		 * samples files and produce wrong result. FIXME */
		printf("unable to create directory %s\n", dir_name);
		exit(1);
	}

	if (!(dir = opendir(smpdir))) {
		printf("unable to open directory %s\n", smpdir);
		exit(1);
	}

	printf("Backing up samples file to directory %s\n", dir_name);

	while ((dirent = readdir(dir)) != 0) {
		if (opd_move_regular_file(dir_name, smpdir, dirent->d_name)) {
			printf("unable to backup %s/%s to directory %s\n",
			       smpdir, dirent->d_name, dir_name);
		}
	}

	closedir(dir);

	free(dir_name);
}

/**
 * opd_need_backup_samples_files - test if we need to 
 * backup samples files
 *
 * We can't backup lazily samples files else it can
 * leads to detect that backup is needed after some
 * samples has been written (e.g. ctr 1 have the same
 * setting from the previous runs, ctr 0 have different
 * setting and the first samples output come from ctr1)
 *
 */
static int opd_need_backup_samples_files(void)
{
	DIR * dir;
	struct dirent * dirent;
	struct stat stat_buf;
	int need_backup;
	/* bitmaps: bit i is on if counter i is enabled */
	int counter_set, old_counter_set;
	uint i;

	if (!(dir = opendir(smpdir))) {
		printf("unable to open directory %s\n", smpdir);

		exit(1);
	}

	counter_set = old_counter_set = 0;
	need_backup = 0;

	while ((dirent = readdir(dir)) != 0 && need_backup == 0) {
		char * file = xmalloc(strlen(smpdir) + strlen(dirent->d_name) + 2);
		strcpy(file, smpdir);
		strcat(file, "/");
		strcat(file, dirent->d_name);
		if (!stat(file, &stat_buf) && S_ISREG(stat_buf.st_mode)) {
			struct opd_header header;
			FILE * fp = fopen(file, "r"); 
			if (!fp)
				continue;

			if (fread(&header, sizeof( header), 1, fp) != 1)
				goto close;

			if (memcmp(&header.magic, OPD_MAGIC, sizeof(header.magic)) || header.version != OPD_VERSION)
				goto close;

			if (header.ctr_event != ctr_event[header.ctr] ||
			    header.ctr_um != ctr_um[header.ctr] ||
			    header.ctr_count != ctr_count[header.ctr] ||
			    header.cpu_type != (u32)cpu_type ||
			    header.separate_samples != separate_samples) {
				need_backup = 1;
			}

			old_counter_set |= 1 << header.ctr;

		close:
			fclose(fp);
		}

		free(file);
	}

	for (i = 0 ; i < op_nr_counters; ++i) {
		if (ctr_enabled[i])
			counter_set |= 1 << i;
	}

	/* old_counter_set == 0 means there is no samples file in the sample
	 * dir, so avoid to try to backup else we get an empty backup dir */
	if (old_counter_set && old_counter_set != counter_set)
		need_backup = 1;

	closedir(dir);

	return need_backup;
}

/**
 * opd_pmc_options - read sysctls for pmc options
 */
static void opd_pmc_options(void)
{
	int ret;
	uint i;
	/* should be sufficient to hold /proc/sys/dev/oprofile/%d/yyyy */
	char filename[PATH_MAX + 1];
 
	for (i = 0 ; i < op_nr_counters ; ++i) {
		sprintf(filename, "/proc/sys/dev/oprofile/%d/event", i);
		ctr_event[i]= opd_read_int_from_file(filename);

		sprintf(filename, "/proc/sys/dev/oprofile/%d/count", i);
		ctr_count[i]= opd_read_int_from_file(filename);

		sprintf(filename, "/proc/sys/dev/oprofile/%d/unit_mask", i);
		ctr_um[i]= opd_read_int_from_file(filename);

		sprintf(filename, "/proc/sys/dev/oprofile/%d/enabled", i);
		ctr_enabled[i]= opd_read_int_from_file(filename);
	}

	for (i = 0 ; i < op_nr_counters ; ++i) {
		ret = op_check_events(i, ctr_event[i], ctr_um[i], cpu_type);

		if (ret & OP_EVT_NOT_FOUND)
			fprintf(stderr, "oprofiled: ctr%d: %d: no such event for cpu %s\n",
				i, ctr_event[i], op_get_cpu_type_str(cpu_type));

		if (ret & OP_EVT_NO_UM) 
			fprintf(stderr, "oprofiled: ctr%d: 0x%.2x: invalid unit mask for cpu %s\n",
				i, ctr_um[i], op_get_cpu_type_str(cpu_type));

		if (ret & OP_EVT_CTR_NOT_ALLOWED)
			fprintf(stderr, "oprofiled: ctr%d: %d: can't count event for this counter\n",
				i, ctr_count[i]);

		if (ret != OP_EVENTS_OK)
			exit(1);
	}
} 
 
/**
 * opd_options - parse command line options
 * @argc: argc
 * @argv: argv array
 *
 * Parse all command line arguments, and sanity
 * check what the user passed. Incorrect arguments
 * are a fatal error.
 */
static void opd_options(int argc, char const *argv[])
{
	poptContext optcon;

	optcon = opd_poptGetContext(NULL, argc, argv, options, 0);

	if (showvers) {
		printf(VERSION_STRING " compiled on " __DATE__ " " __TIME__ "\n");
		exit(0);
	}

	if (!vmlinux || streq("", vmlinux)) {
		fprintf(stderr, "oprofiled: no vmlinux specified.\n");
		poptPrintHelp(optcon, stderr, 0);
		exit(1);
	}

	if (!systemmapfilename || streq("", systemmapfilename)) {
		fprintf(stderr, "oprofiled: no System.map specified.\n");
		poptPrintHelp(optcon, stderr, 0);
		exit(1);
	}

	opd_buf_size = opd_read_int_from_file("/proc/sys/dev/oprofile/bufsize");
	opd_note_buf_size = opd_read_int_from_file("/proc/sys/dev/oprofile/notesize");
	kernel_only = opd_read_int_from_file("/proc/sys/dev/oprofile/kernel_only");

	if (cpu_type != CPU_RTC) {
		opd_pmc_options();
	}

	if (cpu_speed_str && strlen(cpu_speed_str))
		sscanf(cpu_speed_str, "%lf", &cpu_speed);

	poptFreeContext(optcon);
}

/**
 * opd_fork - fork and return as child
 *
 * fork() and exit the parent with _exit().
 * Failure is fatal.
 */
static void opd_fork(void)
{
	switch (fork()) {
		case -1:
			perror("oprofiled: fork() failed: ");
			exit(1);
			break;
		case 0:
			break;
		default:
			/* parent */
			_exit(0);
			break;
	}
}

/**
 * opd_go_daemon - become daemon process
 *
 * Become an un-attached daemon in the standard
 * way (fork(),chdir(),setsid(),fork()). Sets
 * the global variable mypid to the pid of the second
 * child. Parents perform _exit().
 *
 * Any failure is fatal.
 */
static void opd_go_daemon(void)
{
	opd_fork();

	if (chdir(opd_dir)) {
		fprintf(stderr,"oprofiled: opd_go_daemon: couldn't chdir to %s: %s", opd_dir, strerror(errno));
		exit(1);
	}

	if (setsid() < 0) {
		perror("oprofiled: opd_go_daemon: couldn't setsid: ");
		exit(1);
	}

	opd_fork();
	mypid = getpid();
}

void opd_do_samples(const struct op_sample *opd_buf, size_t count);
void opd_do_notes(struct op_note *opd_buf, size_t count);

/**
 * do_shutdown - shutdown cleanly, reading as much remaining data as possible.
 * @buf: sample buffer area
 * @size: size of sample buffer
 * @nbuf: note buffer area
 * @nsize: size of note buffer
 */
static void opd_shutdown(struct op_sample *buf, size_t size, struct op_note *nbuf, size_t nsize)
{
	ssize_t count = -1;
	ssize_t ncount = -1;
 
	/* the dump may have added no samples, so we must set
	 * non-blocking */
	if (fcntl(devfd, F_SETFL, fcntl(devfd, F_GETFL) | O_NONBLOCK) < 0) {
		perror("Failed to set non-blocking read for device: ");
		exit(1);
	}

	/* it's always OK to read the note device */
	while (ncount < 0) {
		ncount = opd_read_device(notedevfd, nbuf, nsize, TRUE);
	}
 
	if (ncount > 0) {
		opd_do_notes(nbuf, ncount);
	}
 
	/* read as much as we can until we have exhausted the data
	 * (EAGAIN is returned).
	 *
	 * This will not livelock as the profiler has been partially
	 * shut down by now. Multiple stops from user-space can cause
	 * more reads of 0 size, so we cater for that. Technically this
	 * would be a livelock, but it requires root to send stops constantly
	 * and never lose the race. */
	while (1) {
		count = opd_read_device(devfd, buf, size, TRUE);
		if (count < 0 && errno == EAGAIN) {
			break;
		} else if (count > 0) {
			opd_do_samples(buf, count);
		}
	}
}

/**
 * opd_do_read - enter processing loop
 * @buf: buffer to read into
 * @size: size of buffer
 * @nbuf: note buffer
 * @nsize: size of note buffer
 *
 * Read some of a buffer from the device and process
 * the contents.
 *
 * Never returns.
 */
static void opd_do_read(struct op_sample *buf, size_t size, struct op_note *nbuf, size_t nsize)
{
	while (1) {
		ssize_t count = -1;
		ssize_t ncount = -1;
 
		/* loop to handle EINTR */
		while (count < 0) {
			count = opd_read_device(devfd, buf, size, TRUE);
 
			/* if count == 0, that means we need to stop ! */
			if (count == 0) {
				opd_shutdown(buf, size, nbuf, nsize);
				return;
			}
		}

		while (ncount < 0) {
			ncount = opd_read_device(notedevfd, nbuf, nsize, TRUE);
		}
 
		opd_do_notes(nbuf, ncount);
		opd_do_samples(buf, count);
	}
}

/**
 * opd_do_notes - process a notes buffer
 * @opd_buf: buffer to process
 * @count: number of bytes in buffer
 *
 * Process a buffer of notes.
 */
void opd_do_notes(struct op_note *opd_buf, size_t count)
{
	uint i; 
	struct op_note * note;
 
	/* prevent signals from messing us up */
	sigprocmask(SIG_BLOCK, &maskset, NULL);

	for (i = 0; i < count/sizeof(struct op_note); i++) {
		note = &opd_buf[i];
		if (ignore_myself && note->pid == mypid)
			continue;
 
		opd_stats[OPD_NOTIFICATIONS]++;

		switch (note->type) {
			case OP_MAP:
			case OP_EXEC: 
				if (note->type == OP_EXEC)
					opd_handle_exec(note->pid);
				opd_handle_mapping(note);
				break;

			case OP_FORK:
				opd_handle_fork(note);
				break;

			case OP_DROP_MODULES:
				opd_clear_module_info();
				break;

			case OP_EXIT:
				opd_handle_exit(note);
				break;

			default:
				fprintf(stderr, "Received unknown notification type %u\n", note->type);
				exit(1);
				break;
		}
	}
	sigprocmask(SIG_UNBLOCK, &maskset, NULL);
}

/**
 * opd_do_samples - process a sample buffer
 * @opd_buf: buffer to process
 * @count: number of bytes in buffer 
 *
 * Process a buffer of samples.
 * The signals specified by the global variable maskset are
 * masked. Samples for oprofiled are ignored if the global
 * variable ignore_myself is set.
 *
 * If the sample could be processed correctly, it is written
 * to the relevant sample file. Additionally mapping and
 * process notifications are handled here.
 */
void opd_do_samples(const struct op_sample *opd_buf, size_t count)
{
	uint i;

	/* prevent signals from messing us up */
	sigprocmask(SIG_BLOCK, &maskset, NULL);

	opd_stats[OPD_DUMP_COUNT]++;

	for (i=0; i < count/sizeof(struct op_sample); i++) {
		verbprintf("%.6u: EIP: 0x%.8x pid: %.6d count: %.6d\n", i, opd_buf[i].eip, opd_buf[i].pid, opd_buf[i].count);

		if (ignore_myself && opd_buf[i].pid == mypid)
			continue;

		opd_put_sample(&opd_buf[i]);
	}

	sigprocmask(SIG_UNBLOCK, &maskset, NULL);
}

/* re-open logfile for logrotate */
static void opd_sighup(int val __attribute__((unused)))
{
	close(1);
	close(2);
	opd_open_logfile();
}

int main(int argc, char const *argv[])
{
	struct op_sample *sbuf;
	size_t s_buf_bytesize;
	struct op_note *nbuf;
	size_t n_buf_bytesize;
	struct sigaction act;
	int i;

	cpu_type = op_get_cpu_type();
	if (cpu_type == CPU_ATHLON)
		op_nr_counters = 4;

	opd_options(argc, argv);

	s_buf_bytesize = opd_buf_size * sizeof(struct op_sample);

 	sbuf = xmalloc(s_buf_bytesize);

	n_buf_bytesize = opd_note_buf_size * sizeof(struct op_note);
	nbuf = xmalloc(n_buf_bytesize);
 
	opd_init_images();

	opd_go_daemon();

	if (opd_need_backup_samples_files()) {
		opd_backup_samples_files();
	}

	opd_open_files();

	/* yes, this is racey. */
	opd_get_ascii_procs();

	for (i=0; i< OPD_MAX_STATS; i++) {
		opd_stats[i] = 0;
	}

	act.sa_handler = opd_alarm;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);

	if (sigaction(SIGALRM, &act, NULL)) {
		perror("oprofiled: install of SIGALRM handler failed: ");
		exit(1);
	}

	act.sa_handler = opd_sighup;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGALRM);

	if (sigaction(SIGHUP, &act, NULL)) {
		perror("oprofiled: install of SIGHUP handler failed: ");
		exit(1);
	}

	sigemptyset(&maskset);
	sigaddset(&maskset, SIGALRM);
	sigaddset(&maskset, SIGHUP);

	/* clean up every 10 minutes */
	alarm(60*10);

	/* simple sleep-then-process loop */
	opd_do_read(sbuf, s_buf_bytesize, nbuf, n_buf_bytesize);

	opd_print_stats();
	printf("oprofiled stopped %s", opd_get_time());
	return 0;
}
