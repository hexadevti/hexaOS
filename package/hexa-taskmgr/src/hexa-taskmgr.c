/*
 * hexa-taskmgr: the hexaOS Task Manager (GTK 3).
 *
 *   Processes    running programs with their CPU and memory use; end them
 *   Performance  live graphs of CPU, memory, disk and network use
 *
 * Opened from the Start menu, by right-clicking the taskbar or the Start
 * button, or with Ctrl+Shift+Esc. Everything is read from /proc and /sys
 * once a second.
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
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

enum { COL_NAME, COL_PID, COL_USER, COL_STATUS, COL_CPU, COL_MEM, N_COLS };
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
	int pid;
	unsigned long long ticks;	/* CPU time used so far */
	gboolean seen;
	GtkTreeIter iter;
} Proc;

typedef struct {
	GtkApplication *gapp;
	GtkWidget *window, *tree, *search, *end_btn, *status, *menu;
	GtkListStore *store;
	GtkTreeModel *filter, *sort;
	GHashTable *procs;		/* pid -> Proc */
	GHashTable *users;		/* uid -> user name */
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

	d = opendir("/proc");
	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[64], comm[64], state, *stat, *lp, *rp, *name;
		unsigned long flags;
		unsigned long long ut, st, ticks;
		long threads, rss;
		struct stat sb;
		double cpu = 0;
		Proc *p;
		int pid;

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
		    sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %lu %*u %*u %*u %*u "
			   "%llu %llu %*d %*d %*d %*d %ld %*d %*u %*u %ld",
			   &state, &flags, &ut, &st, &threads, &rss) != 6) {
			g_free(stat);
			continue;
		}
		g_strlcpy(comm, lp + 1, MIN(sizeof comm, (gsize) (rp - lp)));
		g_free(stat);

		if (flags & PF_KTHREAD)
			continue;	/* kernel threads are not programs */

		(*nprocs)++;
		*nthreads += threads;

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
			gtk_list_store_append(a->store, &p->iter);
			g_hash_table_insert(a->procs, GINT_TO_POINTER(pid), p);
		}
		p->ticks = ticks;
		p->seen = TRUE;

		name = process_name(pid, comm);
		gtk_list_store_set(a->store, &p->iter,
				   COL_NAME, name,
				   COL_PID, pid,
				   COL_USER, user_name(a, sb.st_uid),
				   COL_STATUS, status_text(state),
				   COL_CPU, cpu,
				   COL_MEM, (guint64) (rss > 0 ? rss : 0) * a->page_size,
				   -1);
		g_free(name);
	}
	closedir(d);

	/* forget the processes that ended */
	g_hash_table_iter_init(&it, a->procs);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		Proc *p = value;

		if (!p->seen) {
			gtk_list_store_remove(a->store, &p->iter);
			g_hash_table_iter_remove(&it);
		}
	}
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

static int
selected_pid(App *a, char **name)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree));
	GtkTreeModel *model;
	GtkTreeIter iter;
	int pid = 0;

	*name = NULL;
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return 0;
	gtk_tree_model_get(model, &iter, COL_PID, &pid, COL_NAME, name, -1);
	return pid;
}

static void
end_selected(App *a, int sig)
{
	char *name, *msg;
	int pid = selected_pid(a, &name);

	if (pid <= 0)
		return;
	if (pid == 1) {
		show_error(a, "This process cannot be ended.",
			   "It is the first process of the system (init): "
			   "ending it would stop the computer.");
	} else if (kill(pid, sig) < 0) {
		if (errno == EPERM)
			msg = g_strdup_printf("You do not have permission to end "
					      "\"%s\" (PID %d). It belongs to "
					      "another user, such as the system.",
					      name, pid);
		else
			msg = g_strdup_printf("\"%s\" (PID %d): %s", name, pid,
					      g_strerror(errno));
		show_error(a, "Could not end the task.", msg);
		g_free(msg);
	}
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
}

static gboolean
row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	App *a = data;
	const char *text = gtk_entry_get_text(GTK_ENTRY(a->search));
	char *name = NULL, *user = NULL, *needle, *hay, pidbuf[16];
	gboolean ok;
	int pid;

	if (!text[0])
		return TRUE;
	gtk_tree_model_get(model, iter, COL_NAME, &name, COL_PID, &pid,
			   COL_USER, &user, -1);
	if (!name) {		/* a row being added */
		g_free(user);
		return FALSE;
	}
	needle = g_utf8_casefold(text, -1);
	hay = g_utf8_casefold(name, -1);
	snprintf(pidbuf, sizeof pidbuf, "%d", pid);
	ok = strstr(hay, needle) || g_str_has_prefix(pidbuf, text) ||
	     (user && g_ascii_strcasecmp(user, text) == 0);
	g_free(hay);
	g_free(needle);
	g_free(name);
	g_free(user);
	return ok;
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

static void
add_column(App *a, const char *title, int col, gboolean right,
	   GtkTreeCellDataFunc func, gboolean expand)
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
		gtk_tree_view_column_set_cell_data_func(c, cell, func, a, NULL);
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
	gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree), c);
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
		"Shift+Delete forces it to stop.");
	g_signal_connect(a->end_btn, "clicked", G_CALLBACK(on_end_clicked), a);
	gtk_box_pack_end(GTK_BOX(bar), a->end_btn, FALSE, FALSE, 0);

	run = gtk_button_new_with_label("Run new task");
	g_signal_connect(run, "clicked", G_CALLBACK(on_run_task), a);
	gtk_box_pack_end(GTK_BOX(bar), run, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

	a->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT,
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
	add_column(a, "PID", COL_PID, TRUE, NULL, FALSE);
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
	gtk_window_set_default_size(GTK_WINDOW(a->window), 900, 620);
	g_signal_connect(a->window, "destroy", G_CALLBACK(on_window_destroy), a);

	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack),
				      GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_add_titled(GTK_STACK(stack), build_processes(a),
			     "processes", "Processes");
	gtk_stack_add_titled(GTK_STACK(stack), build_performance(a),
			     "performance", "Performance");
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
	gtk_widget_grab_focus(a->tree);
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
					 NULL, g_free);
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
