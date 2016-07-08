/*
 * Copyright (C) 2013 Julien Desfossez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdint.h>
#include <babeltrace/babeltrace.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/ctf/callbacks.h>
#include <babeltrace/ctf/iterator.h>
#include <fcntl.h>
#include <pthread.h>
#include <popt.h>
#include <stdlib.h>
#include <ftw.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <fts.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define LTTNG_SYMBOL_NAME_LEN 256

#ifdef LTTNGTOP_MMAP_LIVE
#include <lttng/lttngtop-helper.h>
#include <babeltrace/lttngtopmmappacketseek.h>
#include "mmap-live.h"
#endif /* LTTNGTOP_MMAP_LIVE */

#include "lttngtoptypes.h"
#include "cputop.h"
#include "iostreamtop.h"
#include "common.h"
#include "network-live.h"
#include "lttng-session.h"

#ifdef HAVE_LIBNCURSES
#include "cursesdisplay.h"
#endif

#define NET_URL_PREFIX	"net://"
#define NET4_URL_PREFIX	"net4://"
#define NET6_URL_PREFIX	"net6://"

#define DEFAULT_FILE_ARRAY_SIZE 1

const char *opt_input_path;
int opt_textdump;
int opt_child;
int opt_begin;
int opt_all;

int quit = 0;
/* We need at least one valid trace to start processing. */
int valid_trace = 0;

struct lttngtop *copy;
pthread_t display_thread;
pthread_t timer_thread;

unsigned long refresh_display = 1 * NSEC_PER_SEC;
unsigned long last_display_update = 0;
unsigned long last_event_ts = 0;
struct syscall *last_syscall;

/* list of FDs available for being read with snapshots */
struct bt_mmap_stream_list mmap_list;
GPtrArray *lttng_consumer_stream_array;
int sessiond_metadata, consumerd_metadata;
struct lttng_consumer_local_data *ctx = NULL;
/* list of snapshots currently not consumed */
GPtrArray *available_snapshots;
sem_t metadata_available;
int reload_trace = 0;

uint64_t prev_ts = 0;

enum {
	OPT_NONE = 0,
	OPT_HELP,
	OPT_TEXTDUMP,
	OPT_PID,
	OPT_CHILD,
	OPT_PROCNAME,
	OPT_RELAY_HOSTNAME,
	OPT_KPROBES,
	OPT_BEGIN,
	OPT_ALL,
	OPT_OUTPUT_FILE,
	OPT_VERBOSE,
	OPT_GUI_TEST,
	OPT_CREATE_LOCAL_SESSION,
	OPT_CREATE_LIVE_SESSION,
};

static struct poptOption long_options[] = {
	/* longName, shortName, argInfo, argPtr, value, descrip, argDesc */
	{ "help", 'h', POPT_ARG_NONE, NULL, OPT_HELP, NULL, NULL },
	{ "textdump", 't', POPT_ARG_NONE, NULL, OPT_TEXTDUMP, NULL, NULL },
	{ "child", 'f', POPT_ARG_NONE, NULL, OPT_CHILD, NULL, NULL },
	{ "begin", 'b', POPT_ARG_NONE, NULL, OPT_BEGIN, NULL, NULL },
	{ "pid", 'p', POPT_ARG_STRING, &opt_tid, OPT_PID, NULL, NULL },
	{ "procname", 'n', POPT_ARG_STRING, &opt_procname, OPT_PROCNAME, NULL, NULL },
	{ "relay-hostname", 'r', POPT_ARG_STRING, &opt_relay_hostname,
		OPT_RELAY_HOSTNAME, NULL, NULL },
	{ "kprobes", 'k', POPT_ARG_STRING, &opt_kprobes, OPT_KPROBES, NULL, NULL },
	{ "all", 'a', POPT_ARG_NONE, NULL, OPT_ALL, NULL, NULL },
	{ "output", 'o', POPT_ARG_STRING, &opt_output, OPT_OUTPUT_FILE, NULL, NULL },
	{ "verbose", 'v', POPT_ARG_NONE, NULL, OPT_VERBOSE, NULL, NULL },
	{ "gui-test", 'g', POPT_ARG_NONE, NULL, OPT_GUI_TEST, NULL, NULL },
	{ "create-local-session", 0, POPT_ARG_NONE, NULL, OPT_CREATE_LOCAL_SESSION, NULL, NULL },
	{ "create-live-session", 0, POPT_ARG_NONE, NULL, OPT_CREATE_LIVE_SESSION, NULL, NULL },
	{ NULL, 0, 0, NULL, 0, NULL, NULL },
};

#ifdef LTTNGTOP_MMAP_LIVE
static void handle_textdump_sigterm(int signal)
{
	quit = 1;
	lttng_destroy_session("test");
}
#endif

void *refresh_thread(void *p)
{
	while (1) {
		if (quit) {
			sem_post(&pause_sem);
			sem_post(&timer);
			sem_post(&end_trace_sem);
			sem_post(&goodtodisplay);
			sem_post(&goodtoupdate);
			pthread_exit(0);
		}
		if (!opt_input_path) {
#ifdef LTTNGTOP_MMAP_LIVE 
			mmap_live_flush(mmap_list);
#endif
		}
		sem_wait(&pause_sem);
		sem_post(&pause_sem);
		sem_post(&timer);
		sleep(refresh_display/NSEC_PER_SEC);
	}
}

#ifdef HAVE_LIBNCURSES
void *ncurses_display(void *p)
{
	unsigned int current_display_index = 0;

	sem_wait(&bootstrap);
	/*
	 * Prevent the 1 second delay when we hit ESC
	 */
	ESCDELAY = 0;
	init_ncurses();

	while (1) {
		sem_wait(&timer);
		sem_wait(&goodtodisplay);
		sem_wait(&pause_sem);

		if (quit) {
			sem_post(&pause_sem);
			sem_post(&timer);
			reset_ncurses();
			pthread_exit(0);
		}

		copy = g_ptr_array_index(copies, current_display_index);
		assert(copy);
		display(current_display_index++);

		sem_post(&goodtoupdate);
		sem_post(&pause_sem);
	}
}
#endif /* HAVE_LIBNCURSES */

void print_fields(struct bt_ctf_event *event, const char *procname,
		int pid)
{
	unsigned int cnt, i;
	const struct bt_definition *const * list;
	const struct bt_declaration *l;
	const struct bt_definition *scope;
	enum ctf_type_id type;
	const char *str;
	struct processtop *current_proc;
	struct files *current_file;
	int fd, fd_value = -1;

	scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);

	bt_ctf_get_field_list(event, scope, &list, &cnt);
	for (i = 0; i < cnt; i++) {
		if (i != 0)
			fprintf(output, ", ");
		fprintf(output, "%s = ", bt_ctf_field_name(list[i]));
		l = bt_ctf_get_decl_from_def(list[i]);
		if (strncmp(bt_ctf_field_name(list[i]), "fd", 2) == 0)
			fd = 1;
		else
			fd = 0;
		type = bt_ctf_field_type(l);
		if (type == CTF_TYPE_INTEGER) {
			if (bt_ctf_get_int_signedness(l) == 0) {
				fd_value = bt_ctf_get_uint64(list[i]);
				fprintf(output, "%" PRIu64, bt_ctf_get_uint64(list[i]));
			} else {
				fd_value = bt_ctf_get_int64(list[i]);
				fprintf(output, "%" PRId64, bt_ctf_get_int64(list[i]));
			}
		} else if (type == CTF_TYPE_STRING) {
			fprintf(output, "%s", bt_ctf_get_string(list[i]));
		} else if (type == CTF_TYPE_ARRAY) {
			str = bt_ctf_get_char_array(list[i]);
			if (!bt_ctf_field_get_error() && str)
				fprintf(output, "%s", str);
		}
		if (fd) {
			current_proc = find_process_tid(&lttngtop, pid, procname);
			if (!current_proc)
				continue;
			current_file = get_file(current_proc, fd_value);
			if (!current_file || !current_file->name)
				continue;
			fprintf(output, "<%s>", current_file->name);
		}
	}
}

/*
 * hook on each event to check the timestamp and refresh the display if
 * necessary
 */
enum bt_cb_ret textdump(struct bt_ctf_event *call_data, void *private_data)
{
	unsigned long timestamp;
	uint64_t delta;
	struct tm start;
	uint64_t ts_nsec_start;
	int pid, cpu_id, tid, ret, lookup, current_syscall = 0;
	const struct bt_definition *scope;
	const char *hostname, *procname;
	struct cputime *cpu;
	char *from_syscall = NULL;
	int syscall_exit = 0;

	timestamp = bt_ctf_get_timestamp(call_data);

	/* can happen in network live when tracing is idle */
	if (timestamp < last_event_ts)
		goto end_stop;

	last_event_ts = timestamp;

	start = format_timestamp(timestamp);
	ts_nsec_start = timestamp % NSEC_PER_SEC;

	pid = get_context_pid(call_data);
	if (pid == -1ULL && opt_tid) {
		goto error;
	}

	tid = get_context_tid(call_data);
	
	hostname = get_context_hostname(call_data);
	if (opt_child)
		lookup = pid;
	else
		lookup = tid;
	if (opt_tid || opt_procname || opt_exec_name) {
		if (!lookup_filter_tid_list(lookup)) {
			/* To display when a process of ours in getting scheduled in */
			if (strcmp(bt_ctf_event_name(call_data), "sched_switch") == 0) {
				int next_tid;

				scope = bt_ctf_get_top_level_scope(call_data,
						BT_EVENT_FIELDS);
				next_tid = bt_ctf_get_int64(bt_ctf_get_field(call_data,
							scope, "_next_tid"));
				if (bt_ctf_field_get_error()) {
					fprintf(stderr, "Missing next_tid field\n");
					goto error;
				}
				if (!lookup_filter_tid_list(next_tid)) {
					if (!opt_all)
						goto end;
				} else {
					if (opt_all)
						fprintf(output, "%c[1m", 27);
				}
			} else if (!opt_all) {
				goto end;
			}
		} else {
			if (opt_all)
				fprintf(output, "%c[1m", 27);
		}
	}

	if (((strncmp(bt_ctf_event_name(call_data),
						"exit_syscall", 12)) == 0) ||
			((strncmp(bt_ctf_event_name(call_data),
				 "syscall_exit", 12)) == 0)) {
		syscall_exit = 1;
	}

	if (last_syscall && !syscall_exit) {
		last_syscall = NULL;
		fprintf(output, " ...interrupted...\n");
	}

	cpu_id = get_cpu_id(call_data);
	procname = get_context_comm(call_data);
	if ((strncmp(bt_ctf_event_name(call_data), "sys_", 4) == 0) ||
			(strncmp(bt_ctf_event_name(call_data), "syscall_entry", 13) == 0)){
		cpu = get_cpu(cpu_id);
		cpu->current_syscall = g_new0(struct syscall, 1);
		cpu->current_syscall->name = strdup(bt_ctf_event_name(call_data));
		cpu->current_syscall->ts_start = timestamp;
		cpu->current_syscall->cpu_id = cpu_id;
		last_syscall = cpu->current_syscall;
		current_syscall = 1;
	} else if (syscall_exit) {
		struct tm start_ts;

		/* Return code of a syscall if it was the last displayed event. */
		if (last_syscall && last_syscall->ts_start == prev_ts) {
			if (last_syscall->cpu_id == cpu_id) {
				int64_t syscall_ret;

				delta = timestamp - last_syscall->ts_start;
				scope = bt_ctf_get_top_level_scope(call_data,
						BT_EVENT_FIELDS);
				syscall_ret = bt_ctf_get_int64(bt_ctf_get_field(call_data,
							scope, "_ret"));

				fprintf(output, "= %" PRId64 " (%" PRIu64 ".%09" PRIu64 "s)\n",
						syscall_ret, delta / NSEC_PER_SEC,
						delta % NSEC_PER_SEC);
				last_syscall = NULL;
				goto end;
			} else {
				last_syscall = NULL;
				fprintf(output, " ...interrupted...\n");
			}
		}

		cpu = get_cpu(cpu_id);
		if (cpu->current_syscall) {
			delta = timestamp - cpu->current_syscall->ts_start;
			start_ts = format_timestamp(cpu->current_syscall->ts_start);
			ret = asprintf(&from_syscall, " [from %02d:%02d:%02d.%09" PRIu64
					" (+%" PRIu64 ".%09" PRIu64 ") (cpu %d) %s]",
					start_ts.tm_hour, start_ts.tm_min, start_ts.tm_sec,
					cpu->current_syscall->ts_start % NSEC_PER_SEC,
					delta / NSEC_PER_SEC, delta % NSEC_PER_SEC,
					cpu_id, cpu->current_syscall->name);
			if (ret < 0) {
				goto error;
			}
			free(cpu->current_syscall->name);
			g_free(cpu->current_syscall);
			cpu->current_syscall = NULL;
			last_syscall = NULL;
		}
	}

	if (prev_ts == 0)
		prev_ts = timestamp;
	delta = timestamp - prev_ts;
	prev_ts = timestamp;

	fprintf(output, "%02d:%02d:%02d.%09" PRIu64 " (+%" PRIu64 ".%09" PRIu64 ") %s%s"
			"(cpu %d) [%s (%d/%d)] %s (",
			start.tm_hour, start.tm_min, start.tm_sec,
			ts_nsec_start, delta / NSEC_PER_SEC,
			delta % NSEC_PER_SEC, (hostname) ? hostname : "",
			(hostname) ? " ": "", cpu_id, procname, pid, tid,
			bt_ctf_event_name(call_data));
	print_fields(call_data, procname, pid);
	fprintf(output, ")%s%c", (from_syscall) ? from_syscall : "",
			(!current_syscall) ? '\n' : ' ');

	free(from_syscall);
	if (opt_all && (opt_tid || opt_procname || opt_exec_name))
		fprintf(output, "%c[0m", 27);

end:
	return BT_CB_OK;
error:
	return BT_CB_ERROR_STOP;
end_stop:
	return BT_CB_OK_STOP;
}

enum bt_cb_ret handle_kprobes(struct bt_ctf_event *call_data, void *private_data)
{
	int i;
	struct kprobes *kprobe;

	/* for kprobes */
	for (i = 0; i < lttngtop.kprobes_table->len; i++) {
		kprobe = g_ptr_array_index(lttngtop.kprobes_table, i);
		if (strcmp(bt_ctf_event_name(call_data), kprobe->probe_name) == 0) {
			kprobe->count++;
		}
	}

	return BT_CB_OK;
}

/*
 * hook on each event to check the timestamp and refresh the display if
 * necessary
 */
enum bt_cb_ret check_timestamp(struct bt_ctf_event *call_data, void *private_data)
{
	unsigned long timestamp;

	timestamp = bt_ctf_get_timestamp(call_data);
	if (timestamp == -1ULL)
		goto error;

	/* can happen in network live when tracing is idle */
	if (timestamp < last_event_ts)
		goto end_stop;

	last_event_ts = timestamp;

	if (last_display_update == 0)
		last_display_update = timestamp;

	if (timestamp - last_display_update >= refresh_display) {
		sem_wait(&goodtoupdate);
		g_ptr_array_add(copies, get_copy_lttngtop(last_display_update,
					timestamp));
		sem_post(&goodtodisplay);
		sem_post(&bootstrap);
		last_display_update = timestamp;
	}
	return BT_CB_OK;

error:
	fprintf(stderr, "check_timestamp callback error\n");
	return BT_CB_ERROR_STOP;

end_stop:
	return BT_CB_OK_STOP;
}

/*
 * get_perf_counter : get or create and return a perf_counter struct for
 * either a process or a cpu (only one of the 2 parameters mandatory)
 */
struct perfcounter *get_perf_counter(const char *name, struct processtop *proc,
		struct cputime *cpu)
{
	struct perfcounter *ret;
	GHashTable *table;

	if (proc)
		table = proc->perf;
	else if (cpu)
		table = cpu->perf;
	else
		goto error;

	ret = g_hash_table_lookup(table, (gpointer) name);
	if (ret)
		goto end;

	ret = g_new0(struct perfcounter, 1);
	/* by default, make it visible in the UI */
	ret->visible = 1;
	g_hash_table_insert(table, (gpointer) strdup(name), ret);

end:
	return ret;

error:
	return NULL;
}

void update_perf_value(struct processtop *proc, struct cputime *cpu,
		const char *name, int value)
{
	struct perfcounter *cpu_perf, *process_perf;

	cpu_perf = get_perf_counter(name, NULL, cpu);
	if (cpu_perf->count < value) {
		process_perf = get_perf_counter(name, proc, NULL);
		process_perf->count += value - cpu_perf->count;
		cpu_perf->count = value;
	}
}

void extract_perf_counter_scope(const struct bt_ctf_event *event,
		const struct bt_definition *scope,
		struct processtop *proc,
		struct cputime *cpu)
{
	struct bt_definition const * const *list = NULL;
	const struct bt_definition *field;
	unsigned int count;
	struct perfcounter *perfcounter;
	GHashTableIter iter;
	gpointer key;
	int ret;

	if (!scope)
		goto end;

	ret = bt_ctf_get_field_list(event, scope, &list, &count);
	if (ret < 0)
		goto end;

	if (count == 0)
		goto end;

	g_hash_table_iter_init(&iter, global_perf_liszt);
	while (g_hash_table_iter_next (&iter, &key, (gpointer) &perfcounter)) {
		field = bt_ctf_get_field(event, scope, (char *) key);
		if (field) {
			int value = bt_ctf_get_uint64(field);
			if (bt_ctf_field_get_error())
				continue;
			update_perf_value(proc, cpu, (char *) key, value);
		}
	}

end:
	return;
}

void update_perf_counter(struct processtop *proc, const struct bt_ctf_event *event)
{
	struct cputime *cpu;
	const struct bt_definition *scope;

	cpu = get_cpu(get_cpu_id(event));

	scope = bt_ctf_get_top_level_scope(event, BT_STREAM_EVENT_CONTEXT);
	extract_perf_counter_scope(event, scope, proc, cpu);

	scope = bt_ctf_get_top_level_scope(event, BT_STREAM_PACKET_CONTEXT);
	extract_perf_counter_scope(event, scope, proc, cpu);

	scope = bt_ctf_get_top_level_scope(event, BT_EVENT_CONTEXT);
	extract_perf_counter_scope(event, scope, proc, cpu);
}

enum bt_cb_ret fix_process_table(struct bt_ctf_event *call_data,
		void *private_data)
{
	int pid, tid, ppid, vpid, vtid, vppid;
	char *comm, *hostname;
	struct processtop *parent, *child;
	unsigned long timestamp;

	timestamp = bt_ctf_get_timestamp(call_data);
	if (timestamp == -1ULL)
		goto error;

	pid = get_context_pid(call_data);
	if (pid == -1ULL) {
		goto error;
	}

	tid = get_context_tid(call_data);
	if (tid == -1ULL) {
		goto error;
	}
	ppid = get_context_ppid(call_data);
	if (ppid == -1ULL) {
		goto end;
	}
	vpid = get_context_vpid(call_data);
	if (pid == -1ULL) {
		vpid = -1;
	}
	vtid = get_context_vtid(call_data);
	if (tid == -1ULL) {
		vtid = -1;
	}
	vppid = get_context_vppid(call_data);
	if (ppid == -1ULL) {
		vppid = -1;
	}
	comm = get_context_comm(call_data);
	if (!comm) {
		goto error;
	}
	/* optional */
	hostname = get_context_hostname(call_data);

	/* find or create the current process */
	child = find_process_tid(&lttngtop, tid, comm);
	if (!child)
		child = add_proc(&lttngtop, tid, comm, timestamp, hostname);
	if (!child)
		goto end;
	update_proc(child, pid, tid, ppid, vpid, vtid, vppid, comm, hostname);

	if (opt_procname && lookup_procname(comm) &&
			!lookup_filter_tid_list(tid)) {
		int *tmp_tid;

		tmp_tid = malloc(sizeof(int));
		*tmp_tid = tid;
		printf("ADDING %s %d\n", comm, tid);
		g_hash_table_insert(global_filter_list,
				(gpointer) tmp_tid, tmp_tid);
	}

	if (pid != tid) {
		/* find or create the parent */
		parent = find_process_tid(&lttngtop, pid, comm);
		if (!parent) {
			parent = add_proc(&lttngtop, pid, comm, timestamp, hostname);
			if (parent)
				parent->pid = pid;
		}

		/* attach the parent to the current process */
		child->threadparent = parent;
		add_thread(parent, child);
	}

	update_perf_counter(child, call_data);

end:
	return BT_CB_OK;

error:
	return BT_CB_ERROR_STOP;
}

void init_lttngtop()
{
	copies = g_ptr_array_new();
	global_perf_liszt = g_hash_table_new(g_str_hash, g_str_equal);
	global_filter_list = g_hash_table_new(g_str_hash, g_str_equal);
	global_host_list = g_hash_table_new(g_str_hash, g_str_equal);
	global_procname_list = g_hash_table_new(g_str_hash, g_str_equal);
	tid_filter_list = g_hash_table_new(g_str_hash,
			g_str_equal);

	sem_init(&goodtodisplay, 0, 0);
	sem_init(&goodtoupdate, 0, 1);
	sem_init(&timer, 0, 1);
	sem_init(&bootstrap, 0, 0);
	sem_init(&pause_sem, 0, 1);
	sem_init(&end_trace_sem, 0, 0);

	reset_global_counters();
	lttngtop.nbproc = 0;
	lttngtop.nbthreads = 0;
	lttngtop.nbfiles = 0;

	lttngtop.process_hash_table = g_hash_table_new(g_direct_hash,
			g_direct_equal);
	lttngtop.process_table = g_ptr_array_new();
	lttngtop.files_table = g_ptr_array_new();
	lttngtop.cpu_table = g_ptr_array_new();

	toggle_filter = -1;
}

void usage(FILE *fp)
{
	fprintf(fp, "LTTngTop %s\n\n", VERSION);
	fprintf(fp, "Usage : lttngtop [OPTIONS] TRACE\n");
	fprintf(fp, "  TRACE                    Path to the trace to analyse (-r for network live tracing, nothing for mmap live streaming)\n");
	fprintf(fp, "  -h, --help               This help message\n");
	fprintf(fp, "  -t, --textdump           Display live events in text-only\n");
	fprintf(fp, "  -p, --pid                Comma-separated list of PIDs to display\n");
	fprintf(fp, "  -f, --child              Follow threads associated with selected PIDs\n");
	fprintf(fp, "  -n, --procname           Comma-separated list of procnames to display (require procname context in trace)\n");
	fprintf(fp, "  -a, --all                In textdump mode, display all events but write in bold the processes we are interested in (-f, -p and -n)\n");
	fprintf(fp, "  -k, --kprobes            Comma-separated list of kprobes to insert (same format as lttng enable-event)\n");
	fprintf(fp, "  -r, --relay-hostname     Network live streaming : hostname of the lttng-relayd (default port)\n");
	fprintf(fp, "  -b, --begin              Network live streaming : read the trace for the beginning of the recording\n");
	fprintf(fp, "  -o, --output <filename>  In textdump, output the log in <filename>\n");
	fprintf(fp, "  -g, --gui-test           Test if the ncurses support is compiled in (return 0 if it is)\n");
	fprintf(fp, "  --create-local-session   Setup a LTTng local session with all the right parameters\n");
	fprintf(fp, "  --create-live-session    Setup a LTTng live session on localhost with all the right parameters\n");
}

/*
 * Parse probe options.
 * Shamelessly stolen from lttng-tools :
 * src/bin/lttng/commands/enable_events.c
 */
static struct kprobes *parse_probe_opts(char *opt)
{
	char s_hex[19];
	char name[LTTNG_SYMBOL_NAME_LEN];
	struct kprobes *kprobe;
	int ret;

	/*
	kprobe->probe_addr = 0;
	kprobe->probe_offset = 0;
	asprintf(&kprobe->probe_name, "probe_sys_open");
	asprintf(&kprobe->symbol_name, "sys_open");
	*/

	if (opt == NULL) {
		kprobe = NULL;
		goto end;
	}

	kprobe = g_new0(struct kprobes, 1);

	/* Check for symbol+offset */
	ret = sscanf(opt, "%[^'+']+%s", name, s_hex);
	if (ret == 2) {
		ret = asprintf(&kprobe->probe_name, "probe_%s", name);
		ret = asprintf(&kprobe->symbol_name, "%s", name);

		if (strlen(s_hex) == 0) {
			fprintf(stderr, "Invalid probe offset %s", s_hex);
			ret = -1;
			goto end;
		}
		kprobe->probe_offset = strtoul(s_hex, NULL, 0);
		kprobe->probe_addr = 0;
		goto end;
	}

	/* Check for symbol */
	if (isalpha(name[0])) {
		ret = sscanf(opt, "%s", name);
		if (ret == 1) {
			ret = asprintf(&kprobe->probe_name, "probe_%s", name);
			ret = asprintf(&kprobe->symbol_name, "%s", name);
			kprobe->probe_offset = 0;
			kprobe->probe_addr = 0;
			goto end;
		}
	}

	/* Check for address */
	ret = sscanf(opt, "%s", s_hex);
	if (ret > 0) {
		if (strlen(s_hex) == 0) {
			fprintf(stderr, "Invalid probe address %s", s_hex);
			ret = -1;
			goto end;
		}
		ret = asprintf(&kprobe->probe_name, "probe_%s", s_hex);
		kprobe->probe_offset = 0;
		kprobe->probe_addr = strtoul(s_hex, NULL, 0);
		goto end;
	}

	/* No match */
	kprobe = NULL;

end:
	return kprobe;
}

/*
 * Return 0 if caller should continue, < 0 if caller should return
 * error, > 0 if caller should exit without reporting error.
 */
static int parse_options(int argc, char **argv)
{
	poptContext pc;
	int opt, ret = 0;
	char *tmp_str;
	int *tid;
	int i;

	remote_live = 0;

	pc = poptGetContext(NULL, argc, (const char **) argv, long_options, 0);
	poptReadDefaultConfig(pc, 0);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
			case OPT_HELP:
				usage(stdout);
				ret = 1;    /* exit cleanly */
				goto end;
			case OPT_GUI_TEST:
#ifdef HAVE_LIBNCURSES
				exit(EXIT_SUCCESS);
#else
				exit(EXIT_FAILURE);
#endif
				goto end;
			case OPT_CREATE_LOCAL_SESSION:
				ret = create_local_session();
				exit(ret);
			case OPT_CREATE_LIVE_SESSION:
				ret = create_live_local_session(NULL, NULL, 1);
				exit(ret);
			case OPT_TEXTDUMP:
				opt_textdump = 1;
				break;
			case OPT_ALL:
				opt_all = 1;
				break;
			case OPT_CHILD:
				opt_child = 1;
				break;
			case OPT_PID:
				toggle_filter = 1;
				tmp_str = strtok(opt_tid, ",");
				while (tmp_str) {
					tid = malloc(sizeof(int));
					*tid = atoi(tmp_str);
					g_hash_table_insert(tid_filter_list,
							(gpointer) tid, tid);
					tmp_str = strtok(NULL, ",");
				}
				break;
			case OPT_BEGIN:
				/* start reading the live trace from the beginning */
				opt_begin = 1;
				break;
			case OPT_PROCNAME:
				toggle_filter = 1;
				tmp_str = strtok(opt_procname, ",");
				while (tmp_str) {
					add_procname_list(tmp_str, 1);
					tmp_str = strtok(NULL, ",");
				}
				break;
			case OPT_RELAY_HOSTNAME:
				remote_live = 1;
				break;
			case OPT_KPROBES:
				lttngtop.kprobes_table = g_ptr_array_new();
				tmp_str = strtok(opt_kprobes, ",");
				while (tmp_str) {
					struct kprobes *kprobe;

					kprobe = parse_probe_opts(tmp_str);
					if (kprobe) {
						g_ptr_array_add(
							lttngtop.kprobes_table,
							kprobe);
					} else {
						ret = -EINVAL;
						goto end;
					}
					tmp_str = strtok(NULL, ",");
				}
				break;
			case OPT_OUTPUT_FILE:
				break;
			case OPT_VERBOSE:
				babeltrace_verbose = 1;
				break;
			default:
				ret = -EINVAL;
				goto end;
		}
	}

	opt_exec_name = NULL;
	opt_exec_argv = NULL;
	for (i = 0; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == '-') {
			opt_exec_name = argv[i + 1];
			opt_exec_argv = &argv[i + 1];
			break;
		}
	}
	if (!opt_exec_name) {
		opt_input_path = poptGetArg(pc);
	}
	if (!opt_output) {
		opt_output = strdup("/dev/stdout");
	}
	output = fopen(opt_output, "w");
	if (!output) {
		perror("Error opening output file");
		ret = -1;
		goto end;
	}

end:
	if (pc) {
		poptFreeContext(pc);
	}
	return ret;
}

void iter_trace(struct bt_context *bt_ctx)
{
	struct bt_ctf_iter *iter;
	struct bt_iter_pos begin_pos;
	struct kprobes *kprobe;
	const struct bt_ctf_event *event;
	int i;
	int ret = 0;

	begin_pos.type = BT_SEEK_BEGIN;
	iter = bt_ctf_iter_create(bt_ctx, &begin_pos, NULL);

	/* at each event, verify the status of the process table */
	bt_ctf_iter_add_callback(iter, 0, NULL, 0,
			fix_process_table,
			NULL, NULL, NULL);
	/* to handle the follow child option */
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("sched_process_fork"),
			NULL, 0, handle_sched_process_fork, NULL, NULL, NULL);
	/* to clean up the process table */
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("sched_process_free"),
			NULL, 0, handle_sched_process_free, NULL, NULL, NULL);
	/* to get all the process from the statedumps */
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string(
				"lttng_statedump_process_state"),
			NULL, 0, handle_statedump_process_state,
			NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string(
				"lttng_statedump_file_descriptor"),
			NULL, 0, handle_statedump_file_descriptor,
			NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("sys_open"),
			NULL, 0, handle_sys_open, NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("syscall_entry_open"),
			NULL, 0, handle_sys_open, NULL, NULL, NULL);

	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("sys_socket"),
			NULL, 0, handle_sys_socket, NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("syscall_entry_socket"),
			NULL, 0, handle_sys_socket, NULL, NULL, NULL);

	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("sys_close"),
			NULL, 0, handle_sys_close, NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("syscall_entry_close"),
			NULL, 0, handle_sys_close, NULL, NULL, NULL);

	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("exit_syscall"),
			NULL, 0, handle_exit_syscall, NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("syscall_exit_open"),
			NULL, 0, handle_exit_syscall, NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("syscall_exit_socket"),
			NULL, 0, handle_exit_syscall, NULL, NULL, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("syscall_exit_close"),
			NULL, 0, handle_exit_syscall, NULL, NULL, NULL);
	if (opt_textdump) {
		bt_ctf_iter_add_callback(iter, 0, NULL, 0,
				textdump,
				NULL, NULL, NULL);
	} else {
		/* at each event check if we need to refresh */
		bt_ctf_iter_add_callback(iter, 0, NULL, 0,
				check_timestamp,
				NULL, NULL, NULL);
		/* to handle the scheduling events */
		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("sched_switch"),
				NULL, 0, handle_sched_switch, NULL, NULL, NULL);
		/* for IO top */
		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("sys_write"),
				NULL, 0, handle_sys_write, NULL, NULL, NULL);
		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("syscall_entry_write"),
				NULL, 0, handle_sys_write, NULL, NULL, NULL);
		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("syscall_exit_write"),
				NULL, 0, handle_exit_syscall, NULL, NULL, NULL);

		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("sys_read"),
				NULL, 0, handle_sys_read, NULL, NULL, NULL);
		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("syscall_entry_read"),
				NULL, 0, handle_sys_read, NULL, NULL, NULL);
		bt_ctf_iter_add_callback(iter,
				g_quark_from_static_string("syscall_exit_read"),
				NULL, 0, handle_exit_syscall, NULL, NULL, NULL);

		/* for kprobes */
		if (lttngtop.kprobes_table) {
			for (i = 0; i < lttngtop.kprobes_table->len; i++) {
				kprobe = g_ptr_array_index(lttngtop.kprobes_table, i);
				bt_ctf_iter_add_callback(iter,
						g_quark_from_static_string(
							kprobe->probe_name),
						NULL, 0, handle_kprobes,
						NULL, NULL, NULL);
			}
		}
	}

	if (opt_exec_name) {
		pid_t pid;

		pid = fork();
		if (pid == 0) {
			execvpe(opt_exec_name, opt_exec_argv, opt_exec_env);
			exit(EXIT_SUCCESS);
		} else if (pid > 0) {
			opt_exec_pid = pid;
			g_hash_table_insert(tid_filter_list,
					(gpointer) &pid,
					&pid);
		} else {
			perror("fork");
			exit(EXIT_FAILURE);
		}
	}

	while ((event = bt_ctf_iter_read_event(iter)) != NULL) {
		if (quit || reload_trace)
			goto end_iter;
		ret = bt_iter_next(bt_ctf_get_iter(iter));
		if (ret < 0)
			goto end_iter;
	}

	/* block until quit, we reached the end of the trace */
	sem_wait(&end_trace_sem);

end_iter:
	bt_ctf_iter_destroy(iter);
}

/*
 * bt_context_add_traces_recursive: Open a trace recursively
 * (copied from BSD code in converter/babeltrace.c)
 *
 * Find each trace present in the subdirectory starting from the given
 * path, and add them to the context. The packet_seek parameter can be
 * NULL: this specify to use the default format packet_seek.
 *
 * Return: 0 on success, nonzero on failure.
 * Unable to open toplevel: failure.
 * Unable to open some subdirectory or file: warn and continue;
 */
int bt_context_add_traces_recursive(struct bt_context *ctx, const char *path,
		const char *format_str,
		void (*packet_seek)(struct bt_stream_pos *pos,
			size_t offset, int whence))
{
	FTS *tree;
	FTSENT *node;
	GArray *trace_ids;
	char lpath[PATH_MAX];
	char * const paths[2] = { lpath, NULL };
	int ret = -1;

	if ((strncmp(path, NET4_URL_PREFIX, sizeof(NET4_URL_PREFIX) - 1)) == 0 ||
			(strncmp(path, NET6_URL_PREFIX, sizeof(NET6_URL_PREFIX) - 1)) == 0 ||
			(strncmp(path, NET_URL_PREFIX, sizeof(NET_URL_PREFIX) - 1)) == 0) {
		ret = bt_context_add_trace(ctx,
				path, format_str, packet_seek, NULL, NULL);
		if (ret < 0) {
			fprintf(stderr, "[warning] [Context] cannot open trace \"%s\" "
					"for reading.\n", path);
			/* Allow to skip erroneous traces. */
			ret = 1;	/* partial error */
		}
		return ret;
	}
	/*
	 * Need to copy path, because fts_open can change it.
	 * It is the pointer array, not the strings, that are constant.
	 */
	strncpy(lpath, path, PATH_MAX);
	lpath[PATH_MAX - 1] = '\0';

	tree = fts_open(paths, FTS_NOCHDIR | FTS_LOGICAL, 0);
	if (tree == NULL) {
		fprintf(stderr, "[error] [Context] Cannot traverse \"%s\" for reading.\n",
				path);
		return -EINVAL;
	}

	trace_ids = g_array_new(FALSE, TRUE, sizeof(int));

	while ((node = fts_read(tree))) {
		int dirfd, metafd;

		if (!(node->fts_info & FTS_D))
			continue;

		dirfd = open(node->fts_accpath, 0);
		if (dirfd < 0) {
			fprintf(stderr, "[error] [Context] Unable to open trace "
				"directory file descriptor.\n");
			ret = dirfd;
			goto error;
		}
		metafd = openat(dirfd, "metadata", O_RDONLY);
		if (metafd < 0) {
			close(dirfd);
			continue;
		} else {
			int trace_id;

			ret = close(metafd);
			if (ret < 0) {
				perror("close");
				goto error;
			}
			ret = close(dirfd);
			if (ret < 0) {
				perror("close");
				goto error;
			}

			trace_id = bt_context_add_trace(ctx,
				node->fts_accpath, format_str,
				packet_seek, NULL, NULL);
			if (trace_id < 0) {
				fprintf(stderr, "[warning] [Context] opening trace \"%s\" from %s "
					"for reading.\n", node->fts_accpath, path);
				/* Allow to skip erroneous traces. */
				continue;
			}
			g_array_append_val(trace_ids, trace_id);
		}
	}

	g_array_free(trace_ids, TRUE);
	return ret;

error:
	return ret;
}

static int check_field_requirements(const struct bt_ctf_field_decl *const * field_list,
		int field_cnt, int *tid_check, int *pid_check,
		int *procname_check, int *ppid_check)
{
	int j;
	struct perfcounter *global;
	const char *name;

	for (j = 0; j < field_cnt; j++) {
		name = bt_ctf_get_decl_field_name(field_list[j]);
		if (*tid_check == 0) {
			if (strncmp(name, "tid", 3) == 0)
				(*tid_check)++;
		}
		if (*pid_check == 0) {
			if (strncmp(name, "pid", 3) == 0)
				(*pid_check)++;
		}
		if (*ppid_check == 0) {
			if (strncmp(name, "ppid", 4) == 0)
				(*ppid_check)++;
		}
		if (*procname_check == 0) {
			if (strncmp(name, "procname", 8) == 0)
				(*procname_check)++;
		}
		if (strncmp(name, "perf_", 5) == 0) {
			global = g_hash_table_lookup(global_perf_liszt, (gpointer) name);
			if (!global) {
				global = g_new0(struct perfcounter, 1);
				/* by default, sort on the first perf context */
				if (g_hash_table_size(global_perf_liszt) == 0)
					global->sort = 1;
				global->visible = 1;
				g_hash_table_insert(global_perf_liszt, (gpointer) strdup(name), global);
			}
		}
	}

	if (*tid_check == 1 && *pid_check == 1 && *ppid_check == 1 &&
			*procname_check == 1)
		return 0;

	return -1;
}

/*
 * check_requirements: check if the required context informations are available
 *
 * If each mandatory context information is available for at least in one
 * event, return 0 otherwise return -1.
 */
int check_requirements(struct bt_context *ctx)
{
	unsigned int i, evt_cnt, field_cnt;
	struct bt_ctf_event_decl *const * evt_list;
	const struct bt_ctf_field_decl *const * field_list;
	int tid_check = 0;
	int pid_check = 0;
	int procname_check = 0;
	int ppid_check = 0;
	int ret = 0;

	ret = bt_ctf_get_event_decl_list(0, ctx, &evt_list, &evt_cnt);
	if (ret < 0) {
		goto end;
	}

	for (i = 0; i < evt_cnt; i++) {
		bt_ctf_get_decl_fields(evt_list[i], BT_STREAM_EVENT_CONTEXT,
				&field_list, &field_cnt);
		ret = check_field_requirements(field_list, field_cnt,
				&tid_check, &pid_check, &procname_check,
				&ppid_check);

		bt_ctf_get_decl_fields(evt_list[i], BT_EVENT_CONTEXT,
				&field_list, &field_cnt);
		ret = check_field_requirements(field_list, field_cnt,
				&tid_check, &pid_check, &procname_check,
				&ppid_check);

		bt_ctf_get_decl_fields(evt_list[i], BT_STREAM_PACKET_CONTEXT,
				&field_list, &field_cnt);
		ret = check_field_requirements(field_list, field_cnt,
				&tid_check, &pid_check, &procname_check,
				&ppid_check);
	}

	if (tid_check == 0) {
		ret = -1;
		fprintf(stderr, "[error] missing tid context information\n");
	}
	if (pid_check == 0) {
		ret = -1;
		fprintf(stderr, "[error] missing pid context information\n");
	}
	if (ppid_check == 0) {
		ret = -1;
		fprintf(stderr, "[error] missing ppid context information\n");
	}
	if (procname_check == 0) {
		ret = -1;
		fprintf(stderr, "[error] missing procname context information\n");
	}
	if (ret == 0) {
		valid_trace = 1;
	}

end:
	return ret;
}

static void handle_sigchild(int signal)
{
	int status;

	waitpid(opt_exec_pid, &status, 0);
}

int main(int argc, char **argv, char **envp)
{
	int ret;
	struct bt_context *bt_ctx = NULL;
	char *live_session_name = NULL;

	init_lttngtop();
	ret = parse_options(argc, argv);
	if (ret < 0) {
		fprintf(stdout, "Error parsing options.\n\n");
		usage(stdout);
		exit(EXIT_FAILURE);
	} else if (ret > 0) {
		exit(EXIT_SUCCESS);
	}

	if (opt_exec_name) {
		opt_exec_env = envp;
		signal(SIGCHLD, handle_sigchild);
	}

	if (!opt_input_path && !remote_live && !opt_exec_name) {
		/* mmap live */
		ret = create_live_local_session(&opt_relay_hostname,
				&live_session_name, 0);
		if (ret < 0)
			goto end;
		remote_live = 1;
	}
	if (!opt_input_path && remote_live) {
		/* network live */
		bt_ctx = bt_context_create();
		ret = bt_context_add_traces_recursive(bt_ctx, opt_relay_hostname,
				"lttng-live", NULL);
		if (ret < 0) {
			fprintf(stderr, "[error] Opening the trace\n");
			goto end;
		}
	} else {
		bt_ctx = bt_context_create();
		ret = bt_context_add_traces_recursive(bt_ctx, opt_input_path, "ctf", NULL);
		if (ret < 0) {
			fprintf(stderr, "[error] Opening the trace\n");
			goto end;
		}

		ret = check_requirements(bt_ctx);
		if (ret < 0 && !valid_trace) {
			fprintf(stderr, "[error] some mandatory contexts "
					"were missing, exiting.\n");
			//goto end;
		}

		if (!opt_textdump) {
#ifdef HAVE_LIBNCURSES
			pthread_create(&display_thread, NULL, ncurses_display,
					(void *) NULL);
			pthread_create(&timer_thread, NULL, refresh_thread,
					(void *) NULL);
#else
			printf("Ncurses support not compiled, please install "
					"the missing dependencies and recompile\n");
			goto end;
#endif
		}

		iter_trace(bt_ctx);
	}


	pthread_join(display_thread, NULL);
	quit = 1;
	pthread_join(timer_thread, NULL);

	ret = 0;

end:
	if (bt_ctx)
		bt_context_put(bt_ctx);

	if (live_session_name) {
		ret = destroy_live_local_session(live_session_name);
		if (ret < 0) {
			fprintf(stderr, "Error destroying %s\n", live_session_name);
		}
	}

	return ret;
}
