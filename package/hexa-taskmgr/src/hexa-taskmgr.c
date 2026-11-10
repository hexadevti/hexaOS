/*
 * hexa-taskmgr: the hexaOS Task Manager (GTK 3).
 *
 *   Apps         the apps with windows on the desktop, with the CPU and
 *                memory of all their processes; end them
 *   Processes    running programs with their CPU and memory use; end them
 *   Performance  live graphs of CPU, memory, disk and network use
 *   Services     the system services (/etc/init.d); start, stop, restart
 *
 * Opened from the Start menu, by right-clicking the taskbar or the Start
 * button, or with Ctrl+Shift+Esc. Everything is read from /proc and /sys
 * once a second. The windows come from the desktop shell, which keeps
 * them in $XDG_RUNTIME_DIR/hexa-windows (Weston patch 0008); services are
 * controlled through /usr/libexec/hexa-taskmgr-helper, run with sudo.
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define APP_ID		"org.hexaos.TaskManager"
#define HISTORY		60		/* samples in a graph: one a second */
#define PF_KTHREAD	0x00200000	/* /proc/PID/stat flag of kernel threads */
#define MAX_STATS	6
#define INIT_DIR	"/etc/init.d"
#define HELPER		"/usr/libexec/hexa-taskmgr-helper"

enum { COL_NAME, COL_PID, COL_USER, COL_STATUS, COL_CPU, COL_MEM, N_COLS };
/* Apps tab: an app row per process with windows, a child row per window
 * (AC_WINID is 0 on app rows) */
enum { AC_ICON, AC_NAME, AC_PID, AC_CPU, AC_MEM, AC_WINID, N_ACOLS };
/* Services tab; SC_STATE: SVC_RUNNING, SVC_STOPPED or SVC_ONESHOT */
enum { SC_NAME, SC_DESC, SC_STATUS, SC_PID, SC_STARTUP, SC_STATE, N_SCOLS };
enum { SVC_ONESHOT = -1, SVC_STOPPED = 0, SVC_RUNNING = 1 };
enum { RES_CPU, RES_MEM, RES_DISK, RES_NET, N_RES };

/* One resource of the Performance tab */
typedef struct {
	const char *name;
	const char *caption;		/* above the big graph */
	double r, g, b;
	double max;			/* graph scale; 0 = automatic */
	gboolean two;			/* a second (dashed) line in hist2 */
	double hist[HISTORY], hist2[HISTORY];
	double scale;			/* current automatic scale */
	GtkWidget *mini, *mini_value;	/* sidebar */
	GtkWidget *graph, *subtitle, *scale_label;
	GtkWidget *stat[MAX_STATS];
	int nstats;
} Res;

typedef struct {
	int pid, ppid;
	unsigned long long ticks;	/* CPU time used so far */
	double cpu;			/* % in the last second */
	guint64 mem;			/* resident bytes */
	char *name;
	const char *user, *status;	/* not owned */
	gboolean seen;
} Proc;

/* A row of the Processes tab: the processes of one program. With one
 * process the row is that process; with more, it adds them up and each
 * one is a child row (like Chrome's many processes on Windows). */
typedef struct {
	GtkTreeIter iter;
	gboolean seen;
	GHashTable *kids;		/* pid -> GtkTreeIter * (2+ processes) */
} Group;

typedef struct {
	GtkTreeIter iter;
	gboolean seen;
} WinRow;

/* A row of the Apps tab: the process that owns some windows (or, when
 * the process is unknown, a single window) */
typedef struct {
	GtkTreeIter iter;
	gboolean seen;
	int pid;
	char *name;
	GHashTable *wins;		/* window id -> WinRow */
	int nwins;
	double cpu;
	guint64 mem;
} AppRow;

typedef struct {
	GtkTreeIter iter;
	gboolean seen;
	char *busy;			/* "Starting…" while an action runs */
} SvcRow;

typedef struct {
	GtkApplication *gapp;
	GtkWidget *window, *tree, *search, *end_btn, *status, *menu;
	GtkTreeStore *store;
	GtkTreeModel *filter, *sort;
	GHashTable *procs;		/* pid -> Proc */
	GHashTable *groups;		/* program name -> Group */
	GHashTable *users;		/* uid -> user name */
	GHashTable *comms;		/* program name -> a pid running it */
	GtkWidget *stack;

	/* Apps tab */
	GtkWidget *app_tree, *app_end_btn, *app_menu, *app_status;
	GtkTreeStore *app_store;
	GtkTreeModel *app_sort;
	GHashTable *apps;		/* "p<pid>" or "w<window id>" -> AppRow */
	char *windows_path;

	/* Services tab */
	GtkWidget *svc_tree, *svc_start, *svc_stop, *svc_restart, *svc_menu;
	GtkWidget *svc_status;
	GtkListStore *svc_store;
	GtkTreeModel *svc_sort;
	GHashTable *svcs;		/* name -> SvcRow */
	char *admin_pw;			/* for sudo, once given */
	gboolean sudo_nopass;		/* sudo needs no password */

	long page_size;
	int ncpu;
	guint timer;
	guint64 mem_total;

	/* previous sample, for rates */
	gboolean have_prev;
	gint64 last_time;
	unsigned long long cpu_total, cpu_idle;
	unsigned long long disk_read, disk_write, disk_io_ms;
	unsigned long long net_rx, net_tx;

	Res res[N_RES];
	GtkWidget *perf_stack;
	char *cpu_model;
} App;

/*
 * Small helpers
 */

static char *
read_file(const char *path)
{
	char *s = NULL;

	if (!g_file_get_contents(path, &s, NULL, NULL))
		return NULL;
	return s;
}

static void
push(double *hist, double v)
{
	memmove(hist, hist + 1, (HISTORY - 1) * sizeof *hist);
	hist[HISTORY - 1] = v;
}

static char *
fmt_size(guint64 bytes)
{
	return g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
}

static char *
fmt_rate(double bytes)
{
	char *s = g_format_size((guint64) (bytes < 0 ? 0 : bytes));
	char *r = g_strdup_printf("%s/s", s);

	g_free(s);
	return r;
}

/* Rounds a throughput up to 1, 2 or 5 x 10^n, at least 10 kB/s */
static double
nice_scale(double v)
{
	static const double steps[] = { 1, 2, 5 };
	double e;
	int i;

	for (e = 1e4; e < 1e13; e *= 10)
		for (i = 0; i < 3; i++)
			if (steps[i] * e >= v)
				return steps[i] * e;
	return 1e13;
}

static void
set_stat(Res *r, int i, char *text)	/* takes text */
{
	if (i < r->nstats)
		gtk_label_set_text(GTK_LABEL(r->stat[i]), text);
	g_free(text);
}

static GtkWidget *styled_label(const char *text, const char *css_class,
			       float xalign);

static void
show_error(App *a, const char *primary, const char *secondary)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", primary);

	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/*
 * Reading the system state
 */

static void
sample_cpu(App *a, double *usage, unsigned long long *dtotal)
{
	unsigned long long v[8] = { 0 }, total = 0, idle;
	FILE *f = fopen("/proc/stat", "r");
	int i;

	*usage = 0;
	*dtotal = 0;
	if (!f)
		return;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) {
		fclose(f);
		return;
	}
	fclose(f);

	for (i = 0; i < 8; i++)
		total += v[i];
	idle = v[3] + v[4];
	if (a->have_prev && total > a->cpu_total) {
		unsigned long long dt = total - a->cpu_total;
		unsigned long long di = idle - a->cpu_idle;

		*usage = 100.0 * (double) (dt - (di < dt ? di : dt)) / dt;
		*dtotal = dt;
	}
	a->cpu_total = total;
	a->cpu_idle = idle;
}

/* Average clock speed of the cores, in MHz (0 if unknown) */
static double
cpu_mhz(App *a)
{
	char line[512];
	double sum = 0;
	int n = 0;
	FILE *f = fopen("/proc/cpuinfo", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *c = strchr(line, ':');

		if (!c)
			continue;
		if (strncmp(line, "cpu MHz", 7) == 0) {
			sum += atof(c + 1);
			n++;
		} else if (!a->cpu_model && strncmp(line, "model name", 10) == 0) {
			a->cpu_model = g_strstrip(g_strdup(c + 1));
		}
	}
	fclose(f);
	return n ? sum / n : 0;
}

static const char *
user_name(App *a, uid_t uid)
{
	char *name = g_hash_table_lookup(a->users, GUINT_TO_POINTER(uid));
	struct passwd *pw;

	if (name)
		return name;
	pw = getpwuid(uid);
	name = pw ? g_strdup(pw->pw_name) : g_strdup_printf("%u", (unsigned) uid);
	g_hash_table_insert(a->users, GUINT_TO_POINTER(uid), name);
	return name;
}

static const char *
status_text(char state)
{
	switch (state) {
	case 'R': return "Running";
	case 'D': return "Waiting";
	case 'T': case 't': return "Stopped";
	case 'Z': return "Not responding";
	default: return "";
	}
}

/* The kernel keeps only 15 characters of a program name: take the full
 * one from the command line when it starts the same way */
static char *
process_name(int pid, const char *comm)
{
	char path[64], *cmdline, *base, *a, *b;
	char *name = NULL;

	if (strlen(comm) < 15)
		return g_strdup(comm);

	snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
	cmdline = read_file(path);
	if (cmdline) {
		a = strrchr(cmdline, '/');
		b = strrchr(cmdline, '\\');
		base = a > b ? a + 1 : b ? b + 1 : cmdline;
		if (g_str_has_prefix(base, comm))
			name = g_strndup(base, strcspn(base, " "));
		g_free(cmdline);
	}
	return name ? name : g_strdup(comm);
}

static void
proc_free(gpointer data)
{
	Proc *p = data;

	g_free(p->name);
	g_free(p);
}

static void
group_free(gpointer data)
{
	Group *g = data;

	g_hash_table_destroy(g->kids);
	g_free(g);
}

static void
set_proc_row(App *a, GtkTreeIter *iter, Proc *p)
{
	gtk_tree_store_set(a->store, iter,
			   COL_NAME, p->name,
			   COL_PID, p->pid,
			   COL_USER, p->user,
			   COL_STATUS, p->status,
			   COL_CPU, p->cpu,
			   COL_MEM, p->mem,
			   -1);
}

/* Puts the processes in the tree, one top row per program name */
static void
update_process_tree(App *a)
{
	GHashTable *by_name = g_hash_table_new_full(g_str_hash, g_str_equal,
		NULL, (GDestroyNotify) g_ptr_array_unref);
	GHashTableIter it, kit;
	gpointer key, value;

	g_hash_table_iter_init(&it, a->procs);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		Proc *p = value;
		GPtrArray *list = g_hash_table_lookup(by_name, p->name);

		if (!list) {
			list = g_ptr_array_new();
			g_hash_table_insert(by_name, p->name, list);
		}
		g_ptr_array_add(list, p);
	}

	g_hash_table_iter_init(&it, a->groups);
	while (g_hash_table_iter_next(&it, &key, &value))
		((Group *) value)->seen = FALSE;

	g_hash_table_iter_init(&it, by_name);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		const char *name = key;
		GPtrArray *list = value;
		Group *g = g_hash_table_lookup(a->groups, name);
		const char *user = NULL;
		double cpu = 0;
		guint64 mem = 0;
		char *title;
		guint i;

		if (!g) {
			g = g_new0(Group, 1);
			g->kids = g_hash_table_new_full(g_direct_hash,
							g_direct_equal, NULL,
							g_free);
			gtk_tree_store_append(a->store, &g->iter, NULL);
			g_hash_table_insert(a->groups, g_strdup(name), g);
		}
		g->seen = TRUE;

		if (list->len == 1) {	/* a single process: no child rows */
			g_hash_table_iter_init(&kit, g->kids);
			while (g_hash_table_iter_next(&kit, NULL, &value))
				gtk_tree_store_remove(a->store, value);
			g_hash_table_remove_all(g->kids);
			set_proc_row(a, &g->iter, list->pdata[0]);
			continue;
		}

		for (i = 0; i < list->len; i++) {
			Proc *p = list->pdata[i];
			GtkTreeIter *ci = g_hash_table_lookup(g->kids,
						GINT_TO_POINTER(p->pid));

			if (!ci) {
				ci = g_new(GtkTreeIter, 1);
				gtk_tree_store_append(a->store, ci, &g->iter);
				g_hash_table_insert(g->kids,
						    GINT_TO_POINTER(p->pid), ci);
			}
			set_proc_row(a, ci, p);
			cpu += p->cpu;
			mem += p->mem;
			if (i == 0)
				user = p->user;
			else if (user && strcmp(user, p->user) != 0)
				user = NULL;	/* several users */
		}
		/* child rows of processes that ended or became another program */
		g_hash_table_iter_init(&kit, g->kids);
		while (g_hash_table_iter_next(&kit, &key, &value)) {
			Proc *p = g_hash_table_lookup(a->procs, key);

			if (!p || strcmp(p->name, name) != 0) {
				gtk_tree_store_remove(a->store, value);
				g_hash_table_iter_remove(&kit);
			}
		}

		title = g_strdup_printf("%s (%u)", name, list->len);
		gtk_tree_store_set(a->store, &g->iter,
				   COL_NAME, title,
				   COL_PID, 0,
				   COL_USER, user ? user : "",
				   COL_STATUS, "",
				   COL_CPU, MIN(cpu, 100.0),
				   COL_MEM, mem,
				   -1);
		g_free(title);
	}

	g_hash_table_iter_init(&it, a->groups);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		Group *g = value;

		if (!g->seen) {
			gtk_tree_store_remove(a->store, &g->iter);
			g_hash_table_iter_remove(&it);
		}
	}
	g_hash_table_destroy(by_name);

	/* a search by PID must also show the group of a new process */
	if (gtk_entry_get_text(GTK_ENTRY(a->search))[0])
		gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(a->filter));
}

static void
scan_processes(App *a, unsigned long long dtotal, int *nprocs, int *nthreads)
{
	GHashTableIter it;
	gpointer key, value;
	struct dirent *de;
	DIR *d;

	*nprocs = *nthreads = 0;
	g_hash_table_iter_init(&it, a->procs);
	while (g_hash_table_iter_next(&it, &key, &value))
		((Proc *) value)->seen = FALSE;
	g_hash_table_remove_all(a->comms);

	d = opendir("/proc");
	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[64], comm[64], state, *stat, *lp, *rp;
		unsigned long flags;
		unsigned long long ut, st, ticks;
		long threads, rss;
		struct stat sb;
		double cpu = 0;
		Proc *p;
		int pid, ppid;

		if (!isdigit((unsigned char) de->d_name[0]))
			continue;
		pid = atoi(de->d_name);
		snprintf(path, sizeof path, "/proc/%d/stat", pid);
		stat = read_file(path);
		if (!stat)
			continue;
		lp = strchr(stat, '(');
		rp = strrchr(stat, ')');
		if (!lp || !rp || rp < lp ||
		    sscanf(rp + 2, "%c %d %*d %*d %*d %*d %lu %*u %*u %*u %*u "
			   "%llu %llu %*d %*d %*d %*d %ld %*d %*u %*u %ld",
			   &state, &ppid, &flags, &ut, &st, &threads, &rss) != 7) {
			g_free(stat);
			continue;
		}
		g_strlcpy(comm, lp + 1, MIN(sizeof comm, (gsize) (rp - lp)));
		g_free(stat);

		if (flags & PF_KTHREAD)
			continue;	/* kernel threads are not programs */

		(*nprocs)++;
		*nthreads += threads;
		/* for the Services tab: is this daemon running? */
		g_hash_table_replace(a->comms, g_strdup(comm),
				     GINT_TO_POINTER(pid));

		snprintf(path, sizeof path, "/proc/%d", pid);
		if (lstat(path, &sb) < 0)
			continue;

		ticks = ut + st;
		p = g_hash_table_lookup(a->procs, GINT_TO_POINTER(pid));
		if (p && dtotal && ticks >= p->ticks)
			cpu = 100.0 * (double) (ticks - p->ticks) / dtotal;
		if (cpu > 100)
			cpu = 100;
		if (!p) {
			p = g_new0(Proc, 1);
			p->pid = pid;
			g_hash_table_insert(a->procs, GINT_TO_POINTER(pid), p);
		}
		p->ppid = ppid;
		p->ticks = ticks;
		p->seen = TRUE;
		p->cpu = cpu;
		p->mem = (guint64) (rss > 0 ? rss : 0) * a->page_size;
		p->user = user_name(a, sb.st_uid);
		p->status = status_text(state);
		g_free(p->name);
		p->name = process_name(pid, comm);
	}
	closedir(d);

	/* forget the processes that ended */
	g_hash_table_iter_init(&it, a->procs);
	while (g_hash_table_iter_next(&it, &key, &value))
		if (!((Proc *) value)->seen)
			g_hash_table_iter_remove(&it);

	update_process_tree(a);
}

typedef struct {
	guint64 total, available, cached, swap_total, swap_free;
} MemInfo;

static void
sample_mem(MemInfo *m)
{
	char line[256], key[64];
	unsigned long long v;
	FILE *f = fopen("/proc/meminfo", "r");

	memset(m, 0, sizeof *m);
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%63[^:]: %llu", key, &v) != 2)
			continue;
		v *= 1024;
		if (strcmp(key, "MemTotal") == 0)
			m->total = v;
		else if (strcmp(key, "MemAvailable") == 0)
			m->available = v;
		else if (strcmp(key, "Cached") == 0)
			m->cached = v;
		else if (strcmp(key, "SwapTotal") == 0)
			m->swap_total = v;
		else if (strcmp(key, "SwapFree") == 0)
			m->swap_free = v;
	}
	fclose(f);
}

typedef struct {
	unsigned long long read, written, io_ms;
	char name[64];		/* the first disk */
	char model[128];
	guint64 capacity;
} DiskInfo;

/* Adds up the real disks (those with a device behind them: no loop or
 * RAM disks, no partitions) */
static void
sample_disk(DiskInfo *di)
{
	char line[512], name[64], path[160];
	unsigned long long f[11];
	FILE *fp = fopen("/proc/diskstats", "r");

	memset(di, 0, sizeof *di);
	if (!fp)
		return;
	while (fgets(line, sizeof line, fp)) {
		if (sscanf(line, "%*u %*u %63s %llu %llu %llu %llu %llu %llu "
			   "%llu %llu %llu %llu %llu", name, &f[0], &f[1], &f[2],
			   &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9],
			   &f[10]) < 11)
			continue;
		snprintf(path, sizeof path, "/sys/block/%s/device", name);
		if (access(path, F_OK) != 0)
			continue;
		di->read += f[2] * 512;
		di->written += f[6] * 512;
		di->io_ms += f[9];
		if (!di->name[0]) {
			char *s;

			g_strlcpy(di->name, name, sizeof di->name);
			snprintf(path, sizeof path, "/sys/block/%s/device/model",
				 name);
			s = read_file(path);
			if (s) {
				g_strlcpy(di->model, g_strstrip(s),
					  sizeof di->model);
				g_free(s);
			}
			snprintf(path, sizeof path, "/sys/block/%s/size", name);
			s = read_file(path);
			if (s) {
				di->capacity = g_ascii_strtoull(s, NULL, 10) * 512;
				g_free(s);
			}
		}
	}
	fclose(fp);
}

typedef struct {
	unsigned long long rx, tx;
	char adapter[IF_NAMESIZE + 1];
	char ipv4[INET_ADDRSTRLEN];
} NetInfo;

static void
sample_net(NetInfo *ni)
{
	char line[512], name[64];
	unsigned long long rx, tx;
	struct ifaddrs *ifs, *i;
	FILE *f = fopen("/proc/net/dev", "r");

	memset(ni, 0, sizeof *ni);
	if (f) {
		while (fgets(line, sizeof line, f)) {
			if (sscanf(line, " %63[^:]: %llu %*u %*u %*u %*u %*u "
				   "%*u %*u %llu", name, &rx, &tx) != 3)
				continue;
			if (strcmp(name, "lo") == 0)
				continue;
			ni->rx += rx;
			ni->tx += tx;
		}
		fclose(f);
	}

	/* the adapter in use: the first one up with an IPv4 address */
	if (getifaddrs(&ifs) == 0) {
		for (i = ifs; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
			    (i->ifa_flags & IFF_LOOPBACK) ||
			    !(i->ifa_flags & IFF_UP))
				continue;
			g_strlcpy(ni->adapter, i->ifa_name, sizeof ni->adapter);
			inet_ntop(AF_INET,
				  &((struct sockaddr_in *) i->ifa_addr)->sin_addr,
				  ni->ipv4, sizeof ni->ipv4);
			break;
		}
		freeifaddrs(ifs);
	}
}

static char *
uptime_text(void)
{
	char *s = read_file("/proc/uptime");
	unsigned long t;

	if (!s)
		return g_strdup("—");
	t = (unsigned long) g_ascii_strtod(s, NULL);
	g_free(s);
	return g_strdup_printf("%lu:%02lu:%02lu:%02lu", t / 86400,
			       t / 3600 % 24, t / 60 % 60, t % 60);
}

/*
 * The once-a-second refresh
 */

static void refresh_apps(App *a);
static void refresh_services(App *a);

static unsigned long long
delta(unsigned long long now, unsigned long long before)
{
	return now >= before ? now - before : 0;	/* counters can reset */
}

static gboolean
refresh(gpointer data)
{
	App *a = data;
	gint64 now = g_get_monotonic_time();
	double secs = a->have_prev ? (now - a->last_time) / 1e6 : 1;
	double cpu, mhz, used_pct, rd = 0, wr = 0, active = 0, rx = 0, tx = 0;
	unsigned long long dtotal;
	int nprocs, nthreads, i;
	MemInfo m;
	DiskInfo di;
	NetInfo ni;
	guint64 used;
	char *s, *t;

	if (secs <= 0)
		secs = 1;

	sample_cpu(a, &cpu, &dtotal);
	mhz = cpu_mhz(a);
	sample_mem(&m);
	a->mem_total = m.total;
	scan_processes(a, dtotal, &nprocs, &nthreads);
	refresh_apps(a);
	refresh_services(a);
	sample_disk(&di);
	sample_net(&ni);

	if (a->have_prev) {
		rd = delta(di.read, a->disk_read) / secs;
		wr = delta(di.written, a->disk_write) / secs;
		active = delta(di.io_ms, a->disk_io_ms) / (secs * 10.0);
		if (active > 100)
			active = 100;
		rx = delta(ni.rx, a->net_rx) / secs;
		tx = delta(ni.tx, a->net_tx) / secs;
	}
	a->disk_read = di.read;
	a->disk_write = di.written;
	a->disk_io_ms = di.io_ms;
	a->net_rx = ni.rx;
	a->net_tx = ni.tx;
	a->have_prev = TRUE;
	a->last_time = now;

	used = m.total > m.available ? m.total - m.available : 0;
	used_pct = m.total ? 100.0 * used / m.total : 0;

	/* CPU */
	{
		Res *r = &a->res[RES_CPU];

		push(r->hist, cpu);
		set_stat(r, 0, g_strdup_printf("%.0f%%", cpu));
		set_stat(r, 1, mhz > 0 ? g_strdup_printf("%.2f GHz", mhz / 1000)
				       : g_strdup("—"));
		set_stat(r, 2, g_strdup_printf("%d", nprocs));
		set_stat(r, 3, g_strdup_printf("%d", nthreads));
		set_stat(r, 4, uptime_text());
		set_stat(r, 5, g_strdup_printf("%d", a->ncpu));
		gtk_label_set_text(GTK_LABEL(r->subtitle),
				   a->cpu_model ? a->cpu_model : "");
		s = mhz > 0 ? g_strdup_printf("%.0f%%  %.2f GHz", cpu, mhz / 1000)
			    : g_strdup_printf("%.0f%%", cpu);
		gtk_label_set_text(GTK_LABEL(r->mini_value), s);
		g_free(s);
	}

	/* Memory */
	{
		Res *r = &a->res[RES_MEM];

		push(r->hist, used_pct);
		set_stat(r, 0, fmt_size(used));
		set_stat(r, 1, fmt_size(m.available));
		set_stat(r, 2, fmt_size(m.cached));
		set_stat(r, 3, fmt_size(m.total));
		if (m.swap_total) {
			s = fmt_size(m.swap_total - m.swap_free);
			t = fmt_size(m.swap_total);
			set_stat(r, 4, g_strdup_printf("%s of %s", s, t));
			g_free(s);
			g_free(t);
		} else {
			set_stat(r, 4, g_strdup("None"));
		}
		t = fmt_size(m.total);
		gtk_label_set_text(GTK_LABEL(r->subtitle), t);
		s = fmt_size(used);
		g_free(t);
		t = g_strdup_printf("%s (%.0f%%)", s, used_pct);
		gtk_label_set_text(GTK_LABEL(r->mini_value), t);
		g_free(s);
		g_free(t);
	}

	/* Disk */
	{
		Res *r = &a->res[RES_DISK];

		push(r->hist, active);
		set_stat(r, 0, g_strdup_printf("%.0f%%", active));
		set_stat(r, 1, fmt_rate(rd));
		set_stat(r, 2, fmt_rate(wr));
		set_stat(r, 3, di.capacity ? fmt_size(di.capacity)
					   : g_strdup("—"));
		set_stat(r, 4, g_strdup(di.name[0] ? di.name : "—"));
		gtk_label_set_text(GTK_LABEL(r->subtitle), di.model);
		s = g_strdup_printf("%s  %.0f%%", di.name[0] ? di.name : "",
				    active);
		gtk_label_set_text(GTK_LABEL(r->mini_value), g_strstrip(s));
		g_free(s);
	}

	/* Network */
	{
		Res *r = &a->res[RES_NET];
		double peak = 0;

		push(r->hist, rx);
		push(r->hist2, tx);
		for (i = 0; i < HISTORY; i++)
			peak = MAX(peak, MAX(r->hist[i], r->hist2[i]));
		r->scale = nice_scale(peak);
		set_stat(r, 0, fmt_rate(tx));
		set_stat(r, 1, fmt_rate(rx));
		set_stat(r, 2, g_strdup(ni.adapter[0] ? ni.adapter
						      : "Not connected"));
		set_stat(r, 3, g_strdup(ni.ipv4[0] ? ni.ipv4 : "—"));
		gtk_label_set_text(GTK_LABEL(r->subtitle),
				   !ni.adapter[0] ? "Not connected" :
				   ni.adapter[0] == 'w' ? "Wi-Fi" : "Ethernet");
		s = fmt_rate(r->scale);
		gtk_label_set_text(GTK_LABEL(r->scale_label), s);
		g_free(s);
		s = fmt_rate(tx);
		t = fmt_rate(rx);
		{
			char *line = g_strdup_printf("S: %s  R: %s", s, t);

			gtk_label_set_text(GTK_LABEL(r->mini_value), line);
			g_free(line);
		}
		g_free(s);
		g_free(t);
	}

	for (i = 0; i < N_RES; i++) {
		gtk_widget_queue_draw(a->res[i].mini);
		gtk_widget_queue_draw(a->res[i].graph);
	}

	s = uptime_text();
	t = g_strdup_printf("Processes: %d    CPU: %.0f%%    Memory: %.0f%%    "
			    "Up time: %s", nprocs, cpu, used_pct, s);
	gtk_label_set_text(GTK_LABEL(a->status), t);
	g_free(s);
	g_free(t);

	return G_SOURCE_CONTINUE;
}

/*
 * Processes tab
 */

/* Sends SIG to the processes; reports the first one that cannot be ended */
static void
end_pids(App *a, GArray *pids, const char *name, int sig)
{
	char *msg = NULL;
	guint i;

	for (i = 0; i < pids->len; i++) {
		int pid = g_array_index(pids, int, i);

		if (pid <= 0)
			continue;
		if (pid == 1) {
			show_error(a, "This process cannot be ended.",
				   "It is the first process of the system (init): "
				   "ending it would stop the computer.");
			return;
		}
		if (kill(pid, sig) == 0 || errno == ESRCH || msg)
			continue;
		if (errno == EPERM)
			msg = g_strdup_printf("You do not have permission to end "
					      "\"%s\" (PID %d). It belongs to "
					      "another user, such as the system.",
					      name, pid);
		else
			msg = g_strdup_printf("\"%s\" (PID %d): %s", name, pid,
					      g_strerror(errno));
	}
	if (msg) {
		show_error(a, "Could not end the task.", msg);
		g_free(msg);
	}
}

/* The selected row's processes: the row's own, or all of its group's */
static GArray *
selected_pids(GtkWidget *tree, int pid_col, int name_col, char **name)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
	GArray *pids = g_array_new(FALSE, FALSE, sizeof(int));
	GtkTreeModel *model;
	GtkTreeIter iter, child;
	int pid = 0;

	*name = NULL;
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return pids;
	gtk_tree_model_get(model, &iter, pid_col, &pid, name_col, name, -1);
	if (pid > 0) {
		g_array_append_val(pids, pid);
	} else if (gtk_tree_model_iter_children(model, &child, &iter)) {
		do {
			gtk_tree_model_get(model, &child, pid_col, &pid, -1);
			if (pid > 0)
				g_array_append_val(pids, pid);
		} while (gtk_tree_model_iter_next(model, &child));
	}
	return pids;
}

static void
end_selected(App *a, int sig)
{
	char *name;
	GArray *pids = selected_pids(a->tree, COL_PID, COL_NAME, &name);

	end_pids(a, pids, name ? name : "", sig);
	g_array_free(pids, TRUE);
	g_free(name);
}

static void
on_end_clicked(GtkWidget *w, App *a)
{
	end_selected(a, SIGTERM);
}

static void
on_kill_activate(GtkWidget *w, App *a)
{
	end_selected(a, SIGKILL);
}

static void
on_selection_changed(GtkTreeSelection *sel, App *a)
{
	gtk_widget_set_sensitive(a->end_btn,
		gtk_tree_selection_get_selected(sel, NULL, NULL));
}

static gboolean
on_tree_button(GtkWidget *w, GdkEventButton *ev, App *a)
{
	GtkTreePath *path;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(a->tree), ev->x, ev->y,
					   &path, NULL, NULL, NULL))
		return FALSE;
	gtk_tree_selection_select_path(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)), path);
	gtk_tree_path_free(path);
	gtk_menu_popup_at_pointer(GTK_MENU(a->menu), (GdkEvent *) ev);
	return TRUE;
}

static gboolean
on_tree_key(GtkWidget *w, GdkEventKey *ev, App *a)
{
	if (ev->keyval == GDK_KEY_Delete) {
		end_selected(a, (ev->state & GDK_SHIFT_MASK) ? SIGKILL : SIGTERM);
		return TRUE;
	}
	return FALSE;
}

static void
on_search_changed(GtkSearchEntry *e, App *a)
{
	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(a->filter));
	if (gtk_entry_get_text(GTK_ENTRY(e))[0])
		gtk_tree_view_expand_all(GTK_TREE_VIEW(a->tree));
}

static gboolean
row_matches(GtkTreeModel *model, GtkTreeIter *iter, const char *text)
{
	char *name = NULL, *user = NULL, *needle, *hay, pidbuf[16];
	gboolean ok;
	int pid;

	gtk_tree_model_get(model, iter, COL_NAME, &name, COL_PID, &pid,
			   COL_USER, &user, -1);
	if (!name) {		/* a row being added */
		g_free(user);
		return FALSE;
	}
	needle = g_utf8_casefold(text, -1);
	hay = g_utf8_casefold(name, -1);
	snprintf(pidbuf, sizeof pidbuf, "%d", pid);
	ok = strstr(hay, needle) || (pid > 0 && g_str_has_prefix(pidbuf, text)) ||
	     (user && g_ascii_strcasecmp(user, text) == 0);
	g_free(hay);
	g_free(needle);
	g_free(name);
	g_free(user);
	return ok;
}

/* A group is shown when it or one of its processes matches; a process
 * when it or its group does */
static gboolean
row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	App *a = data;
	const char *text = gtk_entry_get_text(GTK_ENTRY(a->search));
	GtkTreeIter other;

	if (!text[0] || row_matches(model, iter, text))
		return TRUE;
	if (gtk_tree_model_iter_parent(model, &other, iter))
		return row_matches(model, &other, text);
	if (gtk_tree_model_iter_children(model, &other, iter)) {
		do {
			if (row_matches(model, &other, text))
				return TRUE;
		} while (gtk_tree_model_iter_next(model, &other));
	}
	return FALSE;
}

static gint
compare_names(GtkTreeModel *model, GtkTreeIter *x, GtkTreeIter *y,
	      gpointer data)
{
	char *a = NULL, *b = NULL;
	gint r;

	gtk_tree_model_get(model, x, COL_NAME, &a, -1);
	gtk_tree_model_get(model, y, COL_NAME, &b, -1);
	r = g_ascii_strcasecmp(a ? a : "", b ? b : "");
	g_free(a);
	g_free(b);
	return r;
}

/* Like on Windows, busier cells get a warmer background */
static void
set_heat(GtkCellRenderer *cell, double frac)
{
	GdkRGBA c = { 0.95, 0.70, 0.15, 0 };

	if (frac > 0.005) {
		c.alpha = 0.08 + frac * 0.6;
		if (c.alpha > 0.55)
			c.alpha = 0.55;
		g_object_set(cell, "cell-background-rgba", &c, NULL);
	} else {
		g_object_set(cell, "cell-background-set", FALSE, NULL);
	}
}

static void
cpu_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
	 GtkTreeIter *iter, gpointer data)
{
	char buf[32];
	double v = 0;

	gtk_tree_model_get(model, iter, COL_CPU, &v, -1);
	snprintf(buf, sizeof buf, "%.1f%%", v);
	g_object_set(cell, "text", buf, NULL);
	set_heat(cell, v / 100);
}

/* Group rows have no PID of their own */
static void
pid_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
	 GtkTreeIter *iter, gpointer data)
{
	char buf[16] = "";
	int pid = 0;

	gtk_tree_model_get(model, iter, COL_PID, &pid, -1);
	if (pid > 0)
		snprintf(buf, sizeof buf, "%d", pid);
	g_object_set(cell, "text", buf, NULL);
}

static void
mem_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
	 GtkTreeIter *iter, gpointer data)
{
	App *a = data;
	guint64 v = 0;
	char *s;

	gtk_tree_model_get(model, iter, COL_MEM, &v, -1);
	s = fmt_size(v);
	g_object_set(cell, "text", s, NULL);
	g_free(s);
	set_heat(cell, a->mem_total ? (double) v / a->mem_total * 4 : 0);
}

static GtkTreeViewColumn *text_column(GtkWidget *tree, const char *title,
	int col, gboolean right, GtkTreeCellDataFunc func, gpointer data,
	gboolean expand);

static void
add_column(App *a, const char *title, int col, gboolean right,
	   GtkTreeCellDataFunc func, gboolean expand)
{
	text_column(a->tree, title, col, right, func, a, expand);
}

static void
on_run_task(GtkWidget *w, App *a)
{
	GtkWidget *d, *box, *label, *entry;
	GError *err = NULL;

	d = gtk_dialog_new_with_buttons("Run new task", GTK_WINDOW(a->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Run", GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	label = gtk_label_new("Type the name of a program to start, "
			      "for example: firefox, foot or code");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_widget_show_all(d);

	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		char *cmd = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));

		gtk_widget_hide(d);
		if (cmd[0] && !g_spawn_command_line_async(cmd, &err)) {
			show_error(a, "Could not start the program.",
				   err->message);
			g_error_free(err);
		}
		g_free(cmd);
	}
	gtk_widget_destroy(d);
}

static GtkWidget *
build_processes(App *a)
{
	GtkWidget *box, *bar, *run, *scroll, *item;
	GtkTreeSelection *sel;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	g_object_set(bar, "margin", 8, NULL);
	a->search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(a->search),
				       "Search by name, PID or user");
	gtk_entry_set_width_chars(GTK_ENTRY(a->search), 32);
	g_signal_connect(a->search, "search-changed",
			 G_CALLBACK(on_search_changed), a);
	gtk_box_pack_start(GTK_BOX(bar), a->search, FALSE, FALSE, 0);

	a->end_btn = gtk_button_new_with_label("End task");
	gtk_style_context_add_class(gtk_widget_get_style_context(a->end_btn),
				    "destructive-action");
	gtk_widget_set_sensitive(a->end_btn, FALSE);
	gtk_widget_set_tooltip_text(a->end_btn,
		"Asks the program to close (Delete). "
		"Shift+Delete forces it to stop. "
		"On a group, ends all of its processes.");
	g_signal_connect(a->end_btn, "clicked", G_CALLBACK(on_end_clicked), a);
	gtk_box_pack_end(GTK_BOX(bar), a->end_btn, FALSE, FALSE, 0);

	run = gtk_button_new_with_label("Run new task");
	g_signal_connect(run, "clicked", G_CALLBACK(on_run_task), a);
	gtk_box_pack_end(GTK_BOX(bar), run, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

	a->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT,
				      G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_DOUBLE, G_TYPE_UINT64);
	a->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(a->store), NULL);
	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(a->filter),
					       row_visible, a, NULL);
	a->sort = gtk_tree_model_sort_new_with_model(a->filter);
	gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(a->sort), COL_NAME,
					compare_names, NULL, NULL);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a->sort),
					     COL_NAME, GTK_SORT_ASCENDING);

	a->tree = gtk_tree_view_new_with_model(a->sort);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(a->tree), FALSE);
	add_column(a, "Name", COL_NAME, FALSE, NULL, TRUE);
	add_column(a, "PID", COL_PID, TRUE, pid_cell, FALSE);
	add_column(a, "User", COL_USER, FALSE, NULL, FALSE);
	add_column(a, "Status", COL_STATUS, FALSE, NULL, FALSE);
	add_column(a, "CPU", COL_CPU, TRUE, cpu_cell, FALSE);
	add_column(a, "Memory", COL_MEM, TRUE, mem_cell, FALSE);

	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree));
	g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), a);
	g_signal_connect(a->tree, "button-press-event",
			 G_CALLBACK(on_tree_button), a);
	g_signal_connect(a->tree, "key-press-event", G_CALLBACK(on_tree_key), a);

	/* right-click menu of a process */
	a->menu = gtk_menu_new();
	item = gtk_menu_item_new_with_label("End task");
	g_signal_connect(item, "activate", G_CALLBACK(on_end_clicked), a);
	gtk_menu_shell_append(GTK_MENU_SHELL(a->menu), item);
	item = gtk_menu_item_new_with_label("End task now (force)");
	g_signal_connect(item, "activate", G_CALLBACK(on_kill_activate), a);
	gtk_menu_shell_append(GTK_MENU_SHELL(a->menu), item);
	gtk_widget_show_all(a->menu);
	gtk_menu_attach_to_widget(GTK_MENU(a->menu), a->tree, NULL);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), a->tree);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	a->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(a->status), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(a->status),
				    "tm-status");
	gtk_box_pack_start(GTK_BOX(box), a->status, FALSE, FALSE, 0);
	return box;
}

/*
 * Apps tab
 */

static void
app_row_free(gpointer data)
{
	AppRow *r = data;

	g_free(r->name);
	g_hash_table_destroy(r->wins);
	g_free(r);
}

static GDesktopAppInfo *
desktop_file(const char *id)
{
	char *file = g_strdup_printf("%s.desktop", id);
	GDesktopAppInfo *info = g_desktop_app_info_new(file);

	g_free(file);
	return info;
}

/* The app's name and icon: from its .desktop file (by app id, then by
 * program name), else its window title */
static char *
app_info(const char *app_id, int pid, const char *title, GIcon **icon)
{
	GDesktopAppInfo *info = NULL;
	char *comm = NULL, *name;

	if (app_id[0]) {
		info = desktop_file(app_id);
		if (!info) {
			char *lower = g_ascii_strdown(app_id, -1);

			info = desktop_file(lower);
			g_free(lower);
		}
	}
	if (pid > 0) {
		char path[64];

		snprintf(path, sizeof path, "/proc/%d/comm", pid);
		comm = read_file(path);
		if (comm)
			g_strstrip(comm);
	}
	if (!info && comm && comm[0])
		info = desktop_file(comm);

	*icon = NULL;
	if (info) {
		GIcon *i = g_app_info_get_icon(G_APP_INFO(info));

		name = g_strdup(g_app_info_get_name(G_APP_INFO(info)));
		if (i)
			*icon = g_object_ref(i);
		g_object_unref(info);
	} else {
		name = g_strdup(title[0] ? title : app_id[0] ? app_id :
				comm && comm[0] ? comm : "Unknown app");
	}
	if (!*icon)
		*icon = g_themed_icon_new("application-x-executable");
	g_free(comm);
	return name;
}

static void
app_key(char *buf, gsize size, int pid, guint id)
{
	if (pid > 0)
		snprintf(buf, size, "p%d", pid);
	else
		snprintf(buf, size, "w%u", id);
}

/* Reads the desktop's windows and updates the Apps tab */
static void
refresh_apps(App *a)
{
	char *data = a->windows_path ? read_file(a->windows_path) : NULL;
	char **lines, **l, key[24], *text;
	GHashTableIter it, wit;
	gpointer k, value;
	int nwins = 0;

	g_hash_table_iter_init(&it, a->apps);
	while (g_hash_table_iter_next(&it, &k, &value)) {
		AppRow *r = value;

		r->seen = FALSE;
		r->nwins = 0;
		r->cpu = 0;
		r->mem = 0;
		g_hash_table_iter_init(&wit, r->wins);
		while (g_hash_table_iter_next(&wit, NULL, &value))
			((WinRow *) value)->seen = FALSE;
	}

	lines = g_strsplit(data ? data : "", "\n", -1);
	for (l = lines; *l; l++) {
		char **f = g_strsplit(*l, "\t", 5);
		const char *app_id, *title;
		AppRow *r;
		WinRow *w;
		guint id;
		int pid;

		if (g_strv_length(f) != 5) {
			g_strfreev(f);
			continue;
		}
		id = (guint) g_ascii_strtoull(f[0], NULL, 10);
		pid = atoi(f[1]);
		app_id = f[3];
		title = f[4];

		app_key(key, sizeof key, pid, id);
		r = g_hash_table_lookup(a->apps, key);
		if (!r) {
			GIcon *icon;

			r = g_new0(AppRow, 1);
			r->pid = pid;
			r->wins = g_hash_table_new_full(g_direct_hash,
							g_direct_equal, NULL,
							g_free);
			r->name = app_info(app_id, pid, title, &icon);
			gtk_tree_store_append(a->app_store, &r->iter, NULL);
			gtk_tree_store_set(a->app_store, &r->iter,
					   AC_ICON, icon, AC_PID, pid,
					   AC_WINID, 0, -1);
			g_object_unref(icon);
			g_hash_table_insert(a->apps, g_strdup(key), r);
		}
		r->seen = TRUE;
		r->nwins++;
		nwins++;

		w = g_hash_table_lookup(r->wins, GUINT_TO_POINTER(id));
		if (!w) {
			w = g_new0(WinRow, 1);
			gtk_tree_store_append(a->app_store, &w->iter, &r->iter);
			g_hash_table_insert(r->wins, GUINT_TO_POINTER(id), w);
		}
		w->seen = TRUE;
		gtk_tree_store_set(a->app_store, &w->iter,
				   AC_NAME, title[0] ? title : r->name,
				   AC_PID, pid, AC_WINID, id, -1);
		g_strfreev(f);
	}
	g_strfreev(lines);

	/* forget the windows and apps that closed */
	g_hash_table_iter_init(&it, a->apps);
	while (g_hash_table_iter_next(&it, &k, &value)) {
		AppRow *r = value;

		if (!r->seen) {
			gtk_tree_store_remove(a->app_store, &r->iter);
			g_hash_table_iter_remove(&it);
			continue;
		}
		g_hash_table_iter_init(&wit, r->wins);
		while (g_hash_table_iter_next(&wit, NULL, &value)) {
			WinRow *w = value;

			if (!w->seen) {
				gtk_tree_store_remove(a->app_store, &w->iter);
				g_hash_table_iter_remove(&wit);
			}
		}
	}

	/* each process counts for the nearest app above it (a browser's
	 * helpers, a Flatpak app inside its sandbox) */
	g_hash_table_iter_init(&it, a->procs);
	while (g_hash_table_iter_next(&it, &k, &value)) {
		Proc *p = value, *up = p;
		int depth;

		for (depth = 0; up && depth < 64; depth++) {
			AppRow *r;

			app_key(key, sizeof key, up->pid, 0);
			r = g_hash_table_lookup(a->apps, key);
			if (r) {
				r->cpu += p->cpu;
				r->mem += p->mem;
				break;
			}
			up = up->ppid > 0 ? g_hash_table_lookup(a->procs,
					GINT_TO_POINTER(up->ppid)) : NULL;
		}
	}

	g_hash_table_iter_init(&it, a->apps);
	while (g_hash_table_iter_next(&it, &k, &value)) {
		AppRow *r = value;

		text = r->nwins > 1 ? g_strdup_printf("%s (%d)", r->name, r->nwins)
				    : g_strdup(r->name);
		gtk_tree_store_set(a->app_store, &r->iter,
				   AC_NAME, text,
				   AC_CPU, MIN(r->cpu, 100.0),
				   AC_MEM, r->mem, -1);
		g_free(text);
	}

	if (!data)
		text = g_strdup("The desktop did not share its window list: "
				"restart the session after updating hexaOS.");
	else
		text = g_strdup_printf("Apps: %u    Windows: %d",
				       g_hash_table_size(a->apps), nwins);
	gtk_label_set_text(GTK_LABEL(a->app_status), text);
	g_free(text);
	g_free(data);
}

/* Window rows only show their title */
static gboolean
is_window_row(GtkTreeModel *model, GtkTreeIter *iter)
{
	guint id = 0;

	gtk_tree_model_get(model, iter, AC_WINID, &id, -1);
	return id != 0;
}

static void
app_cpu_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
	     GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	char buf[32] = "";
	double v = 0;

	if (!is_window_row(model, iter)) {
		gtk_tree_model_get(model, iter, AC_CPU, &v, -1);
		snprintf(buf, sizeof buf, "%.1f%%", v);
	}
	g_object_set(cell, "text", buf, NULL);
	set_heat(cell, v / 100);
}

static void
app_mem_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
	     GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	App *a = data;
	guint64 v = 0;
	char *s;

	if (is_window_row(model, iter)) {
		g_object_set(cell, "text", "", NULL);
		set_heat(cell, 0);
		return;
	}
	gtk_tree_model_get(model, iter, AC_MEM, &v, -1);
	s = fmt_size(v);
	g_object_set(cell, "text", s, NULL);
	g_free(s);
	set_heat(cell, a->mem_total ? (double) v / a->mem_total * 4 : 0);
}

static void
app_pid_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
	     GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	char buf[16] = "";
	int pid = 0;

	if (!is_window_row(model, iter))
		gtk_tree_model_get(model, iter, AC_PID, &pid, -1);
	if (pid > 0)
		snprintf(buf, sizeof buf, "%d", pid);
	g_object_set(cell, "text", buf, NULL);
}

static int
app_selected_pid(App *a, char **name)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(a->app_tree));
	GtkTreeModel *model;
	GtkTreeIter iter, parent;
	int pid = 0;

	*name = NULL;
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return 0;
	if (gtk_tree_model_iter_parent(model, &parent, &iter))
		iter = parent;	/* a window: its app */
	gtk_tree_model_get(model, &iter, AC_PID, &pid, AC_NAME, name, -1);
	return pid;
}

static void
app_end(App *a, int sig)
{
	GArray *pids = g_array_new(FALSE, FALSE, sizeof(int));
	char *name;
	int pid = app_selected_pid(a, &name);

	if (pid > 0) {
		g_array_append_val(pids, pid);
		end_pids(a, pids, name, sig);
	}
	g_array_free(pids, TRUE);
	g_free(name);
}

static void
on_app_end_clicked(GtkWidget *w, App *a)
{
	app_end(a, SIGTERM);
}

static void
on_app_kill_activate(GtkWidget *w, App *a)
{
	app_end(a, SIGKILL);
}

/* "Go to details": the app's processes in the Processes tab */
static void
on_app_details_activate(GtkWidget *w, App *a)
{
	char *name, buf[16];
	int pid = app_selected_pid(a, &name);

	g_free(name);
	if (pid <= 0)
		return;
	snprintf(buf, sizeof buf, "%d", pid);
	gtk_stack_set_visible_child_name(GTK_STACK(a->stack), "processes");
	gtk_entry_set_text(GTK_ENTRY(a->search), buf);
	gtk_widget_grab_focus(a->tree);
}

static void
on_app_selection_changed(GtkTreeSelection *sel, App *a)
{
	char *name;
	int pid = app_selected_pid(a, &name);

	g_free(name);
	gtk_widget_set_sensitive(a->app_end_btn, pid > 0);
	gtk_widget_set_tooltip_text(a->app_end_btn,
		pid > 0 || !gtk_tree_selection_get_selected(sel, NULL, NULL) ?
		"Asks the app to close (Delete). Shift+Delete forces it to stop." :
		"The process of this window is not known, so it cannot be "
		"ended from here: close the window instead.");
}

static gboolean
on_app_tree_button(GtkWidget *w, GdkEventButton *ev, App *a)
{
	GtkTreePath *path;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(a->app_tree), ev->x,
					   ev->y, &path, NULL, NULL, NULL))
		return FALSE;
	gtk_tree_selection_select_path(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(a->app_tree)), path);
	gtk_tree_path_free(path);
	if (gtk_widget_get_sensitive(a->app_end_btn))
		gtk_menu_popup_at_pointer(GTK_MENU(a->app_menu), (GdkEvent *) ev);
	return TRUE;
}

static gboolean
on_app_tree_key(GtkWidget *w, GdkEventKey *ev, App *a)
{
	if (ev->keyval == GDK_KEY_Delete) {
		app_end(a, (ev->state & GDK_SHIFT_MASK) ? SIGKILL : SIGTERM);
		return TRUE;
	}
	return FALSE;
}

static GtkTreeViewColumn *
text_column(GtkWidget *tree, const char *title, int col, gboolean right,
	    GtkTreeCellDataFunc func, gpointer data, gboolean expand)
{
	GtkCellRenderer *cell = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *c = gtk_tree_view_column_new();

	if (right)
		g_object_set(cell, "xalign", 1.0, NULL);
	else
		g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_column_set_title(c, title);
	gtk_tree_view_column_pack_start(c, cell, TRUE);
	if (func)
		gtk_tree_view_column_set_cell_data_func(c, cell, func, data, NULL);
	else
		gtk_tree_view_column_add_attribute(c, cell, "text", col);
	gtk_tree_view_column_set_sort_column_id(c, col);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_expand(c, expand);
	if (right) {
		gtk_tree_view_column_set_alignment(c, 1.0);
		gtk_tree_view_column_set_min_width(c, 90);
	} else if (!expand) {
		gtk_tree_view_column_set_min_width(c, 110);
	}
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
	return c;
}

static GtkWidget *
scrolled(GtkWidget *child)
{
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), child);
	gtk_widget_set_vexpand(scroll, TRUE);
	return scroll;
}

static GtkWidget *
status_label(void)
{
	GtkWidget *l = gtk_label_new("");

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(l),
				    "tm-status");
	return l;
}

static GtkWidget *
build_apps(App *a)
{
	GtkWidget *box, *bar, *run, *item, *hint;
	GtkCellRenderer *icon, *text;
	GtkTreeViewColumn *c;
	GtkTreeSelection *sel;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	g_object_set(bar, "margin", 8, NULL);
	hint = styled_label("The apps with windows on the desktop, with "
			    "all of their processes", "tm-sub", 0);
	gtk_box_pack_start(GTK_BOX(bar), hint, FALSE, FALSE, 0);
	a->app_end_btn = gtk_button_new_with_label("End task");
	gtk_style_context_add_class(gtk_widget_get_style_context(a->app_end_btn),
				    "destructive-action");
	gtk_widget_set_sensitive(a->app_end_btn, FALSE);
	g_signal_connect(a->app_end_btn, "clicked",
			 G_CALLBACK(on_app_end_clicked), a);
	gtk_box_pack_end(GTK_BOX(bar), a->app_end_btn, FALSE, FALSE, 0);
	run = gtk_button_new_with_label("Run new task");
	g_signal_connect(run, "clicked", G_CALLBACK(on_run_task), a);
	gtk_box_pack_end(GTK_BOX(bar), run, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

	a->app_store = gtk_tree_store_new(N_ACOLS, G_TYPE_ICON, G_TYPE_STRING,
					  G_TYPE_INT, G_TYPE_DOUBLE,
					  G_TYPE_UINT64, G_TYPE_UINT);
	a->app_sort = gtk_tree_model_sort_new_with_model(
		GTK_TREE_MODEL(a->app_store));
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a->app_sort),
					     AC_NAME, GTK_SORT_ASCENDING);
	a->app_tree = gtk_tree_view_new_with_model(a->app_sort);

	/* Name: icon and text */
	c = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(c, "Name");
	icon = gtk_cell_renderer_pixbuf_new();
	g_object_set(icon, "stock-size", GTK_ICON_SIZE_LARGE_TOOLBAR, NULL);
	gtk_tree_view_column_pack_start(c, icon, FALSE);
	gtk_tree_view_column_add_attribute(c, icon, "gicon", AC_ICON);
	text = gtk_cell_renderer_text_new();
	g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_column_pack_start(c, text, TRUE);
	gtk_tree_view_column_add_attribute(c, text, "text", AC_NAME);
	gtk_tree_view_column_set_sort_column_id(c, AC_NAME);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_expand(c, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(a->app_tree), c);

	text_column(a->app_tree, "PID", AC_PID, TRUE, app_pid_cell, a, FALSE);
	text_column(a->app_tree, "CPU", AC_CPU, TRUE, app_cpu_cell, a, FALSE);
	text_column(a->app_tree, "Memory", AC_MEM, TRUE, app_mem_cell, a,
		    FALSE);

	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->app_tree));
	g_signal_connect(sel, "changed", G_CALLBACK(on_app_selection_changed), a);
	g_signal_connect(a->app_tree, "button-press-event",
			 G_CALLBACK(on_app_tree_button), a);
	g_signal_connect(a->app_tree, "key-press-event",
			 G_CALLBACK(on_app_tree_key), a);

	a->app_menu = gtk_menu_new();
	item = gtk_menu_item_new_with_label("End task");
	g_signal_connect(item, "activate", G_CALLBACK(on_app_end_clicked), a);
	gtk_menu_shell_append(GTK_MENU_SHELL(a->app_menu), item);
	item = gtk_menu_item_new_with_label("End task now (force)");
	g_signal_connect(item, "activate", G_CALLBACK(on_app_kill_activate), a);
	gtk_menu_shell_append(GTK_MENU_SHELL(a->app_menu), item);
	gtk_menu_shell_append(GTK_MENU_SHELL(a->app_menu),
			      gtk_separator_menu_item_new());
	item = gtk_menu_item_new_with_label("Go to details");
	g_signal_connect(item, "activate",
			 G_CALLBACK(on_app_details_activate), a);
	gtk_menu_shell_append(GTK_MENU_SHELL(a->app_menu), item);
	gtk_widget_show_all(a->app_menu);
	gtk_menu_attach_to_widget(GTK_MENU(a->app_menu), a->app_tree, NULL);

	gtk_box_pack_start(GTK_BOX(box), scrolled(a->app_tree), TRUE, TRUE, 0);
	a->app_status = status_label();
	gtk_box_pack_start(GTK_BOX(box), a->app_status, FALSE, FALSE, 0);
	return box;
}

/*
 * Services tab
 */

/* Friendlier descriptions of the services hexaOS has */
static const struct {
	const char *name, *desc;
} known_services[] = {
	{ "psplash",	"Boot splash screen" },
	{ "keymap",	"Keyboard layout" },
	{ "seedrng",	"Random number generator seed" },
	{ "syslogd",	"System log" },
	{ "klogd",	"Kernel log" },
	{ "growfs",	"Grows the system partition to fill the disk" },
	{ "sysctl",	"Kernel settings" },
	{ "udevd",	"Device manager (udev)" },
	{ "modules",	"Loads kernel modules" },
	{ "speakers",	"Turns on the laptop speakers (MAX98390)" },
	{ "cgroupfs2",	"Control groups (used by containers)" },
	{ "dbus-daemon", "Message bus (D-Bus)" },
	{ "dbus",	"Message bus (D-Bus)" },
	{ "iptables",	"Firewall rules" },
	{ "iwd",	"Wi-Fi (iwd)" },
	{ "network",	"Network interfaces" },
	{ "wired",	"Wired network (DHCP)" },
	{ "chronyd",	"Clock synchronization (chrony)" },
	{ "crond",	"Scheduled tasks (cron)" },
	{ "seatd",	"Seat manager: gives the desktop the screen and input" },
	{ "cupsd",	"Printing (CUPS)" },
	{ "dockerd",	"Docker containers" },
	{ "fuse3",	"FUSE file systems" },
};

/* Stopping these ends the desktop session or breaks the system */
static gboolean
service_is_critical(const char *name)
{
	return strcmp(name, "seatd") == 0 || strcmp(name, "dbus-daemon") == 0 ||
	       strcmp(name, "dbus") == 0 || strcmp(name, "udevd") == 0;
}

static char *
unquote(const char *v)
{
	char *s = g_strstrip(g_strdup(v));
	gsize n = strlen(s);

	if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = 0;
	}
	return s;
}

/* Replaces $DAEMON and ${DAEMON} */
static char *
expand_daemon(const char *v, const char *daemon)
{
	char **parts, *a, *b;

	if (!daemon)
		return g_strdup(v);
	parts = g_strsplit(v, "${DAEMON}", -1);
	a = g_strjoinv(daemon, parts);
	g_strfreev(parts);
	parts = g_strsplit(a, "$DAEMON", -1);
	b = g_strjoinv(daemon, parts);
	g_strfreev(parts);
	g_free(a);
	return b;
}

/* What an init script runs: DAEMON=, PIDFILE= and a description (from
 * the LSB header, else the first comment line) */
static void
parse_init_script(const char *path, char **daemon, char **pidfile,
		  char **desc)
{
	char *data = read_file(path), **lines, **l, *raw_pidfile = NULL;
	int n = 0;

	*daemon = *pidfile = *desc = NULL;
	if (!data)
		return;
	lines = g_strsplit(data, "\n", -1);
	g_free(data);
	for (l = lines; *l; l++, n++) {
		const char *line = *l, *c;

		if (!*daemon && g_str_has_prefix(line, "DAEMON="))
			*daemon = unquote(line + 7);
		else if (!raw_pidfile && g_str_has_prefix(line, "PIDFILE="))
			raw_pidfile = unquote(line + 8);
		else if ((c = strstr(line, "Short-Description:")))
			g_free(*desc), *desc = g_strstrip(g_strdup(c + 18));
		else if (!*desc && n > 0 && n < 6 && g_str_has_prefix(line, "# ") &&
			 !strstr(line, "shellcheck") && !strstr(line, "###")) {
			/* the first sentence */
			const char *dot = strstr(line + 2, ". ");

			*desc = dot ? g_strndup(line + 2, dot - line - 2)
				    : g_strdup(line + 2);
			g_strstrip(*desc);
			if (g_str_has_suffix(*desc, "."))
				(*desc)[strlen(*desc) - 1] = 0;
		}
	}
	g_strfreev(lines);
	if (*daemon && !**daemon)
		g_free(*daemon), *daemon = NULL;
	if (raw_pidfile) {
		*pidfile = expand_daemon(raw_pidfile, *daemon);
		g_free(raw_pidfile);
	}
}

static int
pid_from_file(const char *path)
{
	char *s = read_file(path), proc[32];
	int pid;

	if (!s)
		return 0;
	pid = atoi(s);
	g_free(s);
	snprintf(proc, sizeof proc, "/proc/%d", pid);
	return pid > 0 && access(proc, F_OK) == 0 ? pid : 0;
}

typedef struct {
	char *script;
	gboolean autostart;
} SvcScan;

static void
svc_scan_free(gpointer data)
{
	SvcScan *sc = data;

	g_free(sc->script);
	g_free(sc);
}

static void
svc_row_free(gpointer data)
{
	SvcRow *r = data;

	g_free(r->busy);
	g_free(r);
}

static void update_svc_buttons(App *a);

/* Lists /etc/init.d: SNNname runs at startup, a plain name only on demand */
static void
refresh_services(App *a)
{
	GHashTable *found = g_hash_table_new_full(g_str_hash, g_str_equal,
						  g_free, svc_scan_free);
	GHashTableIter it;
	gpointer key, value;
	struct dirent *de;
	int running = 0;
	char *text;
	DIR *d;

	d = opendir(INIT_DIR);
	while (d && (de = readdir(d))) {
		const char *n = de->d_name, *name;
		gboolean at_boot;
		struct stat sb;
		SvcScan *sc;
		char *path;

		if (n[0] == '.' || strcmp(n, "rcS") == 0 || strcmp(n, "rcK") == 0)
			continue;
		if (n[0] == 'K' && isdigit((unsigned char) n[1]) &&
		    isdigit((unsigned char) n[2]))
			continue;	/* kill scripts */
		path = g_build_filename(INIT_DIR, n, NULL);
		if (stat(path, &sb) < 0 || !S_ISREG(sb.st_mode) ||
		    access(path, X_OK) != 0) {
			g_free(path);
			continue;
		}
		at_boot = n[0] == 'S' && isdigit((unsigned char) n[1]) &&
			  isdigit((unsigned char) n[2]) && n[3];
		name = at_boot ? n + 3 : n;
		sc = g_hash_table_lookup(found, name);
		if (!sc) {
			sc = g_new0(SvcScan, 1);
			sc->script = path;
			g_hash_table_insert(found, g_strdup(name), sc);
		} else if (!at_boot) {	/* S60dockerd -> dockerd */
			g_free(sc->script);
			sc->script = path;
		} else {
			g_free(path);
		}
		if (at_boot)
			sc->autostart = TRUE;
	}
	if (d)
		closedir(d);

	g_hash_table_iter_init(&it, a->svcs);
	while (g_hash_table_iter_next(&it, &key, &value))
		((SvcRow *) value)->seen = FALSE;

	g_hash_table_iter_init(&it, found);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		const char *name = key, *status;
		SvcScan *sc = value;
		SvcRow *r = g_hash_table_lookup(a->svcs, name);
		char *daemon, *pidfile, *desc;
		int pid = 0, state;
		guint i;

		parse_init_script(sc->script, &daemon, &pidfile, &desc);
		for (i = 0; i < G_N_ELEMENTS(known_services); i++)
			if (strcmp(known_services[i].name, name) == 0) {
				g_free(desc);
				desc = g_strdup(known_services[i].desc);
			}

		if (pidfile)
			pid = pid_from_file(pidfile);
		if (!pid && daemon)
			pid = GPOINTER_TO_INT(g_hash_table_lookup(a->comms,
								  daemon));
		if (pid)
			state = SVC_RUNNING;
		else if (pidfile || daemon)
			state = SVC_STOPPED;
		else
			state = SVC_ONESHOT;	/* does its job at startup */
		if (state == SVC_RUNNING)
			running++;

		if (!r) {
			r = g_new0(SvcRow, 1);
			gtk_list_store_append(a->svc_store, &r->iter);
			g_hash_table_insert(a->svcs, g_strdup(name), r);
		}
		r->seen = TRUE;
		status = r->busy ? r->busy :
			 state == SVC_RUNNING ? "Running" :
			 state == SVC_STOPPED ? "Stopped" : "Startup task";
		gtk_list_store_set(a->svc_store, &r->iter,
				   SC_NAME, name,
				   SC_DESC, desc ? desc : "",
				   SC_STATUS, status,
				   SC_PID, pid,
				   SC_STARTUP, sc->autostart ? "Automatic" : "Manual",
				   SC_STATE, state,
				   -1);
		g_free(daemon);
		g_free(pidfile);
		g_free(desc);
	}

	g_hash_table_iter_init(&it, a->svcs);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		SvcRow *r = value;

		if (!r->seen && !r->busy) {
			gtk_list_store_remove(a->svc_store, &r->iter);
			g_hash_table_iter_remove(&it);
		}
	}

	text = g_strdup_printf("Services: %u    Running: %d",
			       g_hash_table_size(found), running);
	gtk_label_set_text(GTK_LABEL(a->svc_status), text);
	g_free(text);
	g_hash_table_destroy(found);
	update_svc_buttons(a);
}

/* The selected service: its name (to free), state and row */
static char *
svc_selected(App *a, int *state, SvcRow **row)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(a->svc_tree));
	GtkTreeModel *model;
	GtkTreeIter iter;
	char *name = NULL;

	*state = SVC_STOPPED;
	if (row)
		*row = NULL;
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return NULL;
	gtk_tree_model_get(model, &iter, SC_NAME, &name, SC_STATE, state, -1);
	if (row && name)
		*row = g_hash_table_lookup(a->svcs, name);
	return name;
}

static void
update_svc_buttons(App *a)
{
	SvcRow *row;
	int state;
	char *name = svc_selected(a, &state, &row);
	gboolean ok = name && row && !row->busy;

	gtk_widget_set_sensitive(a->svc_start, ok && state != SVC_RUNNING);
	gtk_widget_set_sensitive(a->svc_stop, ok && state != SVC_STOPPED);
	gtk_widget_set_sensitive(a->svc_restart, ok);
	g_free(name);
}

static char *
ask_password(App *a, const char *text)
{
	GtkWidget *d, *box, *label, *entry;
	char *pw = NULL;

	d = gtk_dialog_new_with_buttons("Administrator permission",
		GTK_WINDOW(a->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
	entry = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(d);
	return pw;
}

/* Checks the password with sudo (sudo -v) */
static gboolean
password_ok(const char *pw)
{
	const char *argv[] = { "sudo", "-k", "-S", "-p", "", "-v", NULL };
	GSubprocess *p;
	char *line = g_strdup_printf("%s\n", pw);
	gboolean ok;

	p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
			      G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
			      G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	ok = p && g_subprocess_communicate_utf8(p, line, NULL, NULL, NULL, NULL) &&
	     g_subprocess_get_successful(p);
	if (p)
		g_object_unref(p);
	memset(line, 0, strlen(line));
	g_free(line);
	return ok;
}

/* FALSE if the user cancelled */
static gboolean
get_admin(App *a)
{
	const char *argv[] = { "sudo", "-n", "true", NULL };
	const char *text = "Starting and stopping services affects the whole "
			   "computer. Type your password to allow it.";
	GSubprocess *p;

	if (getuid() == 0 || a->sudo_nopass || a->admin_pw)
		return TRUE;
	/* sudo may need no password (NOPASSWD, e.g. after hexa-dev) */
	p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
			      G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	if (p && g_subprocess_wait(p, NULL, NULL) &&
	    g_subprocess_get_successful(p))
		a->sudo_nopass = TRUE;
	if (p)
		g_object_unref(p);
	if (a->sudo_nopass)
		return TRUE;

	for (;;) {
		char *pw = ask_password(a, text);

		if (!pw)
			return FALSE;
		if (password_ok(pw)) {
			a->admin_pw = pw;
			return TRUE;
		}
		memset(pw, 0, strlen(pw));
		g_free(pw);
		text = "Wrong password, or this account may not change the "
		       "system. Try again.";
	}
}

typedef struct {
	App *a;
	char *name, *action;
} SvcJob;

static void
svc_done(GObject *src, GAsyncResult *res, gpointer data)
{
	SvcJob *j = data;
	App *a = j->a;
	SvcRow *r = g_hash_table_lookup(a->svcs, j->name);
	char *out = NULL;
	GError *err = NULL;
	gboolean ok;

	ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
						  NULL, &err) &&
	     g_subprocess_get_successful(G_SUBPROCESS(src));
	if (r) {
		g_free(r->busy);
		r->busy = NULL;
	}
	if (!ok) {
		char *title = g_strdup_printf("Could not %s the service \"%s\".",
					      j->action, j->name);

		if (out && (strstr(out, "incorrect password") ||
			    strstr(out, "password is required"))) {
			g_free(a->admin_pw);
			a->admin_pw = NULL;
			a->sudo_nopass = FALSE;
		}
		show_error(a, title, err ? err->message :
			   out && *g_strstrip(out) ? out : NULL);
		g_free(title);
	}
	refresh_services(a);
	g_clear_error(&err);
	g_free(out);
	g_object_unref(src);
	g_free(j->name);
	g_free(j->action);
	g_free(j);
}

static void
svc_run(App *a, const char *action)
{
	GPtrArray *argv;
	GSubprocess *p;
	GError *err = NULL;
	SvcRow *row;
	SvcJob *j;
	char *name, *input = NULL;
	int state;

	name = svc_selected(a, &state, &row);
	if (!name || !row || row->busy) {
		g_free(name);
		return;
	}
	if (strcmp(action, "start") != 0 && service_is_critical(name)) {
		GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->window),
			GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
			"%s \"%s\"?", strcmp(action, "stop") == 0 ?
			"Stop" : "Restart", name);
		int resp;

		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
			"The desktop depends on this service: the session may "
			"close, and unsaved work in open apps would be lost.");
		gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel",
				       GTK_RESPONSE_CANCEL, "_Continue",
				       GTK_RESPONSE_OK, NULL);
		resp = gtk_dialog_run(GTK_DIALOG(d));
		gtk_widget_destroy(d);
		if (resp != GTK_RESPONSE_OK) {
			g_free(name);
			return;
		}
	}
	if (!get_admin(a)) {
		g_free(name);
		return;
	}

	argv = g_ptr_array_new();
	if (getuid() != 0) {
		g_ptr_array_add(argv, "sudo");
		if (a->sudo_nopass) {
			g_ptr_array_add(argv, "-n");
		} else {
			g_ptr_array_add(argv, "-k");
			g_ptr_array_add(argv, "-S");
			g_ptr_array_add(argv, "-p");
			g_ptr_array_add(argv, "");
			input = g_strdup_printf("%s\n", a->admin_pw);
		}
	}
	g_ptr_array_add(argv, HELPER);
	g_ptr_array_add(argv, "service");
	g_ptr_array_add(argv, name);
	g_ptr_array_add(argv, (gpointer) action);
	g_ptr_array_add(argv, NULL);
	p = g_subprocess_newv((const char *const *) argv->pdata,
			      G_SUBPROCESS_FLAGS_STDIN_PIPE |
			      G_SUBPROCESS_FLAGS_STDOUT_PIPE |
			      G_SUBPROCESS_FLAGS_STDERR_MERGE, &err);
	g_ptr_array_free(argv, TRUE);
	if (!p) {
		show_error(a, "Could not run the service helper.", err->message);
		g_error_free(err);
		g_free(input);
		g_free(name);
		return;
	}

	row->busy = g_strdup(strcmp(action, "start") == 0 ? "Starting…" :
			     strcmp(action, "stop") == 0 ? "Stopping…" :
			     "Restarting…");
	gtk_list_store_set(a->svc_store, &row->iter, SC_STATUS, row->busy, -1);
	update_svc_buttons(a);

	j = g_new0(SvcJob, 1);
	j->a = a;
	j->name = name;
	j->action = g_strdup(action);
	g_subprocess_communicate_utf8_async(p, input, NULL, svc_done, j);
	if (input) {
		memset(input, 0, strlen(input));
		g_free(input);
	}
}

static void on_svc_start(GtkWidget *w, App *a) { svc_run(a, "start"); }
static void on_svc_stop(GtkWidget *w, App *a) { svc_run(a, "stop"); }
static void on_svc_restart(GtkWidget *w, App *a) { svc_run(a, "restart"); }

static void
on_svc_selection_changed(GtkTreeSelection *sel, App *a)
{
	update_svc_buttons(a);
}

static gboolean
on_svc_tree_button(GtkWidget *w, GdkEventButton *ev, App *a)
{
	GtkTreePath *path;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(a->svc_tree), ev->x,
					   ev->y, &path, NULL, NULL, NULL))
		return FALSE;
	gtk_tree_selection_select_path(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(a->svc_tree)), path);
	gtk_tree_path_free(path);
	gtk_menu_popup_at_pointer(GTK_MENU(a->svc_menu), (GdkEvent *) ev);
	return TRUE;
}

static void
svc_status_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
		GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	GdkRGBA green = { 0.35, 0.85, 0.50, 1 };
	char *status = NULL;
	int state = 0;

	gtk_tree_model_get(model, iter, SC_STATUS, &status, SC_STATE, &state, -1);
	g_object_set(cell, "text", status ? status : "", NULL);
	if (state == SVC_RUNNING)
		g_object_set(cell, "foreground-rgba", &green, NULL);
	else
		g_object_set(cell, "foreground-set", FALSE, NULL);
	g_free(status);
}

static void
svc_pid_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
	     GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	char buf[16] = "";
	int pid = 0;

	gtk_tree_model_get(model, iter, SC_PID, &pid, -1);
	if (pid > 0)
		snprintf(buf, sizeof buf, "%d", pid);
	g_object_set(cell, "text", buf, NULL);
}

/* The menu items follow the buttons' sensitivity */
static GtkWidget *
svc_menu_item(App *a, const char *label, GtkWidget *button, GCallback cb)
{
	GtkWidget *item = gtk_menu_item_new_with_label(label);

	g_signal_connect(item, "activate", cb, a);
	g_object_bind_property(button, "sensitive", item, "sensitive",
			       G_BINDING_SYNC_CREATE);
	return item;
}

static GtkWidget *
build_services(App *a)
{
	GtkWidget *box, *bar, *hint;
	GtkTreeSelection *sel;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	g_object_set(bar, "margin", 8, NULL);
	hint = styled_label("System services (/etc/init.d). Changing them "
			    "asks for your password.", "tm-sub", 0);
	gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(bar), hint, TRUE, TRUE, 0);
	a->svc_restart = gtk_button_new_with_label("Restart");
	g_signal_connect(a->svc_restart, "clicked", G_CALLBACK(on_svc_restart), a);
	gtk_box_pack_end(GTK_BOX(bar), a->svc_restart, FALSE, FALSE, 0);
	a->svc_stop = gtk_button_new_with_label("Stop");
	g_signal_connect(a->svc_stop, "clicked", G_CALLBACK(on_svc_stop), a);
	gtk_box_pack_end(GTK_BOX(bar), a->svc_stop, FALSE, FALSE, 0);
	a->svc_start = gtk_button_new_with_label("Start");
	g_signal_connect(a->svc_start, "clicked", G_CALLBACK(on_svc_start), a);
	gtk_box_pack_end(GTK_BOX(bar), a->svc_start, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

	a->svc_store = gtk_list_store_new(N_SCOLS, G_TYPE_STRING, G_TYPE_STRING,
					  G_TYPE_STRING, G_TYPE_INT,
					  G_TYPE_STRING, G_TYPE_INT);
	a->svc_sort = gtk_tree_model_sort_new_with_model(
		GTK_TREE_MODEL(a->svc_store));
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a->svc_sort),
					     SC_NAME, GTK_SORT_ASCENDING);
	a->svc_tree = gtk_tree_view_new_with_model(a->svc_sort);
	text_column(a->svc_tree, "Name", SC_NAME, FALSE, NULL, a, FALSE);
	text_column(a->svc_tree, "Description", SC_DESC, FALSE, NULL, a, TRUE);
	text_column(a->svc_tree, "Status", SC_STATUS, FALSE, svc_status_cell,
		    a, FALSE);
	text_column(a->svc_tree, "PID", SC_PID, TRUE, svc_pid_cell, a, FALSE);
	text_column(a->svc_tree, "Startup", SC_STARTUP, FALSE, NULL, a, FALSE);

	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->svc_tree));
	g_signal_connect(sel, "changed", G_CALLBACK(on_svc_selection_changed), a);
	g_signal_connect(a->svc_tree, "button-press-event",
			 G_CALLBACK(on_svc_tree_button), a);

	a->svc_menu = gtk_menu_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(a->svc_menu),
		svc_menu_item(a, "Start", a->svc_start, G_CALLBACK(on_svc_start)));
	gtk_menu_shell_append(GTK_MENU_SHELL(a->svc_menu),
		svc_menu_item(a, "Stop", a->svc_stop, G_CALLBACK(on_svc_stop)));
	gtk_menu_shell_append(GTK_MENU_SHELL(a->svc_menu),
		svc_menu_item(a, "Restart", a->svc_restart,
			      G_CALLBACK(on_svc_restart)));
	gtk_widget_show_all(a->svc_menu);
	gtk_menu_attach_to_widget(GTK_MENU(a->svc_menu), a->svc_tree, NULL);

	gtk_box_pack_start(GTK_BOX(box), scrolled(a->svc_tree), TRUE, TRUE, 0);
	a->svc_status = status_label();
	gtk_box_pack_start(GTK_BOX(box), a->svc_status, FALSE, FALSE, 0);
	update_svc_buttons(a);
	return box;
}

/*
 * Performance tab
 */

static void
draw_graph(cairo_t *cr, double w, double h, Res *r, gboolean big)
{
	double scale = r->max > 0 ? r->max : r->scale;
	int line, i;

	if (scale <= 0)
		scale = 1;

	cairo_set_source_rgb(cr, 0.067, 0.086, 0.122);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);

	cairo_set_line_width(cr, 1);
	if (big) {
		cairo_set_source_rgba(cr, r->r, r->g, r->b, 0.12);
		for (i = 1; i < 10; i++) {
			cairo_move_to(cr, (int) (w * i / 10) + 0.5, 0);
			cairo_line_to(cr, (int) (w * i / 10) + 0.5, h);
		}
		for (i = 1; i < 5; i++) {
			cairo_move_to(cr, 0, (int) (h * i / 5) + 0.5);
			cairo_line_to(cr, w, (int) (h * i / 5) + 0.5);
		}
		cairo_stroke(cr);
	}

	for (line = r->two ? 1 : 0; line >= 0; line--) {
		double *hist = line ? r->hist2 : r->hist;
		int pass;

		for (pass = 0; pass < 2; pass++) {
			if (line && pass == 0)
				continue;	/* the second line is not filled */
			cairo_new_path(cr);
			for (i = 0; i < HISTORY; i++) {
				double v = hist[i] / scale;
				double x = w * i / (HISTORY - 1);
				double y;

				v = v < 0 ? 0 : v > 1 ? 1 : v;
				y = h - 1 - v * (h - 2);
				if (i == 0)
					cairo_move_to(cr, x, y);
				else
					cairo_line_to(cr, x, y);
			}
			if (pass == 0) {
				cairo_line_to(cr, w, h);
				cairo_line_to(cr, 0, h);
				cairo_close_path(cr);
				cairo_set_source_rgba(cr, r->r, r->g, r->b, 0.18);
				cairo_fill(cr);
			} else {
				static const double dash[] = { 4, 3 };

				cairo_set_dash(cr, dash, line ? 2 : 0, 0);
				cairo_set_line_width(cr, big ? 1.5 : 1);
				cairo_set_source_rgba(cr, r->r, r->g, r->b, 1);
				cairo_stroke(cr);
				cairo_set_dash(cr, NULL, 0, 0);
			}
		}
	}

	cairo_set_line_width(cr, 1);
	cairo_set_source_rgba(cr, r->r, r->g, r->b, 0.7);
	cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
	cairo_stroke(cr);
}

static gboolean
on_draw_graph(GtkWidget *w, cairo_t *cr, Res *r)
{
	draw_graph(cr, gtk_widget_get_allocated_width(w),
		   gtk_widget_get_allocated_height(w), r, TRUE);
	return FALSE;
}

static gboolean
on_draw_mini(GtkWidget *w, cairo_t *cr, Res *r)
{
	draw_graph(cr, gtk_widget_get_allocated_width(w),
		   gtk_widget_get_allocated_height(w), r, FALSE);
	return FALSE;
}

static GtkWidget *
styled_label(const char *text, const char *css_class, float xalign)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), xalign);
	if (css_class)
		gtk_style_context_add_class(gtk_widget_get_style_context(l),
					    css_class);
	return l;
}

static void
add_stat(Res *r, GtkWidget *grid, const char *label)
{
	int i = r->nstats++;
	GtkWidget *l = styled_label(label, "tm-stat-label", 0);
	GtkWidget *v = styled_label("—", "tm-stat-value", 0);

	gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
	gtk_grid_attach(GTK_GRID(grid), l, i % 3, (i / 3) * 2, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v, i % 3, (i / 3) * 2 + 1, 1, 1);
	r->stat[i] = v;
}

static void
on_resource_selected(GtkListBox *list, GtkListBoxRow *row, App *a)
{
	int i = row ? gtk_list_box_row_get_index(row) : -1;

	if (i >= 0 && i < N_RES)
		gtk_stack_set_visible_child_name(GTK_STACK(a->perf_stack),
						 a->res[i].name);
}

static GtkWidget *
build_performance(App *a)
{
	static const char *stats[N_RES][MAX_STATS] = {
		{ "Utilization", "Speed", "Processes", "Threads", "Up time",
		  "Logical processors" },
		{ "In use", "Available", "Cached", "Total", "Swap in use" },
		{ "Active time", "Read speed", "Write speed", "Capacity",
		  "Device" },
		{ "Send", "Receive", "Adapter", "IPv4 address" },
	};
	GtkWidget *box, *list, *scroll;
	int i, j;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list),
					GTK_SELECTION_BROWSE);
	a->perf_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(a->perf_stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);

	for (i = 0; i < N_RES; i++) {
		Res *r = &a->res[i];
		GtkWidget *row, *text, *page, *head, *caps, *grid, *foot;

		/* sidebar entry: small graph, name and value */
		row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		g_object_set(row, "margin", 8, NULL);
		r->mini = gtk_drawing_area_new();
		gtk_widget_set_size_request(r->mini, 72, 46);
		g_signal_connect(r->mini, "draw", G_CALLBACK(on_draw_mini), r);
		gtk_box_pack_start(GTK_BOX(row), r->mini, FALSE, FALSE, 0);
		text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(text),
				   styled_label(r->name, "tm-res-name", 0),
				   FALSE, FALSE, 0);
		r->mini_value = styled_label("", "tm-sub", 0);
		gtk_label_set_ellipsize(GTK_LABEL(r->mini_value),
					PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(r->mini_value), 18);
		gtk_box_pack_start(GTK_BOX(text), r->mini_value, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);

		/* page: title, big graph, numbers */
		page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
		g_object_set(page, "margin", 16, NULL);

		head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		gtk_box_pack_start(GTK_BOX(head),
				   styled_label(r->name, "tm-title", 0),
				   FALSE, FALSE, 0);
		r->subtitle = styled_label("", "tm-sub", 1);
		gtk_label_set_ellipsize(GTK_LABEL(r->subtitle),
					PANGO_ELLIPSIZE_END);
		gtk_widget_set_valign(r->subtitle, GTK_ALIGN_END);
		gtk_box_pack_end(GTK_BOX(head), r->subtitle, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);

		caps = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_box_pack_start(GTK_BOX(caps),
				   styled_label(r->caption, "tm-caption", 0),
				   TRUE, TRUE, 0);
		r->scale_label = styled_label(r->max == 100 ? "100%" : "",
					      "tm-caption", 1);
		gtk_box_pack_end(GTK_BOX(caps), r->scale_label, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(page), caps, FALSE, FALSE, 0);

		r->graph = gtk_drawing_area_new();
		gtk_widget_set_size_request(r->graph, 320, 200);
		gtk_widget_set_vexpand(r->graph, TRUE);
		g_signal_connect(r->graph, "draw", G_CALLBACK(on_draw_graph), r);
		gtk_box_pack_start(GTK_BOX(page), r->graph, TRUE, TRUE, 0);

		foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_box_pack_start(GTK_BOX(foot),
				   styled_label("60 seconds", "tm-caption", 0),
				   TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(foot),
				 styled_label("0", "tm-caption", 1),
				 FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(page), foot, FALSE, FALSE, 0);

		grid = gtk_grid_new();
		gtk_grid_set_column_spacing(GTK_GRID(grid), 40);
		gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
		gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
		gtk_widget_set_margin_top(grid, 10);
		for (j = 0; j < MAX_STATS && stats[i][j]; j++)
			add_stat(r, grid, stats[i][j]);
		gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

		gtk_stack_add_named(GTK_STACK(a->perf_stack), page, r->name);
	}

	g_signal_connect(list, "row-selected",
			 G_CALLBACK(on_resource_selected), a);
	gtk_list_box_select_row(GTK_LIST_BOX(list),
				gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroll, 240, -1);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(box), scroll, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   gtk_separator_new(GTK_ORIENTATION_VERTICAL),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), a->perf_stack, TRUE, TRUE, 0);
	return box;
}

/*
 * Window
 */

static const char css[] =
	".tm-title { font-size: 24px; font-weight: 300; }"
	".tm-sub { opacity: 0.7; }"
	".tm-caption { font-size: 11px; opacity: 0.65; }"
	".tm-stat-label { font-size: 12px; opacity: 0.65; margin-top: 6px; }"
	".tm-stat-value { font-size: 17px; }"
	".tm-res-name { font-weight: bold; }"
	".tm-status { padding: 5px 10px; font-size: 12px; opacity: 0.8; }";

static void
on_window_destroy(GtkWidget *w, App *a)
{
	if (a->timer)
		g_source_remove(a->timer);
	a->timer = 0;
	a->window = NULL;
}

static void
activate(GtkApplication *gapp, App *a)
{
	GtkWidget *header, *switcher, *stack;
	GtkCssProvider *provider;

	if (a->window) {	/* opened again: show the open one */
		gtk_window_present(GTK_WINDOW(a->window));
		return;
	}

	g_object_set(gtk_settings_get_default(),
		     "gtk-application-prefer-dark-theme", TRUE, NULL);
	provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);

	a->window = gtk_application_window_new(gapp);
	gtk_window_set_title(GTK_WINDOW(a->window), "Task Manager");
	gtk_window_set_default_size(GTK_WINDOW(a->window), 960, 640);
	g_signal_connect(a->window, "destroy", G_CALLBACK(on_window_destroy), a);

	stack = a->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack),
				      GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_add_titled(GTK_STACK(stack), build_apps(a),
			     "apps", "Apps");
	gtk_stack_add_titled(GTK_STACK(stack), build_processes(a),
			     "processes", "Processes");
	gtk_stack_add_titled(GTK_STACK(stack), build_performance(a),
			     "performance", "Performance");
	gtk_stack_add_titled(GTK_STACK(stack), build_services(a),
			     "services", "Services");
	gtk_container_add(GTK_CONTAINER(a->window), stack);

	header = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
	/* GtkWindow copies the header bar title into its own (taskbar label) */
	gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Task Manager");
	switcher = gtk_stack_switcher_new();
	gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher),
				     GTK_STACK(stack));
	gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header), switcher);
	gtk_window_set_titlebar(GTK_WINDOW(a->window), header);

	refresh(a);		/* first sample: rates start on the next one */
	a->timer = g_timeout_add_seconds(1, refresh, a);
	gtk_widget_show_all(a->window);
	gtk_widget_grab_focus(a->app_tree);
}

int
main(int argc, char **argv)
{
	static App app;
	App *a = &app;
	GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
	int status;

	/* single instance through the session bus, when there is one */
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;

	a->page_size = sysconf(_SC_PAGESIZE);
	a->ncpu = (int) sysconf(_SC_NPROCESSORS_ONLN);
	a->procs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					 NULL, proc_free);
	a->groups = g_hash_table_new_full(g_str_hash, g_str_equal,
					  g_free, group_free);
	a->comms = g_hash_table_new_full(g_str_hash, g_str_equal,
					 g_free, NULL);
	a->apps = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, app_row_free);
	a->svcs = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, svc_row_free);
	a->windows_path = g_build_filename(g_get_user_runtime_dir(),
					   "hexa-windows", NULL);
	a->users = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					 NULL, g_free);

	a->res[RES_CPU] = (Res) { .name = "CPU", .caption = "% Utilization",
		.r = 0.133, .g = 0.827, .b = 0.933, .max = 100 };
	a->res[RES_MEM] = (Res) { .name = "Memory", .caption = "Memory usage",
		.r = 0.655, .g = 0.545, .b = 0.980, .max = 100 };
	a->res[RES_DISK] = (Res) { .name = "Disk", .caption = "Active time",
		.r = 0.290, .g = 0.870, .b = 0.500, .max = 100 };
	a->res[RES_NET] = (Res) { .name = "Network",
		.caption = "Throughput: receive (solid), send (dashed)",
		.r = 0.980, .g = 0.570, .b = 0.240, .two = TRUE,
		.scale = 1e4 };

	g_set_prgname(APP_ID);	/* GTK3 sends it as the Wayland app-id */
	a->gapp = gtk_application_new(APP_ID, flags);
	g_signal_connect(a->gapp, "activate", G_CALLBACK(activate), a);
	status = g_application_run(G_APPLICATION(a->gapp), argc, argv);
	g_object_unref(a->gapp);
	return status;
}
