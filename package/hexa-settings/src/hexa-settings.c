/*
 * hexa-settings: the hexaOS Settings app (GTK 3)
 *
 * Pages on the left, as in Windows 11: Network, Bluetooth, Sound, Display,
 * Personalization, Keyboard, Mouse & touchpad, Date & time, Power, Account,
 * Services and About. "hexa-settings PAGE" opens on one page (e.g. sound),
 * also in a window that is already open;
 * "hexa-settings --set GROUP KEY VALUE" changes one desktop setting, no window.
 *
 * Desktop settings: what the user changes is kept in
 * ~/.config/hexaos/settings.ini and written out as ~/.config/hexaos/desktop.ini,
 * which hexa-session puts in front of /etc/xdg/weston/weston.ini when the
 * desktop starts. Weston uses the first section it finds with a given name, so
 * every section there is a full copy of the system one with the user's
 * changes applied. These changes take effect when the desktop restarts.
 *
 * System changes (timezone, computer name, password, services) are made by
 * /usr/libexec/hexa-settings-helper, run through sudo with the user's password.
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gio/gdesktopappinfo.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_ID		"org.hexaos.Settings"
#define HELPER		"/usr/libexec/hexa-settings-helper"
#define SYSTEM_INI	"/etc/xdg/weston/weston.ini"
#define WIFI_SH		"/usr/lib/hexaos/wifi.sh"
#define ZONEINFO	"/usr/share/zoneinfo"
#define XKB_LIST	"/usr/share/X11/xkb/rules/evdev.lst"
#define DEFAULT_WALLPAPER "/usr/share/hexaos/wallpaper.png"

/* argv / item lists written in place: ARGV("ls", "-l") */
#define ARGV(...)	((const char *const[]) { __VA_ARGS__, NULL })
#define ITEMS(...)	((const char *const[]) { __VA_ARGS__, NULL })

static GtkWidget *win, *stack, *nav, *restart_bar;
static GKeyFile *user_ini, *sys_ini;
static char *conf_dir, *user_ini_path, *desktop_ini_path;
static char *admin_pw;		/* the user's password, once given (for sudo) */
static const char *start_page;

/*
 * Small helpers
 */

/* Contents of a small file, without surrounding white space (NULL if unreadable) */
static char *
read_file(const char *path)
{
	char *s = NULL;

	if (!g_file_get_contents(path, &s, NULL, NULL))
		return NULL;
	return g_strstrip(s);
}

/* Removes terminal escape sequences (colors) and carriage returns, in place */
static char *
strip_ansi(char *s)
{
	char *r = s, *w = s;

	if (!s)
		return NULL;
	while (*r) {
		if (*r == '\033') {
			r++;
			if (*r == '[') {
				r++;
				while (*r && !(*r >= '@' && *r <= '~'))
					r++;
				if (*r)
					r++;
			} else if (*r) {
				r++;
			}
			continue;
		}
		if (*r == '\r') {
			r++;
			continue;
		}
		*w++ = *r++;
	}
	*w = '\0';
	return s;
}

/* Runs a short command and returns what it printed (NULL if it failed) */
static char *
run_out(const char *const *argv)
{
	char *out = NULL;
	int status = 0;

	if (!g_spawn_sync(NULL, (char **) argv, NULL,
			  G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			  NULL, NULL, &out, NULL, &status, NULL))
		return NULL;
	if (!g_spawn_check_wait_status(status, NULL)) {
		g_free(out);
		return NULL;
	}
	return strip_ansi(out);
}

/* Value after KEY on the line that starts with it ("State    connected") */
static char *
text_value(const char *text, const char *key)
{
	char **lines, **l, *val = NULL;
	size_t n = strlen(key);

	if (!text)
		return NULL;
	lines = g_strsplit(text, "\n", -1);
	for (l = lines; *l && !val; l++) {
		char *s = g_strstrip(*l);

		if (strncmp(s, key, n) == 0 && (s[n] == ' ' || s[n] == '\t' || s[n] == ':')) {
			char *v = s + n;

			while (*v == ' ' || *v == '\t' || *v == ':')
				v++;
			val = g_strdup(v);
		}
	}
	g_strfreev(lines);
	return val;
}

/* PID of the first process called NAME, or 0 */
static pid_t
find_proc(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	pid_t found = 0;

	while (d && !found && (e = readdir(d))) {
		char path[300], comm[64] = "";
		FILE *f;

		if (!isdigit((unsigned char) e->d_name[0]))
			continue;
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		if (!(f = fopen(path, "r")))
			continue;
		if (fgets(comm, sizeof comm, f)) {
			comm[strcspn(comm, "\n")] = '\0';
			if (strcmp(comm, name) == 0)
				found = atoi(e->d_name);
		}
		fclose(f);
	}
	if (d)
		closedir(d);
	return found;
}

typedef void (*JobDone)(gboolean ok, const char *out, const char *err, gpointer data);

typedef struct {
	JobDone done;
	gpointer data;
} Job;

static void
job_finished(GObject *src, GAsyncResult *res, gpointer user)
{
	Job *job = user;
	GSubprocess *p = G_SUBPROCESS(src);
	char *out = NULL, *err = NULL;
	GError *e = NULL;
	gboolean ok;

	ok = g_subprocess_communicate_utf8_finish(p, res, &out, &err, &e);
	if (ok)
		ok = g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
	else if (e)
		err = g_strdup(e->message);
	if (job->done)
		job->done(ok, out ? strip_ansi(out) : "", err ? g_strstrip(strip_ansi(err)) : "",
			  job->data);
	g_free(out);
	g_free(err);
	g_clear_error(&e);
	g_object_unref(p);
	g_free(job);
}

/* Runs a command in the background; DONE (may be NULL) gets its output */
static void
run_job(const char *const *argv, const char *input, JobDone done, gpointer data)
{
	GError *e = NULL;
	GSubprocess *p;
	Job *job;

	p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
			      G_SUBPROCESS_FLAGS_STDOUT_PIPE |
			      G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
	if (!p) {
		if (done)
			done(FALSE, "", e->message, data);
		g_error_free(e);
		return;
	}
	job = g_new0(Job, 1);
	job->done = done;
	job->data = data;
	g_subprocess_communicate_utf8_async(p, input, NULL, job_finished, job);
}

static void
message(GtkMessageType type, const char *title, const char *text)
{
	GtkWidget *d;

	d = gtk_message_dialog_new(GTK_WINDOW(win),
				   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
				   type, GTK_BUTTONS_CLOSE, "%s", title);
	if (text && *text)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static gboolean
confirm(const char *title, const char *text, const char *button)
{
	GtkWidget *d;
	int r;

	d = gtk_message_dialog_new(GTK_WINDOW(win),
				   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
				   GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
	if (text && *text)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
	gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL,
			       button, GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	r = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return r == GTK_RESPONSE_ACCEPT;
}

/* Text field in a dialog. Returns NULL if cancelled. */
static char *
ask_text(const char *title, const char *text, const char *initial, gboolean secret)
{
	GtkWidget *d, *box, *entry, *show = NULL;
	char *result = NULL;

	d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(win),
					GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					"Cancel", GTK_RESPONSE_CANCEL,
					"OK", GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	if (text) {
		GtkWidget *l = gtk_label_new(text);

		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	}
	entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 34);
	if (initial)
		gtk_entry_set_text(GTK_ENTRY(entry), initial);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	if (secret) {
		gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
		gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
		show = gtk_check_button_new_with_label("Show the password");
		g_object_bind_property(show, "active", entry, "visibility", 0);
		gtk_box_pack_start(GTK_BOX(box), show, FALSE, FALSE, 0);
	}
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
		result = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(d);
	return result;
}

/*
 * Administrator (sudo)
 */

/* Checks the user's password with sudo (and remembers it for this session) */
static gboolean
password_ok(const char *pw)
{
	GSubprocess *p;
	GBytes *in;
	gboolean ok;
	char *line = g_strdup_printf("%s\n", pw);

	p = g_subprocess_newv(ARGV("sudo", "-k", "-S", "-p", "", "-v"),
			      G_SUBPROCESS_FLAGS_STDIN_PIPE |
			      G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
			      G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	if (!p) {
		g_free(line);
		return FALSE;
	}
	in = g_bytes_new_take(line, strlen(line));
	ok = g_subprocess_communicate(p, in, NULL, NULL, NULL, NULL) &&
	     g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
	g_bytes_unref(in);
	g_object_unref(p);
	return ok;
}

/* Asks for the password if it is not known yet. FALSE if cancelled. */
static gboolean
get_admin(void)
{
	const char *text = "This change affects the whole computer. "
			   "Type your password to allow it.";

	if (getuid() == 0 || admin_pw)
		return TRUE;
	for (;;) {
		char *pw = ask_text("Administrator permission", text, NULL, TRUE);

		if (!pw)
			return FALSE;
		gdk_window_set_cursor(gtk_widget_get_window(win),
			gdk_cursor_new_from_name(gtk_widget_get_display(win), "wait"));
		while (gtk_events_pending())
			gtk_main_iteration();
		if (password_ok(pw)) {
			admin_pw = pw;
			gdk_window_set_cursor(gtk_widget_get_window(win), NULL);
			return TRUE;
		}
		gdk_window_set_cursor(gtk_widget_get_window(win), NULL);
		g_free(pw);
		text = "Wrong password, or this account may not change the system. Try again.";
	}
}

typedef struct {
	JobDone done;
	gpointer data;
} AdminJob;

static void
admin_finished(gboolean ok, const char *out, const char *err, gpointer user)
{
	AdminJob *a = user;

	if (!ok && strstr(err, "incorrect password")) {
		g_free(admin_pw);
		admin_pw = NULL;
	}
	/* sudo's own lines are not interesting to the user */
	if (g_str_has_prefix(err, "We trust you"))
		err = "";
	if (a->done)
		a->done(ok, out, err, a->data);
	g_free(a);
}

/* Runs "hexa-settings-helper ARGS..." as administrator */
static void
as_admin(const char *const *args, const char *input, JobDone done, gpointer data)
{
	GPtrArray *argv = g_ptr_array_new();
	AdminJob *a;
	char *in;
	int i;

	if (!get_admin()) {
		if (done)
			done(FALSE, "", "", data);	/* cancelled: nothing to show */
		g_ptr_array_free(argv, TRUE);
		return;
	}
	if (getuid() != 0) {
		const char *const sudo[] = { "sudo", "-k", "-S", "-p", "" };

		for (i = 0; i < 5; i++)
			g_ptr_array_add(argv, (gpointer) sudo[i]);
		in = g_strdup_printf("%s\n%s", admin_pw, input ? input : "");
	} else {
		in = g_strdup(input ? input : "");
	}
	g_ptr_array_add(argv, HELPER);
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, (gpointer) args[i]);
	g_ptr_array_add(argv, NULL);

	a = g_new0(AdminJob, 1);
	a->done = done;
	a->data = data;
	run_job((const char *const *) argv->pdata, in, admin_finished, a);
	memset(in, 0, strlen(in));
	g_free(in);
	g_ptr_array_free(argv, TRUE);
}

/* Shows what went wrong in a helper command, if anything */
static void
admin_report(gboolean ok, const char *out, const char *err, gpointer title)
{
	if (!ok && *err)
		message(GTK_MESSAGE_ERROR, title, err);
}

/*
 * Desktop configuration
 */

static char *
cfg_get(const char *group, const char *key, const char *def)
{
	char *v = g_key_file_get_value(user_ini, group, key, NULL);

	if (!v)
		v = g_key_file_get_value(sys_ini, group, key, NULL);
	return v ? v : g_strdup(def);
}

static void
show_restart_bar(void)
{
	gtk_widget_show(restart_bar);
}

/* Weston (14) only scales a screen by whole numbers: a zoom of 125% or 150%
 * keeps the screen at 1 and the rest goes to the apps (app_scale) */
static void
split_scale(const char *v, int *screen, double *apps)
{
	double z = v ? g_ascii_strtod(v, NULL) : 1;

	if (z < 1 || z > 4)
		z = 1;
	*screen = (int) z;
	*apps = z / *screen;
}

/* session.env: read by hexa-session when the desktop starts (app zoom) */
static void
write_session_env(double apps)
{
	char *path = g_build_filename(conf_dir, "session.env", NULL);

	if (apps > 1.01) {
		char buf[G_ASCII_DTOSTR_BUF_SIZE], *txt;

		g_ascii_formatd(buf, sizeof buf, "%.2f", apps);
		txt = g_strdup_printf("# Written by hexa-settings (Display > Zoom of 125%% "
				      "or 150%%): size of the apps\napp-scale=%s\n", buf);
		g_file_set_contents(path, txt, -1, NULL);
		g_free(txt);
	} else {
		g_unlink(path);
	}
	g_free(path);
}

/* Writes desktop.ini: system sections with the user's values on top */
static void
write_desktop_ini(void)
{
	GString *s = g_string_new("# Written by hexa-settings from settings.ini "
				  "(edit that file instead).\n"
				  "# Comes before /etc/xdg/weston/weston.ini "
				  "when the desktop starts.\n");
	char **groups = g_key_file_get_groups(user_ini, NULL);
	double app_scale = 1;
	int i, j;

	for (i = 0; groups[i]; i++) {
		const char *g = groups[i];
		char **keys = g_key_file_get_keys(user_ini, g, NULL, NULL);

		if (!keys || !keys[0]) {
			g_strfreev(keys);
			continue;
		}
		if (g_str_has_prefix(g, "output ")) {
			g_string_append_printf(s, "\n[output]\nname=%s\n", g + 7);
		} else {
			char **sk = g_key_file_get_keys(sys_ini, g, NULL, NULL);

			g_string_append_printf(s, "\n[%s]\n", g);
			for (j = 0; sk && sk[j]; j++) {
				char *v;

				if (g_key_file_has_key(user_ini, g, sk[j], NULL))
					continue;
				v = g_key_file_get_value(sys_ini, g, sk[j], NULL);
				g_string_append_printf(s, "%s=%s\n", sk[j], v ? v : "");
				g_free(v);
			}
			g_strfreev(sk);
		}
		for (j = 0; keys[j]; j++) {
			char *v = g_key_file_get_value(user_ini, g, keys[j], NULL);

			if (g_str_has_prefix(g, "output ") && strcmp(keys[j], "scale") == 0) {
				int screen;
				double apps;

				split_scale(v, &screen, &apps);
				if (apps > app_scale)
					app_scale = apps;
				g_string_append_printf(s, "scale=%d\n", screen);
			} else {
				g_string_append_printf(s, "%s=%s\n", keys[j], v ? v : "");
			}
			g_free(v);
		}
		g_strfreev(keys);
	}
	g_strfreev(groups);
	g_file_set_contents(desktop_ini_path, s->str, -1, NULL);
	write_session_env(app_scale);
	g_string_free(s, TRUE);
}

static void
cfg_save(void)
{
	GError *e = NULL;

	g_mkdir_with_parents(conf_dir, 0755);
	if (!g_key_file_save_to_file(user_ini, user_ini_path, &e)) {
		message(GTK_MESSAGE_ERROR, "Could not save the settings", e->message);
		g_error_free(e);
		return;
	}
	write_desktop_ini();
}

/* Sets (or with NULL removes) a desktop setting and saves */
static void
cfg_set(const char *group, const char *key, const char *value)
{
	char *old = g_key_file_get_value(user_ini, group, key, NULL);

	if (g_strcmp0(old, value) != 0) {
		if (value)
			g_key_file_set_value(user_ini, group, key, value);
		else
			g_key_file_remove_key(user_ini, group, key, NULL);
		cfg_save();
		show_restart_bar();
	}
	g_free(old);
}

/* Settings saved by the old text-mode Settings (KB_LAYOUT=..., SCALE=...) */
static void
migrate_old_settings(void)
{
	char *path = g_build_filename(conf_dir, "settings", NULL);
	char *txt = read_file(path), **lines, **l;

	g_free(path);
	if (!txt)
		return;
	lines = g_strsplit(txt, "\n", -1);
	for (l = lines; *l; l++) {
		char *eq = strchr(*l, '=');

		if (!eq)
			continue;
		*eq++ = '\0';
		if (strcmp(*l, "KB_LAYOUT") == 0)
			g_key_file_set_value(user_ini, "keyboard", "keymap_layout", eq);
		else if (strcmp(*l, "KB_VARIANT") == 0)
			g_key_file_set_value(user_ini, "keyboard", "keymap_variant", eq);
	}
	g_strfreev(lines);
	g_free(txt);
	/* the scale was kept per screen in desktop.ini */
	txt = read_file(desktop_ini_path);
	if (txt) {
		char *name = NULL;

		lines = g_strsplit(txt, "\n", -1);
		for (l = lines; *l; l++) {
			if (g_str_has_prefix(*l, "name=")) {
				g_free(name);
				name = g_strdup(*l + 5);
			} else if (g_str_has_prefix(*l, "scale=") && name) {
				char *g = g_strconcat("output ", name, NULL);

				g_key_file_set_value(user_ini, g, "scale", *l + 6);
				g_free(g);
			}
		}
		g_free(name);
		g_strfreev(lines);
		g_free(txt);
	}
	cfg_save();
}

static void
cfg_load(void)
{
	user_ini = g_key_file_new();
	sys_ini = g_key_file_new();
	conf_dir = g_build_filename(g_get_home_dir(), ".config", "hexaos", NULL);
	user_ini_path = g_build_filename(conf_dir, "settings.ini", NULL);
	desktop_ini_path = g_build_filename(conf_dir, "desktop.ini", NULL);
	g_key_file_load_from_file(sys_ini, SYSTEM_INI, G_KEY_FILE_KEEP_COMMENTS, NULL);
	if (!g_key_file_load_from_file(user_ini, user_ini_path,
				       G_KEY_FILE_KEEP_COMMENTS, NULL)) {
		g_key_file_set_comment(user_ini, NULL, NULL,
			" hexaOS Settings: your desktop settings (only what you changed).\n"
			" Sections and keys as in /etc/xdg/weston/weston.ini; [output NAME] is one screen.",
			NULL);
		migrate_old_settings();
	}
}

/* The desktop read its config before desktop.ini last changed */
static gboolean
restart_pending(void)
{
	struct stat a, b;
	char *runtime = g_build_filename(g_get_user_runtime_dir(), "weston.ini", NULL);
	gboolean pending = stat(desktop_ini_path, &a) == 0 &&
			   stat(runtime, &b) == 0 && a.st_mtime > b.st_mtime;

	g_free(runtime);
	return pending;
}

static void
restart_desktop(void)
{
	pid_t pid;

	if (!confirm("Restart the desktop?",
		     "All open windows will close, so save your work first. "
		     "You come back to the desktop in a few seconds.", "Restart desktop"))
		return;
	pid = find_proc("weston");
	if (pid > 0)
		kill(pid, SIGTERM);
}

/*
 * Page building blocks
 */

static GtkWidget *page_box;	/* page being built */

static void
add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

static GtkWidget *
label_new(const char *text, const char *cls)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
	if (cls)
		add_class(l, cls);
	return l;
}

static void
set_sub(GtkWidget *label, const char *text)
{
	gtk_label_set_text(GTK_LABEL(label), text ? text : "");
	gtk_widget_set_visible(label, text && *text);
}

static GtkWidget *
page_new(const char *title, const char *intro)
{
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	page_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(page_box, 32);
	gtk_widget_set_margin_end(page_box, 32);
	gtk_widget_set_margin_top(page_box, 20);
	gtk_widget_set_margin_bottom(page_box, 28);
	gtk_box_pack_start(GTK_BOX(page_box), label_new(title, "hx-title"), FALSE, FALSE, 0);
	if (intro)
		gtk_box_pack_start(GTK_BOX(page_box), label_new(intro, "hx-dim"), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(sw), page_box);
	return sw;
}

/* A titled card (list of rows) on the current page */
static GtkWidget *
section(const char *title)
{
	GtkWidget *list = gtk_list_box_new();

	if (title)
		gtk_box_pack_start(GTK_BOX(page_box), label_new(title, "hx-section"),
				   FALSE, FALSE, 0);
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	add_class(list, "hx-card");
	gtk_box_pack_start(GTK_BOX(page_box), list, FALSE, FALSE, 0);
	return list;
}

/* A row: title and grey subtitle on the left, CONTROL on the right */
static GtkWidget *
add_row(GtkWidget *list, const char *title, const char *sub, GtkWidget *control,
	GtkWidget **sub_out)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *sl = label_new(NULL, "hx-dim");

	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	add_class(box, "hx-row");
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(text), label_new(title, NULL), FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(sl, TRUE);
	set_sub(sl, sub);
	gtk_box_pack_start(GTK_BOX(text), sl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
	if (control) {
		gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
		gtk_box_pack_end(GTK_BOX(box), control, FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(row), box);
	gtk_container_add(GTK_CONTAINER(list), row);
	if (sub_out)
		*sub_out = sl;
	return row;
}

static void
list_clear(GtkWidget *list)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(list)), *c;

	for (c = children; c; c = c->next)
		gtk_widget_destroy(c->data);
	g_list_free(children);
}

static GtkWidget *
button_new(const char *text, GCallback cb, gpointer data)
{
	GtkWidget *b = gtk_button_new_with_label(text);

	g_signal_connect(b, "clicked", cb, data);
	return b;
}

/* Widgets that read and write one desktop setting */

static void
bind_key(GtkWidget *w, const char *group, const char *key)
{
	g_object_set_data_full(G_OBJECT(w), "group", g_strdup(group), g_free);
	g_object_set_data_full(G_OBJECT(w), "key", g_strdup(key), g_free);
}

#define W_GROUP(w)	((const char *) g_object_get_data(G_OBJECT(w), "group"))
#define W_KEY(w)	((const char *) g_object_get_data(G_OBJECT(w), "key"))

static void
on_bound_switch(GObject *sw, GParamSpec *ps, gpointer data)
{
	gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));

	cfg_set(W_GROUP(sw), W_KEY(sw), g_object_get_data(sw, on ? "on" : "off"));
}

/* Switch: ON / OFF are the values written ("true"/"false", "fade"/"none") */
static GtkWidget *
bound_switch(const char *group, const char *key, const char *on, const char *off,
	     const char *def)
{
	char *v = cfg_get(group, key, def);
	GtkWidget *sw = gtk_switch_new();
	gboolean active = strcmp(v, on) == 0 ||
			  (strcmp(on, "true") == 0 && (!strcmp(v, "1") || !strcmp(v, "yes")));

	gtk_switch_set_active(GTK_SWITCH(sw), active);
	bind_key(sw, group, key);
	g_object_set_data(G_OBJECT(sw), "on", (gpointer) on);
	g_object_set_data(G_OBJECT(sw), "off", (gpointer) off);
	g_signal_connect(sw, "notify::active", G_CALLBACK(on_bound_switch), NULL);
	g_free(v);
	return sw;
}

#define BOOL_SWITCH(g, k, def) bound_switch(g, k, "true", "false", (def) ? "true" : "false")

static void
on_bound_combo(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (id)
		cfg_set(W_GROUP(c), W_KEY(c), id);
}

/* Drop-down list; ITEMS are value, label, value, label... */
static GtkWidget *
bound_combo(const char *group, const char *key, const char *def, const char *const *items)
{
	char *v = cfg_get(group, key, def);
	GtkWidget *c = gtk_combo_box_text_new();
	int i;

	for (i = 0; items[i] && items[i + 1]; i += 2)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), items[i], items[i + 1]);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), v)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), v, v);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), v);
	}
	bind_key(c, group, key);
	g_signal_connect(c, "changed", G_CALLBACK(on_bound_combo), NULL);
	g_free(v);
	return c;
}

static void
on_bound_scale(GtkRange *r, gpointer data)
{
	char buf[G_ASCII_DTOSTR_BUF_SIZE];
	double v = gtk_range_get_value(r);

	if (gtk_scale_get_digits(GTK_SCALE(r)) > 0)
		g_ascii_formatd(buf, sizeof buf, "%.2f", v);
	else
		g_snprintf(buf, sizeof buf, "%d", (int) (v + 0.5));
	cfg_set(W_GROUP(r), W_KEY(r), buf);
}

static GtkWidget *
bound_scale(const char *group, const char *key, double min, double max, double step,
	    double def, int digits)
{
	char *v = cfg_get(group, key, NULL);
	GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);

	gtk_scale_set_digits(GTK_SCALE(s), digits);
	gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
	gtk_widget_set_size_request(s, 260, -1);
	gtk_range_set_value(GTK_RANGE(s), v ? g_ascii_strtod(v, NULL) : def);
	bind_key(s, group, key);
	g_signal_connect(s, "value-changed", G_CALLBACK(on_bound_scale), NULL);
	g_free(v);
	return s;
}

/*
 * Network
 */

static struct {
	char *iface;
	GtkWidget *power, *status, *spinner, *tools, *list, *wired;
	gboolean busy, filling;
} net;

typedef struct {
	char *ssid, *sec, *out;
	gboolean hidden;
	int mark, tries;
} WifiConn;

static void wifi_scan(void);

static void
wifi_update_status(void)
{
	char *dev, *powered, *st = NULL, *state = NULL, *ssid = NULL, *ip = NULL, *text;
	gboolean on;

	if (!net.iface)
		return;
	dev = run_out(ARGV("iwctl", "device", net.iface, "show"));
	powered = text_value(dev, "Powered");
	on = !powered || strcmp(powered, "off") != 0;
	net.filling = TRUE;
	gtk_switch_set_active(GTK_SWITCH(net.power), on);
	net.filling = FALSE;
	if (on) {
		st = run_out(ARGV("iwctl", "station", net.iface, "show"));
		state = text_value(st, "State");
		ssid = text_value(st, "Connected network");
		ip = text_value(st, "IPv4 address");
	}
	if (!on)
		text = g_strdup("Wi-Fi is turned off.");
	else if (g_strcmp0(state, "connected") == 0)
		text = g_strdup_printf("Connected to %s%s%s", ssid ? ssid : "a network",
				       ip ? " · " : "", ip ? ip : "");
	else if (state && g_str_has_prefix(state, "connecting"))
		text = g_strdup_printf("Connecting%s%s...", ssid ? " to " : "", ssid ? ssid : "");
	else
		text = g_strdup("Not connected");
	set_sub(net.status, text);
	gtk_widget_set_sensitive(net.tools, on);
	gtk_widget_set_visible(net.list, on);
	g_free(text);
	g_free(dev);
	g_free(powered);
	g_free(st);
	g_free(state);
	g_free(ssid);
	g_free(ip);
}

static int
dmesg_lines(void)
{
	char *out = run_out(ARGV("sh", "-c", "dmesg 2>/dev/null | wc -l"));
	int n = out ? atoi(out) : 0;

	g_free(out);
	return n;
}

static void
wifi_conn_free(WifiConn *c)
{
	g_free(c->ssid);
	g_free(c->sec);
	g_free(c->out);
	g_free(c);
}

static void
wifi_conn_end(WifiConn *c, gboolean connected)
{
	gtk_spinner_stop(GTK_SPINNER(net.spinner));
	net.busy = FALSE;
	wifi_update_status();
	if (!connected) {
		char *diag, *title = g_strdup_printf("Could not connect to \"%s\"", c->ssid);
		char mark[16];

		g_snprintf(mark, sizeof mark, "%d", c->mark);
		diag = run_out(ARGV("bash", "-c", ". " WIFI_SH "; hx_wifi_diagnose \"$@\"", "_",
				    net.iface, c->out ? c->out : "", mark));
		message(GTK_MESSAGE_ERROR, title, diag ? g_strstrip(diag) : NULL);
		g_free(diag);
		g_free(title);
	}
	wifi_conn_free(c);
	wifi_scan();
}

static gboolean
wifi_conn_poll(gpointer data)
{
	WifiConn *c = data;
	char *st = run_out(ARGV("iwctl", "station", net.iface, "show"));
	char *state = text_value(st, "State");
	gboolean connected = g_strcmp0(state, "connected") == 0;

	g_free(st);
	g_free(state);
	if (connected || ++c->tries >= 15) {
		wifi_conn_end(c, connected);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void
wifi_conn_wait(gboolean ok, const char *out, const char *err, gpointer data)
{
	WifiConn *c = data;

	g_free(c->out);
	c->out = g_strconcat(out, err, NULL);
	g_timeout_add_seconds(1, wifi_conn_poll, c);
}

/* Asks for a WPA password (8 to 63 characters) */
static char *
wifi_ask_password(const char *ssid)
{
	char *text = g_strdup_printf("Password of the Wi-Fi network \"%s\":", ssid);
	char *pw = NULL;

	for (;;) {
		long n;

		g_free(pw);
		pw = ask_text("Wi-Fi password", text, NULL, TRUE);
		if (!pw)
			break;
		n = g_utf8_strlen(pw, -1);
		if (n >= 8 && n <= 63)
			break;
		g_free(text);
		text = g_strdup_printf("A Wi-Fi (WPA) password has 8 to 63 characters; "
				       "that one had %ld. Password of \"%s\":", n, ssid);
	}
	g_free(text);
	return pw;
}

static void
wifi_conn_first(gboolean ok, const char *out, const char *err, gpointer data)
{
	WifiConn *c = data;
	const char *cmd = c->hidden ? "connect-hidden" : "connect";
	char *pw;

	if (ok || strcmp(c->sec, "open") == 0) {
		wifi_conn_wait(ok, out, err, c);
		return;
	}
	/* not saved yet (or the saved password failed): ask for it */
	pw = wifi_ask_password(c->ssid);
	if (!pw) {
		gtk_spinner_stop(GTK_SPINNER(net.spinner));
		net.busy = FALSE;
		wifi_update_status();
		wifi_conn_free(c);
		return;
	}
	c->mark = dmesg_lines();
	run_job(ARGV("iwctl", "--passphrase", pw, "station", net.iface, cmd, c->ssid),
		NULL, wifi_conn_wait, c);
	memset(pw, 0, strlen(pw));
	g_free(pw);
}

static void
wifi_connect(const char *ssid, const char *sec, gboolean hidden)
{
	WifiConn *c;
	char *text;

	if (strcmp(sec, "8021x") == 0) {
		message(GTK_MESSAGE_INFO, "Enterprise network",
			"This network uses enterprise login (802.1X), which Settings "
			"cannot set up yet. See \"man iwd.network\".");
		return;
	}
	if (net.busy)
		return;
	net.busy = TRUE;
	c = g_new0(WifiConn, 1);
	c->ssid = g_strdup(ssid);
	c->sec = g_strdup(sec);
	c->hidden = hidden;
	c->mark = dmesg_lines();
	text = g_strdup_printf("Connecting to %s...", ssid);
	set_sub(net.status, text);
	g_free(text);
	gtk_spinner_start(GTK_SPINNER(net.spinner));
	/* open and saved networks connect without a password */
	run_job(ARGV("iwctl", "--dont-ask", "station", net.iface,
		     hidden ? "connect-hidden" : "connect", ssid),
		NULL, wifi_conn_first, c);
}

static void
wifi_disconnected(gboolean ok, const char *out, const char *err, gpointer data)
{
	wifi_update_status();
	wifi_scan();
}

static void
on_wifi_row_button(GtkButton *b, gpointer data)
{
	const char *ssid = g_object_get_data(G_OBJECT(b), "ssid");
	const char *sec = g_object_get_data(G_OBJECT(b), "sec");

	if (g_object_get_data(G_OBJECT(b), "connected"))
		run_job(ARGV("iwctl", "station", net.iface, "disconnect"), NULL,
			wifi_disconnected, NULL);
	else
		wifi_connect(ssid, sec, FALSE);
}

static void
wifi_add_network(const char *ssid, const char *sec, int dbm100, gboolean connected)
{
	GtkWidget *row, *box, *icon, *text, *name, *sub, *btn;
	const char *sig, *what;

	sig = dbm100 >= -6000 ? "network-wireless-signal-excellent-symbolic" :
	      dbm100 >= -6700 ? "network-wireless-signal-good-symbolic" :
	      dbm100 >= -7500 ? "network-wireless-signal-ok-symbolic" :
				"network-wireless-signal-weak-symbolic";
	what = connected ? "Connected" :
	       strcmp(sec, "open") == 0 ? "Open (not secured)" :
	       strcmp(sec, "8021x") == 0 ? "Secured (enterprise)" : "Secured";

	row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	add_class(box, "hx-row");
	icon = gtk_image_new_from_icon_name(sig, GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	name = label_new(ssid, connected ? "hx-strong" : NULL);
	sub = label_new(what, "hx-dim");
	gtk_box_pack_start(GTK_BOX(text), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), sub, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
	if (strcmp(sec, "open") != 0)
		gtk_box_pack_start(GTK_BOX(box),
			gtk_image_new_from_icon_name("channel-secure-symbolic", GTK_ICON_SIZE_MENU),
			FALSE, FALSE, 0);
	btn = gtk_button_new_with_label(connected ? "Disconnect" : "Connect");
	gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
	g_object_set_data_full(G_OBJECT(btn), "ssid", g_strdup(ssid), g_free);
	g_object_set_data_full(G_OBJECT(btn), "sec", g_strdup(sec), g_free);
	if (connected)
		g_object_set_data(G_OBJECT(btn), "connected", GINT_TO_POINTER(1));
	g_signal_connect(btn, "clicked", G_CALLBACK(on_wifi_row_button), NULL);
	gtk_box_pack_end(GTK_BOX(box), btn, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(row), box);
	gtk_container_add(GTK_CONTAINER(net.list), row);
	gtk_widget_show_all(row);
}

static void
wifi_got_networks(gboolean ok, const char *out, const char *err, gpointer data)
{
	GRegex *re = g_regex_new("^\\s*(>\\s+)?(.*\\S)\\s+(psk|open|8021x|owe|wep)"
				 "(?: \\(unsupported\\))?\\s+(-?\\d+)\\s*$", 0, 0, NULL);
	char **lines = g_strsplit(out, "\n", -1), **l;

	list_clear(net.list);
	for (l = lines; *l; l++) {
		GMatchInfo *mi;

		if (g_regex_match(re, *l, 0, &mi)) {
			char *conn = g_match_info_fetch(mi, 1), *ssid = g_match_info_fetch(mi, 2);
			char *sec = g_match_info_fetch(mi, 3), *dbm = g_match_info_fetch(mi, 4);

			wifi_add_network(ssid, sec, atoi(dbm), conn && *conn);
			g_free(conn);
			g_free(ssid);
			g_free(sec);
			g_free(dbm);
		}
		g_match_info_free(mi);
	}
	g_strfreev(lines);
	g_regex_unref(re);
	gtk_spinner_stop(GTK_SPINNER(net.spinner));
	net.busy = FALSE;
}

static gboolean
wifi_list_later(gpointer data)
{
	run_job(ARGV("iwctl", "station", net.iface, "get-networks", "rssi-dbms"), NULL,
		wifi_got_networks, NULL);
	return G_SOURCE_REMOVE;
}

static void
wifi_scanned(gboolean ok, const char *out, const char *err, gpointer data)
{
	g_timeout_add(2500, wifi_list_later, NULL);
}

static void
wifi_scan(void)
{
	if (!net.iface || net.busy || !gtk_switch_get_active(GTK_SWITCH(net.power)))
		return;
	net.busy = TRUE;
	gtk_spinner_start(GTK_SPINNER(net.spinner));
	run_job(ARGV("iwctl", "station", net.iface, "scan"), NULL, wifi_scanned, NULL);
}

static void
wifi_power_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	if (!ok)
		message(GTK_MESSAGE_ERROR, "Could not turn Wi-Fi on or off",
			*err ? err : out);
	wifi_update_status();
	wifi_scan();
}

static void
on_wifi_power(GObject *sw, GParamSpec *ps, gpointer data)
{
	if (net.filling)
		return;
	run_job(ARGV("iwctl", "device", net.iface, "set-property", "Powered",
		     gtk_switch_get_active(GTK_SWITCH(sw)) ? "on" : "off"),
		NULL, wifi_power_done, NULL);
}

static void
on_wifi_hidden(GtkButton *b, gpointer data)
{
	char *ssid = ask_text("Hidden network",
			      "Name of the network (SSID), exactly as set in the router:",
			      NULL, FALSE);

	if (ssid && *ssid)
		wifi_connect(ssid, "psk", TRUE);
	g_free(ssid);
}

static void
on_forget(GtkButton *b, gpointer data)
{
	const char *name = g_object_get_data(G_OBJECT(b), "ssid");
	char *out = run_out(ARGV("iwctl", "known-networks", name, "forget"));

	if (out)
		gtk_widget_destroy(gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW));
	else
		message(GTK_MESSAGE_ERROR, "Could not forget the network", NULL);
	g_free(out);
}

static void
on_wifi_saved(GtkButton *b, gpointer data)
{
	GtkWidget *d, *box, *list, *sw;
	GRegex *re = g_regex_new("^\\s*(.*\\S)\\s+(psk|open|8021x|owe|wep)(\\s|$)", 0, 0, NULL);
	char *out = run_out(ARGV("iwctl", "known-networks", "list"));
	char **lines = g_strsplit(out ? out : "", "\n", -1), **l;
	int n = 0;

	d = gtk_dialog_new_with_buttons("Saved networks", GTK_WINDOW(win),
					GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					"Close", GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_default_size(GTK_WINDOW(d), 440, 360);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 14);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_box_pack_start(GTK_BOX(box),
		label_new("Saved networks connect by themselves when they are in range. "
			  "Forget one to stop that (and to type its password again).", "hx-dim"),
		FALSE, FALSE, 0);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	add_class(list, "hx-card");
	gtk_list_box_set_placeholder(GTK_LIST_BOX(list), label_new("  No saved networks.", "hx-dim"));
	gtk_container_add(GTK_CONTAINER(sw), list);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	for (l = lines; *l; l++) {
		GMatchInfo *mi;

		if (g_regex_match(re, *l, 0, &mi)) {
			char *name = g_match_info_fetch(mi, 1);
			GtkWidget *btn = button_new("Forget", G_CALLBACK(on_forget), NULL);

			g_object_set_data_full(G_OBJECT(btn), "ssid", name, g_free);
			add_row(list, name, NULL, btn, NULL);
			n++;
		}
		g_match_info_free(mi);
	}
	gtk_widget_show_all(d);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	g_strfreev(lines);
	g_free(out);
	g_regex_unref(re);
	wifi_scan();
}

static char *
iface_ipv4(const char *iface)
{
	char *out = run_out(ARGV("ip", "-4", "addr", "show", "dev", iface));
	char *p = out ? strstr(out, "inet ") : NULL, *ip = NULL;

	if (p) {
		p += 5;
		ip = g_strndup(p, strcspn(p, " \n"));
	}
	g_free(out);
	return ip;
}

static void on_wired_reconnect(GtkButton *b, gpointer data);

static void
wired_refresh(void)
{
	GDir *d = g_dir_open("/sys/class/net", 0, NULL);
	const char *n;
	char *out, *gw = NULL, *dns, **lines, **l;
	GString *ns = g_string_new(NULL);
	int count = 0;
	GtkWidget *row;

	list_clear(net.wired);
	while (d && (n = g_dir_read_name(d))) {
		char *dev, *wl, *carrier_path, *carrier, *ip, *sub;

		if (!g_str_has_prefix(n, "eth") && !g_str_has_prefix(n, "en") &&
		    !g_str_has_prefix(n, "usb"))
			continue;
		dev = g_strdup_printf("/sys/class/net/%s/device", n);
		wl = g_strdup_printf("/sys/class/net/%s/wireless", n);
		if (g_file_test(dev, G_FILE_TEST_EXISTS) && !g_file_test(wl, G_FILE_TEST_EXISTS)) {
			carrier_path = g_strdup_printf("/sys/class/net/%s/carrier", n);
			carrier = read_file(carrier_path);
			ip = iface_ipv4(n);
			if (g_strcmp0(carrier, "1") == 0)
				sub = g_strdup_printf("Cable connected · %s",
						      ip ? ip : "waiting for an address");
			else
				sub = g_strdup("No cable connected");
			add_row(net.wired, n, sub, NULL, NULL);
			count++;
			g_free(carrier_path);
			g_free(carrier);
			g_free(ip);
			g_free(sub);
		}
		g_free(dev);
		g_free(wl);
	}
	if (d)
		g_dir_close(d);
	if (!count)
		add_row(net.wired, "No wired network card",
			"USB-Ethernet adapters work too: just plug one in.", NULL, NULL);

	out = run_out(ARGV("ip", "route"));
	lines = g_strsplit(out ? out : "", "\n", -1);
	for (l = lines; *l && !gw; l++) {
		char **f = g_strsplit(*l, " ", -1);

		if (f[0] && f[1] && f[2] && !strcmp(f[0], "default") && !strcmp(f[1], "via"))
			gw = g_strdup_printf("%s (%s)", f[2],
					     f[3] && f[4] && !strcmp(f[3], "dev") ? f[4] : "?");
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(out);
	dns = read_file("/etc/resolv.conf");
	lines = g_strsplit(dns ? dns : "", "\n", -1);
	for (l = lines; *l; l++)
		if (g_str_has_prefix(*l, "nameserver ")) {
			/* resolv.conf lines may end with a "# interface" comment */
			char *hash = strchr(*l, '#');

			if (hash)
				*hash = '\0';
			g_string_append_printf(ns, "%s%s", ns->len ? ", " : "", g_strstrip(*l + 11));
		}
	g_strfreev(lines);
	g_free(dns);

	add_row(net.wired, "Internet gateway", gw ? gw : "None: not connected to a network",
		NULL, NULL);
	add_row(net.wired, "DNS servers", ns->len ? ns->str : "None", NULL, NULL);
	row = add_row(net.wired, "Reconnect the wired network",
		      "Asks the router for a new address.",
		      button_new("Reconnect", G_CALLBACK(on_wired_reconnect), NULL), NULL);
	gtk_widget_set_sensitive(row, count > 0);
	gtk_widget_show_all(net.wired);
	g_free(gw);
	g_string_free(ns, TRUE);
}

static gboolean
wired_refresh_later(gpointer data)
{
	wired_refresh();
	return G_SOURCE_REMOVE;
}

static void
wired_reconnected(gboolean ok, const char *out, const char *err, gpointer data)
{
	if (!ok && *err)
		message(GTK_MESSAGE_ERROR, "Could not reconnect", err);
	g_timeout_add_seconds(4, wired_refresh_later, NULL);
}

static void
on_wired_reconnect(GtkButton *b, gpointer data)
{
	as_admin(ARGV("wired-restart"), NULL, wired_reconnected, NULL);
}

static void
on_wifi_refresh(GtkButton *b, gpointer data)
{
	wifi_update_status();
	wifi_scan();
	wired_refresh();
}

/* Why there is no Wi-Fi interface: VM, card without driver, or none */
static const char *
wifi_missing_reason(void)
{
	GDir *d = g_dir_open("/sys/bus/pci/devices", 0, NULL);
	const char *n;
	gboolean card = FALSE;
	char *cpu;
	const char *why;

	while (d && !card && (n = g_dir_read_name(d))) {
		char *p = g_strdup_printf("/sys/bus/pci/devices/%s/class", n), *cls = read_file(p);

		card = g_strcmp0(cls, "0x028000") == 0;
		g_free(cls);
		g_free(p);
	}
	if (d)
		g_dir_close(d);
	cpu = read_file("/proc/cpuinfo");
	if (card)
		why = "A Wi-Fi card was found, but it did not start: its driver or "
		      "firmware is probably missing.";
	else if (cpu && strstr(cpu, " hypervisor"))
		why = "This is a virtual machine, which has no Wi-Fi: it uses the wired network.";
	else
		why = "No Wi-Fi card was found. Check that Wi-Fi is not turned off "
		      "in the BIOS/UEFI setup.";
	g_free(cpu);
	return why;
}

static GtkWidget *
build_network(void)
{
	GtkWidget *page = page_new("Network & internet", NULL), *list, *head;
	GDir *d = g_dir_open("/sys/class/net", 0, NULL);
	const char *n;

	while (d && !net.iface && (n = g_dir_read_name(d))) {
		char *p = g_strdup_printf("/sys/class/net/%s/wireless", n);

		if (g_file_test(p, G_FILE_TEST_IS_DIR))
			net.iface = g_strdup(n);
		g_free(p);
	}
	if (d)
		g_dir_close(d);

	list = section("Wi-Fi");
	if (net.iface) {
		net.power = gtk_switch_new();
		add_row(list, "Wi-Fi", "", net.power, &net.status);
		g_signal_connect(net.power, "notify::active", G_CALLBACK(on_wifi_power), NULL);

		head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		net.tools = head;
		gtk_widget_set_margin_top(head, 14);
		gtk_box_pack_start(GTK_BOX(head), label_new("Available networks", "hx-section-inline"),
				   FALSE, FALSE, 0);
		net.spinner = gtk_spinner_new();
		gtk_box_pack_start(GTK_BOX(head), net.spinner, FALSE, FALSE, 0);
		gtk_box_pack_end(GTK_BOX(head), button_new("Saved networks...",
				 G_CALLBACK(on_wifi_saved), NULL), FALSE, FALSE, 0);
		gtk_box_pack_end(GTK_BOX(head), button_new("Hidden network...",
				 G_CALLBACK(on_wifi_hidden), NULL), FALSE, FALSE, 0);
		gtk_box_pack_end(GTK_BOX(head), button_new("Refresh",
				 G_CALLBACK(on_wifi_refresh), NULL), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(page_box), head, FALSE, FALSE, 0);
		net.list = section(NULL);
		gtk_list_box_set_placeholder(GTK_LIST_BOX(net.list),
			label_new("  Looking for networks...", "hx-dim"));
	} else {
		add_row(list, "No Wi-Fi adapter", wifi_missing_reason(), NULL, NULL);
	}

	net.wired = section("Wired network (Ethernet)");
	return page;
}

static void
network_shown(void)
{
	wired_refresh();
	if (net.iface) {
		wifi_update_status();
		wifi_scan();
	}
}

/*
 * Bluetooth (through /usr/bin/hexa-bluetooth, which the taskbar uses too)
 */

#define HEXA_BT		"/usr/bin/hexa-bluetooth"
#define BT_SCAN_SECONDS	"20"

static struct {
	GtkWidget *power, *status, *body, *spinner, *search, *paired, *found, *found_note;
	gboolean available, powered, filling, scanning, busy, refreshing, again;
	guint poll;
	char *last_list;
} bt;

static void bt_refresh(void);
static void bt_scan(void);

static gboolean
bt_has_adapter(void)
{
	GDir *d = g_dir_open("/sys/class/bluetooth", 0, NULL);
	const char *n;
	gboolean found = FALSE;

	while (d && !found && (n = g_dir_read_name(d)))
		found = g_str_has_prefix(n, "hci");
	if (d)
		g_dir_close(d);
	return found;
}

static void
bt_spinner_update(void)
{
	if (bt.scanning || bt.busy)
		gtk_spinner_start(GTK_SPINNER(bt.spinner));
	else
		gtk_spinner_stop(GTK_SPINNER(bt.spinner));
	gtk_widget_set_sensitive(bt.search, !bt.scanning);
	gtk_label_set_text(GTK_LABEL(bt.found_note), bt.scanning ?
		"  Looking for devices..." :
		"  Nothing found. Put your device in pairing mode and press Search.");
}

/* bluetoothctl names a device that has not said its name after its address
 * ("AA-BB-CC-DD-EE-FF"): not worth showing */
static gboolean
bt_unnamed(const char *addr, const char *name)
{
	char *a = g_strdelimit(g_strdup(name), "-", ':');
	gboolean same = g_ascii_strcasecmp(a, addr) == 0;

	g_free(a);
	return same || !*name;
}

static void
bt_action_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	char *title = data;

	bt.busy = FALSE;
	bt_spinner_update();
	if (!ok)
		message(GTK_MESSAGE_ERROR, title, *err ? err :
			"The device did not answer. Make sure it is turned on and close by.");
	g_free(title);
	bt_refresh();
}

static void
bt_device_action(const char *verb, const char *addr, const char *name)
{
	const char *doing = strcmp(verb, "pair") == 0 ? "Pairing with" :
			    strcmp(verb, "connect") == 0 ? "Connecting to" :
			    strcmp(verb, "disconnect") == 0 ? "Disconnecting" : "Removing";
	const char *fail = strcmp(verb, "pair") == 0 ? "Could not pair with" :
			   strcmp(verb, "connect") == 0 ? "Could not connect to" :
			   strcmp(verb, "disconnect") == 0 ? "Could not disconnect" :
			   "Could not remove";
	char *text;

	if (bt.busy)
		return;
	bt.busy = TRUE;
	bt_spinner_update();
	text = g_strdup_printf("%s %s...", doing, name);
	set_sub(bt.status, text);
	g_free(text);
	run_job(ARGV(HEXA_BT, verb, addr), NULL, bt_action_done,
		g_strdup_printf("%s \"%s\"", fail, name));
}

static void
on_bt_device(GtkButton *b, gpointer data)
{
	const char *verb = data;
	const char *addr = g_object_get_data(G_OBJECT(b), "addr");
	const char *name = g_object_get_data(G_OBJECT(b), "name");

	if (strcmp(verb, "remove") == 0) {
		char *title = g_strdup_printf("Remove \"%s\"?", name);
		gboolean yes = confirm(title, "To use it again, you will have to pair it again "
				       "(put it in pairing mode and press Search).", "Remove");

		g_free(title);
		if (!yes)
			return;
	}
	bt_device_action(verb, addr, name);
}

static GtkWidget *
bt_button(const char *label, const char *verb, const char *addr, const char *name)
{
	GtkWidget *b = button_new(label, G_CALLBACK(on_bt_device), (gpointer) verb);

	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	g_object_set_data_full(G_OBJECT(b), "addr", g_strdup(addr), g_free);
	g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name), g_free);
	gtk_widget_set_sensitive(b, !bt.busy);
	return b;
}

static void
bt_add_device(GtkWidget *list, const char *addr, const char *name, gboolean paired,
	      gboolean connected)
{
	GtkWidget *row, *box, *text;

	row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	add_class(box, "hx-row");
	gtk_box_pack_start(GTK_BOX(box),
		gtk_image_new_from_icon_name(connected ? "bluetooth-active-symbolic" :
					     "bluetooth-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR),
		FALSE, FALSE, 0);
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(text), label_new(name, connected ? "hx-strong" : NULL),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), label_new(connected ? "Connected" :
			   paired ? "Paired, not connected" : addr, "hx-dim"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
	if (paired) {
		gtk_box_pack_end(GTK_BOX(box), bt_button("Remove", "remove", addr, name),
				 FALSE, FALSE, 0);
		gtk_box_pack_end(GTK_BOX(box), connected ?
				 bt_button("Disconnect", "disconnect", addr, name) :
				 bt_button("Connect", "connect", addr, name), FALSE, FALSE, 0);
	} else {
		gtk_box_pack_end(GTK_BOX(box), bt_button("Pair", "pair", addr, name),
				 FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(row), box);
	gtk_container_add(GTK_CONTAINER(list), row);
	gtk_widget_show_all(row);
}

static void
bt_got_list(gboolean ok, const char *out, const char *err, gpointer data)
{
	char **lines, **l;
	int n_connected = 0;
	char *first = NULL, *text;

	bt.refreshing = FALSE;
	/* rebuild only on a change: the rows (and their buttons) stay put while
	 * the list is polled during a search */
	if (g_strcmp0(out, bt.last_list) != 0 || bt.busy) {
		g_free(bt.last_list);
		bt.last_list = bt.busy ? NULL : g_strdup(out);
		list_clear(bt.paired);
		list_clear(bt.found);
		lines = g_strsplit(out, "\n", -1);
		for (l = lines; *l; l++) {
			char **f = g_strsplit(*l, "\t", 4);

			if (g_strv_length(f) == 4) {
				gboolean paired = f[1][0] == '1', connected = f[2][0] == '1';

				if (paired)
					bt_add_device(bt.paired, f[0], f[3], TRUE, connected);
				else if (!bt_unnamed(f[0], f[3]))
					bt_add_device(bt.found, f[0], f[3], FALSE, FALSE);
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
	}

	if (!bt.busy) {
		lines = g_strsplit(out, "\n", -1);
		for (l = lines; *l; l++) {
			char **f = g_strsplit(*l, "\t", 4);

			if (g_strv_length(f) == 4 && f[2][0] == '1' && n_connected++ == 0)
				first = g_strdup(f[3]);
			g_strfreev(f);
		}
		g_strfreev(lines);
		if (n_connected == 0)
			text = g_strdup("On, nothing connected");
		else if (n_connected == 1)
			text = g_strdup_printf("Connected to %s", first);
		else
			text = g_strdup_printf("Connected to %s and %d more", first, n_connected - 1);
		set_sub(bt.status, text);
		g_free(text);
		g_free(first);
	}
	if (bt.again) {
		bt.again = FALSE;
		bt_refresh();
	}
}

static void
bt_got_status(gboolean ok, const char *out, const char *err, gpointer data)
{
	gboolean was = bt.powered;

	bt.powered = strstr(out, "powered 1") != NULL;
	bt.filling = TRUE;
	gtk_switch_set_active(GTK_SWITCH(bt.power), bt.powered);
	bt.filling = FALSE;
	gtk_widget_set_visible(bt.body, bt.powered);
	if (!bt.powered) {
		bt.refreshing = FALSE;
		if (!bt.busy)
			set_sub(bt.status, strstr(out, "powered") ? "Bluetooth is turned off." :
				"The Bluetooth service is not answering.");
		g_clear_pointer(&bt.last_list, g_free);
		bt.again = FALSE;
		return;
	}
	run_job(ARGV(HEXA_BT, "list"), NULL, bt_got_list, NULL);
	if (!was && !bt.scanning)
		bt_scan();
}

static void
bt_refresh(void)
{
	if (!bt.available)
		return;
	if (bt.refreshing) {
		bt.again = TRUE;
		return;
	}
	bt.refreshing = TRUE;
	run_job(ARGV(HEXA_BT, "status"), NULL, bt_got_status, NULL);
}

static gboolean
bt_poll(gpointer data)
{
	bt_refresh();
	return G_SOURCE_CONTINUE;
}

static void
bt_scan_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	bt.scanning = FALSE;
	if (bt.poll) {
		g_source_remove(bt.poll);
		bt.poll = 0;
	}
	bt_spinner_update();
	bt_refresh();
}

static void
bt_scan(void)
{
	if (!bt.available || !bt.powered || bt.scanning)
		return;
	bt.scanning = TRUE;
	bt_spinner_update();
	run_job(ARGV(HEXA_BT, "scan", BT_SCAN_SECONDS), NULL, bt_scan_done, NULL);
	/* show what turns up while the search goes on */
	bt.poll = g_timeout_add_seconds(3, bt_poll, NULL);
}

static void
on_bt_search(GtkButton *b, gpointer data)
{
	bt_scan();
}

static void
bt_power_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	if (!ok)
		message(GTK_MESSAGE_ERROR, "Could not turn Bluetooth on or off",
			*err ? err : out);
	bt_refresh();
}

static void
on_bt_power(GObject *sw, GParamSpec *ps, gpointer data)
{
	gboolean on;

	if (bt.filling)
		return;
	on = gtk_switch_get_active(GTK_SWITCH(bt.power));
	set_sub(bt.status, on ? "Turning on..." : "Turning off...");
	run_job(ARGV(HEXA_BT, "power", on ? "on" : "off"), NULL, bt_power_done, NULL);
}

static GtkWidget *
build_bluetooth(void)
{
	GtkWidget *page = page_new("Bluetooth", "Headphones, speakers, game controllers, "
				   "mice and keyboards."), *list, *head, *outer, *note;
	char *cpu;

	bt.available = bt_has_adapter();
	list = section(NULL);
	if (!bt.available) {
		cpu = read_file("/proc/cpuinfo");
		add_row(list, "No Bluetooth adapter",
			cpu && strstr(cpu, " hypervisor") ?
			"This is a virtual machine, which has no Bluetooth. A USB Bluetooth "
			"adapter can be passed through to it." :
			"No Bluetooth adapter was found. Check that Bluetooth is not turned "
			"off in the BIOS/UEFI setup, or plug in a USB Bluetooth adapter.",
			NULL, NULL);
		g_free(cpu);
		return page;
	}
	bt.power = gtk_switch_new();
	add_row(list, "Bluetooth", "", bt.power, &bt.status);
	g_signal_connect(bt.power, "notify::active", G_CALLBACK(on_bt_power), NULL);

	/* everything below shows only while Bluetooth is on */
	outer = page_box;
	bt.body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start(GTK_BOX(outer), bt.body, FALSE, FALSE, 0);
	page_box = bt.body;

	bt.paired = section("Your devices");
	gtk_list_box_set_placeholder(GTK_LIST_BOX(bt.paired),
		label_new("  No devices yet: add one below.", "hx-dim"));

	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(head, 14);
	gtk_box_pack_start(GTK_BOX(head), label_new("Add a device", "hx-section-inline"),
			   FALSE, FALSE, 0);
	bt.spinner = gtk_spinner_new();
	gtk_box_pack_start(GTK_BOX(head), bt.spinner, FALSE, FALSE, 0);
	bt.search = button_new("Search", G_CALLBACK(on_bt_search), NULL);
	gtk_box_pack_end(GTK_BOX(head), bt.search, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page_box), head, FALSE, FALSE, 0);
	note = label_new("Put the device in pairing mode first: usually by holding its "
			     "power or Bluetooth button until a light blinks (on a PlayStation "
			     "controller, hold PS and Create). Devices that ask for a PIN code "
			     "cannot be paired here yet.", "hx-dim");
	gtk_box_pack_start(GTK_BOX(page_box), note, FALSE, FALSE, 0);
	bt.found = section(NULL);
	bt.found_note = label_new("", "hx-dim");
	gtk_list_box_set_placeholder(GTK_LIST_BOX(bt.found), bt.found_note);

	page_box = outer;
	bt_spinner_update();
	return page;
}

static void
bluetooth_shown(void)
{
	if (!bt.available)
		return;
	g_clear_pointer(&bt.last_list, g_free);
	bt_refresh();
	bt_scan();
}

/*
 * Sound
 */

static struct {
	GtkWidget *box, *msg, *combo[2], *scale[2], *mute[2];
	double pending[2];
	guint timer[2];
	gboolean filling;
} snd;

/* Microphone test: a level meter fed by "parec" (raw 16-bit samples of the
 * default input, through pipewire-pulse) */
static struct {
	GtkWidget *button, *level;
	GPid pid;
	guint watch, decay, stop_timer;
	double peak;
} mic;

static void mic_test_stop(void);

static const char *const snd_default[2] = { "@DEFAULT_AUDIO_SINK@", "@DEFAULT_AUDIO_SOURCE@" };

static void
sound_read_volume(int i)
{
	char *out = run_out(ARGV("wpctl", "get-volume", snd_default[i]));
	char *p = out ? strstr(out, "Volume:") : NULL;
	double v = p ? g_ascii_strtod(p + 7, NULL) : 0;

	gtk_range_set_value(GTK_RANGE(snd.scale[i]), v * 100);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(snd.mute[i]),
				     out && strstr(out, "MUTED"));
	g_free(out);
}

static void
sound_refresh(void)
{
	char *st = run_out(ARGV("wpctl", "status"));
	GRegex *re;
	char **lines, **l;
	int i, mode = -1;
	gboolean audio = FALSE;

	gtk_widget_set_visible(snd.msg, !st);
	gtk_widget_set_sensitive(snd.box, st != NULL);
	if (!st)
		return;
	snd.filling = TRUE;
	for (i = 0; i < 2; i++)
		gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(snd.combo[i]));
	re = g_regex_new("(\\*)?\\s+(\\d+)\\.\\s+(.+?)\\s*\\[vol:", 0, 0, NULL);
	lines = g_strsplit(st, "\n", -1);
	for (l = lines; *l; l++) {
		GMatchInfo *mi = NULL;

		if (g_str_has_prefix(*l, "Audio")) {
			audio = TRUE;
			continue;
		}
		if (g_str_has_prefix(*l, "Video") || g_str_has_prefix(*l, "Settings"))
			audio = FALSE;
		if (!audio)
			continue;
		if (strstr(*l, "Sinks:"))
			mode = 0;
		else if (strstr(*l, "Sources:"))
			mode = 1;
		else if (strstr(*l, "Filters:") || strstr(*l, "Streams:") || strstr(*l, "Devices:"))
			mode = -1;
		else if (mode >= 0 && g_regex_match(re, *l, 0, &mi)) {
			char *star = g_match_info_fetch(mi, 1), *id = g_match_info_fetch(mi, 2);
			char *name = g_match_info_fetch(mi, 3);

			gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(snd.combo[mode]), id, name);
			if (star && *star)
				gtk_combo_box_set_active_id(GTK_COMBO_BOX(snd.combo[mode]), id);
			g_free(star);
			g_free(id);
			g_free(name);
			g_match_info_free(mi);
		} else {
			g_match_info_free(mi);
		}
	}
	g_strfreev(lines);
	g_regex_unref(re);
	g_free(st);
	for (i = 0; i < 2; i++)
		sound_read_volume(i);
	snd.filling = FALSE;
}

static void
on_sound_device(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);
	char *out;

	if (snd.filling || !id)
		return;
	out = run_out(ARGV("wpctl", "set-default", id));
	g_free(out);
	/* a running microphone test follows the new microphone */
	if (GPOINTER_TO_INT(data) == 1 && mic.pid) {
		mic_test_stop();
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mic.button), TRUE);
	}
	snd.filling = TRUE;
	sound_read_volume(GPOINTER_TO_INT(data));
	snd.filling = FALSE;
}

static gboolean
sound_apply_volume(gpointer data)
{
	int i = GPOINTER_TO_INT(data);
	char v[G_ASCII_DTOSTR_BUF_SIZE];

	g_ascii_formatd(v, sizeof v, "%.2f", snd.pending[i] / 100.0);
	run_job(ARGV("wpctl", "set-volume", snd_default[i], v), NULL, NULL, NULL);
	snd.timer[i] = 0;
	return G_SOURCE_REMOVE;
}

static void
on_sound_volume(GtkRange *r, gpointer data)
{
	int i = GPOINTER_TO_INT(data);

	if (snd.filling)
		return;
	snd.pending[i] = gtk_range_get_value(r);
	if (!snd.timer[i])	/* at most ~12 changes a second while dragging */
		snd.timer[i] = g_timeout_add(80, sound_apply_volume, data);
}

static void
on_sound_mute(GtkToggleButton *b, gpointer data)
{
	int i = GPOINTER_TO_INT(data);
	gboolean m = gtk_toggle_button_get_active(b);

	gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(
		m ? (i ? "microphone-disabled-symbolic" : "audio-volume-muted-symbolic")
		  : (i ? "audio-input-microphone-symbolic" : "audio-volume-high-symbolic"),
		GTK_ICON_SIZE_BUTTON));
	if (snd.filling)
		return;
	run_job(ARGV("wpctl", "set-mute", snd_default[i], m ? "1" : "0"), NULL, NULL, NULL);
}

static gboolean
mic_test_read(GIOChannel *ch, GIOCondition cond, gpointer data)
{
	gint16 buf[2048];
	gsize n = 0, i;
	double peak = 0;

	if ((cond & G_IO_IN) &&
	    g_io_channel_read_chars(ch, (char *) buf, sizeof buf, &n, NULL) == G_IO_STATUS_NORMAL &&
	    n > 0) {
		for (i = 0; i < n / 2; i++) {
			double s = fabs((double) buf[i] / 32768.0);

			if (s > peak)
				peak = s;
		}
		/* square root: quiet speech still moves the bar */
		peak = sqrt(peak);
		if (peak > mic.peak)
			mic.peak = peak;
		return G_SOURCE_CONTINUE;
	}
	/* parec ended (no sound service, device gone) */
	mic.watch = 0;
	mic_test_stop();
	return G_SOURCE_REMOVE;
}

/* the bar falls back slowly, like a VU meter */
static gboolean
mic_test_decay(gpointer data)
{
	gtk_level_bar_set_value(GTK_LEVEL_BAR(mic.level), mic.peak);
	mic.peak *= 0.80;
	return G_SOURCE_CONTINUE;
}

static gboolean
mic_test_timeout(gpointer data)
{
	mic.stop_timer = 0;
	mic_test_stop();
	return G_SOURCE_REMOVE;
}

static void
mic_test_start(void)
{
	const char *argv[] = { "parec", "--raw", "--format=s16le", "--channels=1",
			       "--rate=16000", "--latency-msec=40",
			       "--client-name=hexaOS Settings", NULL };
	GIOChannel *ch;
	int out;

	if (mic.pid)
		return;
	if (!g_spawn_async_with_pipes(NULL, (char **) argv, NULL,
				      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
				      G_SPAWN_STDERR_TO_DEV_NULL,
				      NULL, NULL, &mic.pid, NULL, &out, NULL, NULL)) {
		mic.pid = 0;
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mic.button), FALSE);
		return;
	}
	ch = g_io_channel_unix_new(out);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_set_buffered(ch, FALSE);
	g_io_channel_set_close_on_unref(ch, TRUE);
	mic.watch = g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, mic_test_read, NULL);
	g_io_channel_unref(ch);
	mic.peak = 0;
	mic.decay = g_timeout_add(50, mic_test_decay, NULL);
	/* the microphone stays open only while someone is looking at it */
	mic.stop_timer = g_timeout_add_seconds(60, mic_test_timeout, NULL);
	gtk_button_set_label(GTK_BUTTON(mic.button), "Stop");
}

static void
mic_test_stop(void)
{
	if (mic.watch)
		g_source_remove(mic.watch);
	if (mic.decay)
		g_source_remove(mic.decay);
	if (mic.stop_timer)
		g_source_remove(mic.stop_timer);
	mic.watch = mic.decay = mic.stop_timer = 0;
	if (mic.pid) {
		kill(mic.pid, SIGTERM);
		waitpid(mic.pid, NULL, 0);
		g_spawn_close_pid(mic.pid);
		mic.pid = 0;
	}
	if (mic.level)
		gtk_level_bar_set_value(GTK_LEVEL_BAR(mic.level), 0);
	if (mic.button) {
		gtk_button_set_label(GTK_BUTTON(mic.button), "Test");
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mic.button)))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mic.button), FALSE);
	}
}

static void
on_mic_test(GtkToggleButton *b, gpointer data)
{
	if (gtk_toggle_button_get_active(b))
		mic_test_start();
	else if (mic.pid)
		mic_test_stop();
}

static void
on_mixer(GtkButton *b, gpointer data)
{
	g_spawn_command_line_async("foot --app-id=hexa-mixer --title=Mixer alsamixer", NULL);
}

static GtkWidget *
build_sound(void)
{
	GtkWidget *page = page_new("Sound", NULL), *list, *box;
	const char *title[2] = { "Output (speakers, headphones, HDMI)", "Input (microphone)" };
	int i;

	snd.msg = label_new("The sound service is not running. It starts with the desktop; "
			    "restarting the desktop usually brings it back.", "hx-warn");
	gtk_widget_set_no_show_all(snd.msg, TRUE);
	gtk_box_pack_start(GTK_BOX(page_box), snd.msg, FALSE, FALSE, 0);
	snd.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start(GTK_BOX(page_box), snd.box, FALSE, FALSE, 0);

	for (i = 0; i < 2; i++) {
		GtkWidget *saved = page_box;

		page_box = snd.box;
		list = section(title[i]);
		page_box = saved;
		snd.combo[i] = gtk_combo_box_text_new();
		gtk_widget_set_size_request(snd.combo[i], 300, -1);
		g_signal_connect(snd.combo[i], "changed", G_CALLBACK(on_sound_device),
				 GINT_TO_POINTER(i));
		add_row(list, i ? "Microphone" : "Play sound through", NULL, snd.combo[i], NULL);

		box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		snd.mute[i] = gtk_toggle_button_new();
		gtk_widget_set_tooltip_text(snd.mute[i], "Mute");
		g_signal_connect(snd.mute[i], "toggled", G_CALLBACK(on_sound_mute),
				 GINT_TO_POINTER(i));
		on_sound_mute(GTK_TOGGLE_BUTTON(snd.mute[i]), GINT_TO_POINTER(i));
		snd.scale[i] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
		gtk_scale_set_digits(GTK_SCALE(snd.scale[i]), 0);
		gtk_scale_set_value_pos(GTK_SCALE(snd.scale[i]), GTK_POS_RIGHT);
		gtk_widget_set_size_request(snd.scale[i], 260, -1);
		g_signal_connect(snd.scale[i], "value-changed", G_CALLBACK(on_sound_volume),
				 GINT_TO_POINTER(i));
		gtk_box_pack_start(GTK_BOX(box), snd.mute[i], FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(box), snd.scale[i], FALSE, FALSE, 0);
		add_row(list, "Volume", NULL, box, NULL);
		if (i == 1) {
			box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
			mic.level = gtk_level_bar_new_for_interval(0, 1);
			gtk_level_bar_set_mode(GTK_LEVEL_BAR(mic.level), GTK_LEVEL_BAR_MODE_CONTINUOUS);
			gtk_widget_set_size_request(mic.level, 220, 10);
			gtk_widget_set_valign(mic.level, GTK_ALIGN_CENTER);
			mic.button = gtk_toggle_button_new_with_label("Test");
			g_signal_connect(mic.button, "toggled", G_CALLBACK(on_mic_test), NULL);
			gtk_box_pack_start(GTK_BOX(box), mic.level, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(box), mic.button, FALSE, FALSE, 0);
			add_row(list, "Test your microphone",
				"Press Test and speak: the bar moves when the microphone hears you. "
				"No movement? Pick another microphone above, or check Mute.",
				box, NULL);
		}
	}
	if (g_file_test("/usr/bin/alsamixer", G_FILE_TEST_EXISTS)) {
		list = section("Advanced");
		add_row(list, "Sound card mixer", "Every volume control of the sound card (alsamixer).",
			button_new("Open mixer", G_CALLBACK(on_mixer), NULL), NULL);
	}
	return page;
}

/*
 * Display
 */

static struct {
	char *backlight;
	int max;
	GtkWidget *scale;
	guint timer;
} bright;

static gboolean
brightness_apply(gpointer data)
{
	char path[300], val[16];
	int v = (int) (gtk_range_get_value(GTK_RANGE(bright.scale)) * bright.max / 100 + 0.5);
	FILE *f;

	bright.timer = 0;
	if (v < 1)
		v = 1;
	g_snprintf(val, sizeof val, "%d", v);
	g_snprintf(path, sizeof path, "/sys/class/backlight/%s/brightness", bright.backlight);
	if ((f = fopen(path, "w"))) {
		fputs(val, f);
		fclose(f);
	} else {
		as_admin(ARGV("brightness", bright.backlight, val), NULL, admin_report,
			 "Could not change the brightness");
	}
	return G_SOURCE_REMOVE;
}

static void
on_brightness(GtkRange *r, gpointer data)
{
	if (!bright.timer)
		bright.timer = g_timeout_add(60, brightness_apply, NULL);
}

static GtkWidget *
build_display(void)
{
	GtkWidget *page = page_new("Display",
		"Screen changes take effect when the desktop restarts."), *list;
	GDir *d;
	const char *n;
	GPtrArray *cards = g_ptr_array_new_with_free_func(g_free);
	guint k;
	int connected = 0;

	list = section("Brightness");
	d = g_dir_open("/sys/class/backlight", 0, NULL);
	if (d && (n = g_dir_read_name(d)))
		bright.backlight = g_strdup(n);
	if (d)
		g_dir_close(d);
	if (bright.backlight) {
		char *p = g_strdup_printf("/sys/class/backlight/%s/max_brightness", bright.backlight);
		char *mx = read_file(p), *cur;

		bright.max = mx ? atoi(mx) : 100;
		g_free(p);
		g_free(mx);
		if (bright.max <= 0)
			bright.max = 100;
		p = g_strdup_printf("/sys/class/backlight/%s/brightness", bright.backlight);
		cur = read_file(p);
		bright.scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 5, 100, 1);
		gtk_scale_set_digits(GTK_SCALE(bright.scale), 0);
		gtk_scale_set_value_pos(GTK_SCALE(bright.scale), GTK_POS_RIGHT);
		gtk_widget_set_size_request(bright.scale, 260, -1);
		gtk_range_set_value(GTK_RANGE(bright.scale),
				    cur ? atoi(cur) * 100.0 / bright.max : 100);
		g_signal_connect(bright.scale, "value-changed", G_CALLBACK(on_brightness), NULL);
		add_row(list, "Brightness of the built-in screen", "Changes right away.",
			bright.scale, NULL);
		g_free(p);
		g_free(cur);
	} else {
		add_row(list, "This screen has no brightness control",
			"External monitors and virtual machines use their own buttons.", NULL, NULL);
	}

	/* connected screens: /sys/class/drm/card0-eDP-1 -> eDP-1 */
	d = g_dir_open("/sys/class/drm", 0, NULL);
	while (d && (n = g_dir_read_name(d)))
		if (g_str_has_prefix(n, "card") && strchr(n, '-'))
			g_ptr_array_add(cards, g_strdup(n));
	if (d)
		g_dir_close(d);
	g_ptr_array_sort(cards, (GCompareFunc) g_strcmp0);

	/* Main screen: apps seen for the first time open there, and apps whose
	 * last screen is not connected (patches/weston/0006-*) */
	{
		GPtrArray *items = g_ptr_array_new_with_free_func(g_free);

		g_ptr_array_add(items, g_strdup(""));
		g_ptr_array_add(items, g_strdup("Automatic (the first screen)"));
		for (k = 0; k < cards->len; k++) {
			const char *card = g_ptr_array_index(cards, k), *name = strchr(card, '-') + 1;
			char *p = g_strdup_printf("/sys/class/drm/%s/status", card), *status = read_file(p);

			if (g_strcmp0(status, "connected") == 0) {
				gboolean builtin = g_str_has_prefix(name, "eDP") ||
						   g_str_has_prefix(name, "LVDS") ||
						   g_str_has_prefix(name, "DSI");

				g_ptr_array_add(items, g_strdup(name));
				g_ptr_array_add(items, builtin ?
						g_strdup_printf("%s (built-in screen)", name) :
						g_strdup(name));
				connected++;
			}
			g_free(status);
			g_free(p);
		}
		g_ptr_array_add(items, NULL);
		if (connected > 1) {
			list = section("Several screens");
			add_row(list, "Main screen",
				"The other screens are placed around it (see Position on each "
				"screen). Apps open on the screen they "
				"were last on. New apps, and apps whose screen is not connected, "
				"open on this one.",
				bound_combo("shell", "primary-output", "",
					    (const char *const *) items->pdata), NULL);
		}
		g_ptr_array_free(items, TRUE);
	}

	for (k = 0; k < cards->len; k++) {
		const char *card = g_ptr_array_index(cards, k), *name = strchr(card, '-') + 1;
		char *p = g_strdup_printf("/sys/class/drm/%s/status", card), *status = read_file(p);
		char *group, *title, *modes, **ml;
		GtkWidget *mode;
		int j;

		g_free(p);
		if (g_strcmp0(status, "connected") != 0) {
			g_free(status);
			continue;
		}
		g_free(status);
		group = g_strconcat("output ", name, NULL);
		title = g_strdup_printf("Screen %s", name);
		list = section(title);

		mode = bound_combo(group, "mode", "preferred",
				   ITEMS("preferred", "Recommended (the screen's own)"));
		p = g_strdup_printf("/sys/class/drm/%s/modes", card);
		modes = read_file(p);
		ml = g_strsplit(modes ? modes : "", "\n", -1);
		g_signal_handlers_block_by_func(mode, on_bound_combo, NULL);
		for (j = 0; ml[j]; j++) {
			char *m = g_strstrip(ml[j]);

			/* the list repeats a size once per refresh rate */
			if (!*m || gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode), m))
				continue;
			gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), m, m);
		}
		{
			char *cur = cfg_get(group, "mode", "preferred");

			gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode), cur);
			g_free(cur);
		}
		g_signal_handlers_unblock_by_func(mode, on_bound_combo, NULL);
		add_row(list, "Resolution", NULL, mode, NULL);
		add_row(list, "Zoom",
			"Bigger text and windows. 200% suits sharp laptop screens. "
			"At 125% and 150% the apps are enlarged, but the taskbar "
			"stays at 100%.",
			bound_combo(group, "scale", "1",
				    ITEMS("1", "100%", "1.25", "125%", "1.5", "150%",
					  "2", "200%", "3", "300%")), NULL);
		add_row(list, "Orientation", NULL,
			bound_combo(group, "transform", "normal",
				    ITEMS("normal", "Landscape",
					  "rotate-90", "Portrait",
					  "rotate-180", "Landscape (upside down)",
					  "rotate-270", "Portrait (upside down)")), NULL);
		/* where this screen goes next to the main one (patches/weston/0012-*) */
		if (connected > 1)
			add_row(list, "Position",
				"Where this screen is, seen from the main screen. The mouse "
				"crosses over on that side. Not used for the main screen.",
				bound_combo(group, "side", "right",
					    ITEMS("right", "Right of the main screen",
						  "left", "Left of the main screen",
						  "above", "Above the main screen",
						  "below", "Below the main screen")), NULL);
		g_strfreev(ml);
		g_free(modes);
		g_free(p);
		g_free(title);
		g_free(group);
	}
	g_ptr_array_free(cards, TRUE);
	return page;
}

/*
 * Personalization
 */

static struct {
	GtkWidget *preview, *chooser, *mode, *fit_row, *pic_row;
	gboolean filling;
} bg;

/* Weston colors are 0xAARRGGBB */
static void
hex_to_rgba(const char *s, GdkRGBA *c)
{
	guint32 v = (guint32) strtoul(s, NULL, 16);

	c->alpha = ((v >> 24) & 0xff) / 255.0;
	c->red = ((v >> 16) & 0xff) / 255.0;
	c->green = ((v >> 8) & 0xff) / 255.0;
	c->blue = (v & 0xff) / 255.0;
}

static char *
rgba_to_hex(const GdkRGBA *c, gboolean alpha)
{
	return g_strdup_printf("0x%02x%02x%02x%02x",
			       alpha ? (int) (c->alpha * 255 + 0.5) : 255,
			       (int) (c->red * 255 + 0.5), (int) (c->green * 255 + 0.5),
			       (int) (c->blue * 255 + 0.5));
}

static void
on_bound_color(GtkColorButton *b, gpointer data)
{
	GdkRGBA c;
	char *hex;

	gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &c);
	hex = rgba_to_hex(&c, gtk_color_chooser_get_use_alpha(GTK_COLOR_CHOOSER(b)));
	cfg_set(W_GROUP(b), W_KEY(b), hex);
	g_free(hex);
}

static GtkWidget *
bound_color(const char *group, const char *key, const char *def, gboolean alpha)
{
	char *v = cfg_get(group, key, def);
	GdkRGBA c;
	GtkWidget *b;

	hex_to_rgba(v, &c);
	if (!alpha)
		c.alpha = 1;
	b = gtk_color_button_new_with_rgba(&c);
	gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(b), alpha);
	bind_key(b, group, key);
	g_signal_connect(b, "color-set", G_CALLBACK(on_bound_color), NULL);
	g_free(v);
	return b;
}

static void
bg_update(void)
{
	char *path = cfg_get("shell", "background-image", DEFAULT_WALLPAPER);
	GdkPixbuf *pb = *path ? gdk_pixbuf_new_from_file_at_scale(path, 320, 180, TRUE, NULL) : NULL;
	gboolean picture = *path != '\0';

	if (pb) {
		gtk_image_set_from_pixbuf(GTK_IMAGE(bg.preview), pb);
		g_object_unref(pb);
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(bg.preview),
			picture ? "image-missing" : "preferences-desktop-wallpaper-symbolic",
			GTK_ICON_SIZE_DIALOG);
	}
	bg.filling = TRUE;
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(bg.mode), picture ? "picture" : "color");
	if (picture)
		gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(bg.chooser), path);
	bg.filling = FALSE;
	gtk_widget_set_sensitive(bg.pic_row, picture);
	gtk_widget_set_sensitive(bg.fit_row, picture);
	g_free(path);
}

static void
on_bg_file(GtkFileChooserButton *b, gpointer data)
{
	char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(b));

	if (f && !bg.filling) {
		cfg_set("shell", "background-image", f);
		bg_update();
	}
	g_free(f);
}

static void
on_bg_mode(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (bg.filling || !id)
		return;
	if (strcmp(id, "color") == 0)
		cfg_set("shell", "background-image", "");
	else
		cfg_set("shell", "background-image", DEFAULT_WALLPAPER);
	bg_update();
}

static void
on_bg_default(GtkButton *b, gpointer data)
{
	cfg_set("shell", "background-image", DEFAULT_WALLPAPER);
	bg_update();
}

static GtkWidget *
build_personal(void)
{
	GtkWidget *page = page_new("Personalization",
		"Changes here take effect when the desktop restarts."), *list, *box;
	GtkFileFilter *filter;

	list = section("Background");
	bg.preview = gtk_image_new();
	gtk_widget_set_size_request(bg.preview, 320, 180);
	add_class(bg.preview, "hx-preview");
	gtk_widget_set_halign(bg.preview, GTK_ALIGN_START);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	add_class(box, "hx-row");
	gtk_box_pack_start(GTK_BOX(box), bg.preview, FALSE, FALSE, 0);
	{
		GtkWidget *row = gtk_list_box_row_new();

		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_container_add(GTK_CONTAINER(row), box);
		gtk_container_add(GTK_CONTAINER(list), row);
	}
	bg.mode = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bg.mode), "picture", "Picture");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bg.mode), "color", "Solid color");
	g_signal_connect(bg.mode, "changed", G_CALLBACK(on_bg_mode), NULL);
	add_row(list, "Background", NULL, bg.mode, NULL);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	bg.chooser = gtk_file_chooser_button_new("Choose a picture", GTK_FILE_CHOOSER_ACTION_OPEN);
	gtk_widget_set_size_request(bg.chooser, 220, -1);
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Pictures");
	gtk_file_filter_add_pixbuf_formats(filter);
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(bg.chooser), filter);
	g_signal_connect(bg.chooser, "file-set", G_CALLBACK(on_bg_file), NULL);
	gtk_box_pack_start(GTK_BOX(box), bg.chooser, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), button_new("Default", G_CALLBACK(on_bg_default), NULL),
			   FALSE, FALSE, 0);
	bg.pic_row = add_row(list, "Picture", "PNG or JPEG.", box, NULL);
	bg.fit_row = add_row(list, "Fit", NULL,
		bound_combo("shell", "background-type", "scale-crop",
			    ITEMS("scale-crop", "Fill (crop to the screen)",
				  "scale", "Stretch",
				  "centered", "Center",
				  "tile", "Tile")), NULL);
	add_row(list, "Background color", "Shown around or instead of the picture.",
		bound_color("shell", "background-color", "0xff0b1220", FALSE), NULL);
	bg_update();

	list = section("Taskbar");
	add_row(list, "Taskbar color", "The color picker's opacity slider makes it see-through.",
		bound_color("shell", "panel-color", "0xe80b1220", TRUE), NULL);

	list = section("Effects");
	add_row(list, "Window animation", "When a window opens.",
		bound_combo("shell", "animation", "zoom",
			    ITEMS("zoom", "Zoom", "fade", "Fade", "none", "None")), NULL);
	add_row(list, "Fade in the desktop when it starts", NULL,
		bound_switch("shell", "startup-animation", "fade", "none", "fade"), NULL);
	add_row(list, "Dim the windows that are not in use", NULL,
		bound_switch("shell", "focus-animation", "dim-layer", "none", "none"), NULL);
	return page;
}

/*
 * Keyboard
 */

typedef struct {
	char *name, *desc;
} Pair;

static struct {
	GPtrArray *layouts;		/* Pair */
	GHashTable *variants;		/* layout -> GPtrArray of Pair */
	GtkWidget *layout, *variant;
	gboolean filling;
} kb;

static void
pair_free(gpointer p)
{
	Pair *x = p;

	g_free(x->name);
	g_free(x->desc);
	g_free(x);
}

static int
pair_cmp(gconstpointer a, gconstpointer b)
{
	const Pair *x = *(Pair *const *) a, *y = *(Pair *const *) b;

	return g_utf8_collate(x->desc, y->desc);
}

static void
xkb_load(void)
{
	char *txt = read_file(XKB_LIST), **lines, **l;
	int sect = 0;

	kb.layouts = g_ptr_array_new_with_free_func(pair_free);
	kb.variants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					    (GDestroyNotify) g_ptr_array_unref);
	if (!txt)
		return;
	lines = g_strsplit(txt, "\n", -1);
	for (l = lines; *l; l++) {
		char *s, *sp;
		Pair *p;

		if ((*l)[0] == '!') {
			sect = strstr(*l, "variant") ? 2 : strstr(*l, "layout") ? 1 : 0;
			continue;
		}
		s = g_strstrip(*l);
		if (!sect || !*s || !(sp = strpbrk(s, " \t")))
			continue;
		p = g_new0(Pair, 1);
		p->name = g_strndup(s, sp - s);
		p->desc = g_strdup(g_strstrip(sp));
		if (sect == 1) {
			g_ptr_array_add(kb.layouts, p);
		} else {
			char *colon = strchr(p->desc, ':'), *lay, *d;
			GPtrArray *a;

			if (!colon) {
				pair_free(p);
				continue;
			}
			lay = g_strndup(p->desc, colon - p->desc);
			d = g_strdup(g_strstrip(colon + 1));
			g_free(p->desc);
			p->desc = d;
			a = g_hash_table_lookup(kb.variants, lay);
			if (!a) {
				a = g_ptr_array_new_with_free_func(pair_free);
				g_hash_table_insert(kb.variants, g_strdup(lay), a);
			}
			g_ptr_array_add(a, p);
			g_free(lay);
		}
	}
	g_strfreev(lines);
	g_free(txt);
	g_ptr_array_sort(kb.layouts, pair_cmp);
}

static void
kb_fill_variants(const char *layout, const char *current)
{
	GPtrArray *a = g_hash_table_lookup(kb.variants, layout);
	guint i;

	kb.filling = TRUE;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(kb.variant));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kb.variant), "", "Standard");
	for (i = 0; a && i < a->len; i++) {
		Pair *p = g_ptr_array_index(a, i);

		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kb.variant), p->name, p->desc);
	}
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(kb.variant), current ? current : "")) {
		/* a variant xkb knows but the list does not name (e.g. abnt2):
		 * keep it selectable instead of showing "Standard" */
		if (current && *current) {
			gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kb.variant), current, current);
			gtk_combo_box_set_active_id(GTK_COMBO_BOX(kb.variant), current);
		} else {
			gtk_combo_box_set_active_id(GTK_COMBO_BOX(kb.variant), "");
		}
	}
	kb.filling = FALSE;
}

static void
on_kb_layout(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (kb.filling || !id)
		return;
	cfg_set("keyboard", "keymap_layout", id);
	cfg_set("keyboard", "keymap_variant", "");
	kb_fill_variants(id, "");
}

static void
on_kb_variant(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (!kb.filling && id)
		cfg_set("keyboard", "keymap_variant", id);
}

static GtkWidget *
build_keyboard(void)
{
	GtkWidget *page = page_new("Keyboard",
		"Changes here take effect when the desktop restarts."), *list, *entry;
	char *layout = cfg_get("keyboard", "keymap_layout", "us");
	char *variant = cfg_get("keyboard", "keymap_variant", "");
	guint i;

	xkb_load();
	list = section("Layout");
	kb.layout = gtk_combo_box_text_new();
	gtk_widget_set_size_request(kb.layout, 300, -1);
	for (i = 0; i < kb.layouts->len; i++) {
		Pair *p = g_ptr_array_index(kb.layouts, i);

		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kb.layout), p->name, p->desc);
	}
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(kb.layout), layout)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kb.layout), layout, layout);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(kb.layout), layout);
	}
	g_signal_connect(kb.layout, "changed", G_CALLBACK(on_kb_layout), NULL);
	add_row(list, "Keyboard layout", "The letters printed on your keys.", kb.layout, NULL);
	kb.variant = gtk_combo_box_text_new();
	gtk_widget_set_size_request(kb.variant, 300, -1);
	kb_fill_variants(layout, variant);
	g_signal_connect(kb.variant, "changed", G_CALLBACK(on_kb_variant), NULL);
	add_row(list, "Variant", "e.g. ABNT2 for Brazilian keyboards, or international "
		"(dead keys) for US keyboards.", kb.variant, NULL);
	add_row(list, "Caps Lock key", NULL,
		bound_combo("keyboard", "keymap_options", "",
			    ITEMS("", "Caps Lock",
				  "ctrl:nocaps", "Works as another Ctrl",
				  "caps:escape", "Works as Esc",
				  "ctrl:swapcaps", "Swapped with Ctrl",
				  "caps:none", "Does nothing")), NULL);

	list = section("Typing");
	add_row(list, "Repeat delay", "How long to hold a key before it repeats (milliseconds).",
		bound_scale("keyboard", "repeat-delay", 150, 1000, 25, 400, 0), NULL);
	add_row(list, "Repeat rate", "Characters per second while a key is held.",
		bound_scale("keyboard", "repeat-rate", 10, 80, 1, 40, 0), NULL);
	add_row(list, "Turn on Num Lock when the desktop starts", NULL,
		BOOL_SWITCH("keyboard", "numlock-on", FALSE), NULL);
	entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Type here to test");
	gtk_widget_set_size_request(entry, 260, -1);
	add_row(list, "Test", "Shows the keyboard as it is now (before a restart).", entry, NULL);
	g_free(layout);
	g_free(variant);
	return page;
}

/*
 * Mouse & touchpad
 */

static GtkWidget *
build_mouse(void)
{
	GtkWidget *page = page_new("Mouse & touchpad",
		"Changes here take effect when the desktop restarts."), *list;

	list = section("Mouse and pointer");
	add_row(list, "Primary button", "Right suits left-handed use.",
		bound_combo("libinput", "left-handed", "false",
			    ITEMS("false", "Left", "true", "Right")), NULL);
	add_row(list, "Pointer speed", NULL,
		bound_scale("libinput", "accel-speed", -1, 1, 0.05, 0, 2), NULL);
	add_row(list, "Pointer acceleration", "Faster movements take the pointer further.",
		bound_switch("libinput", "accel-profile", "adaptive", "flat", "adaptive"), NULL);
	add_row(list, "Natural scrolling", "The content follows your fingers, as on a phone.",
		BOOL_SWITCH("libinput", "natural-scroll", FALSE), NULL);
	add_row(list, "Middle click by pressing both buttons", NULL,
		BOOL_SWITCH("libinput", "middle-button-emulation", FALSE), NULL);

	list = section("Touchpad");
	add_row(list, "Tap to click", "A light tap is a click; a two-finger tap is a right click.",
		BOOL_SWITCH("libinput", "enable-tap", FALSE), NULL);
	add_row(list, "Tap and drag", "Tap, then touch again and move to drag.",
		BOOL_SWITCH("libinput", "tap-and-drag", TRUE), NULL);
	add_row(list, "Ignore the touchpad while typing", NULL,
		BOOL_SWITCH("libinput", "disable-while-typing", TRUE), NULL);
	add_row(list, "Scrolling", NULL,
		bound_combo("libinput", "scroll-method", "two-finger",
			    ITEMS("two-finger", "Two fingers",
				  "edge", "Right edge of the touchpad",
				  "none", "Off")), NULL);
	return page;
}

/*
 * Date & time
 */

static struct {
	GtkWidget *time, *date, *region, *city, *sync_sub;
	char *zone;
	gboolean filling;
} dt;

static const char *const regions[] = {
	"Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic", "Australia",
	"Europe", "Indian", "Pacific", NULL
};

static char *
current_zone(void)
{
	char *z = read_file("/etc/timezone");

	if (!z || !*z) {
		char *l = g_file_read_link("/etc/localtime", NULL);
		char *p = l ? strstr(l, "zoneinfo/") : NULL;

		g_free(z);
		z = p ? g_strdup(p + 9) : NULL;
		g_free(l);
	}
	return z ? z : g_strdup("UTC");
}

static gboolean
clock_tick(gpointer data)
{
	char buf[128];
	time_t now = time(NULL);
	struct tm tm;

	tzset();
	localtime_r(&now, &tm);
	strftime(buf, sizeof buf, "%H:%M:%S", &tm);
	gtk_label_set_text(GTK_LABEL(dt.time), buf);
	strftime(buf, sizeof buf, "%A, %d %B %Y  ·  %Z (UTC%z)", &tm);
	gtk_label_set_text(GTK_LABEL(dt.date), buf);
	return G_SOURCE_CONTINUE;
}

static void
collect_zones(const char *rel, GPtrArray *out)
{
	char *path = g_build_filename(ZONEINFO, rel, NULL);
	GDir *d = g_dir_open(path, 0, NULL);
	const char *n;

	while (d && (n = g_dir_read_name(d))) {
		char *child = g_build_filename(rel, n, NULL);
		char *full = g_build_filename(ZONEINFO, child, NULL);

		if (g_file_test(full, G_FILE_TEST_IS_DIR))
			collect_zones(child, out);
		else
			g_ptr_array_add(out, g_strdup(child));
		g_free(full);
		g_free(child);
	}
	if (d)
		g_dir_close(d);
	g_free(path);
}

static int
str_cmp(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

static void
tz_fill_cities(const char *region)
{
	GPtrArray *zones = g_ptr_array_new_with_free_func(g_free);
	guint i;

	dt.filling = TRUE;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(dt.city));
	if (strcmp(region, "UTC") == 0) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dt.city), "UTC", "UTC");
	} else {
		collect_zones(region, zones);
		g_ptr_array_sort(zones, str_cmp);
		for (i = 0; i < zones->len; i++) {
			const char *z = g_ptr_array_index(zones, i);
			char *label = g_strdup(z + strlen(region) + 1);

			g_strdelimit(label, "_", ' ');
			gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dt.city), z, label);
			g_free(label);
		}
	}
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(dt.city), dt.zone))
		gtk_combo_box_set_active(GTK_COMBO_BOX(dt.city), -1);
	dt.filling = FALSE;
	g_ptr_array_free(zones, TRUE);
}

static void
tz_select_current(void)
{
	char *region = g_strdup(dt.zone), *slash = strchr(region, '/');

	if (slash)
		*slash = '\0';
	if (!g_strv_contains(regions, region)) {
		g_free(region);
		region = g_strdup("UTC");
	}
	dt.filling = TRUE;
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(dt.region), region);
	dt.filling = FALSE;
	tz_fill_cities(region);
	g_free(region);
}

static void
tz_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	char *zone = data;

	if (ok) {
		g_free(dt.zone);
		dt.zone = zone;
		clock_tick(NULL);
	} else {
		if (*err)
			message(GTK_MESSAGE_ERROR, "Could not change the time zone", err);
		g_free(zone);
		tz_select_current();
	}
}

static void
on_tz_city(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (dt.filling || !id || g_strcmp0(id, dt.zone) == 0)
		return;
	as_admin(ARGV("timezone", id), NULL, tz_done, g_strdup(id));
}

static void
on_tz_region(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (dt.filling || !id)
		return;
	tz_fill_cities(id);
	if (strcmp(id, "UTC") == 0)
		on_tz_city(GTK_COMBO_BOX(dt.city), NULL);
}

static void
time_sync_status(void)
{
	char *out = run_out(ARGV("chronyc", "-n", "tracking"));
	char *leap = text_value(out, "Leap status");
	char *ref = text_value(out, "Reference ID");
	char *text;

	if (g_strcmp0(leap, "Normal") == 0) {
		char *paren = ref ? strchr(ref, '(') : NULL;

		text = g_strdup_printf("Synchronized with the internet%s%s", paren ? " " : "",
				       paren ? paren : "");
	} else if (out) {
		text = g_strdup("Not synchronized yet: it needs an internet connection.");
	} else {
		text = g_strdup("The time service (chrony) is not running.");
	}
	set_sub(dt.sync_sub, text);
	g_free(text);
	g_free(leap);
	g_free(ref);
	g_free(out);
}

static void
time_synced(gboolean ok, const char *out, const char *err, gpointer data)
{
	if (!ok && *err)
		message(GTK_MESSAGE_ERROR, "Could not synchronize the clock", err);
	time_sync_status();
	clock_tick(NULL);
}

static void
on_time_sync(GtkButton *b, gpointer data)
{
	set_sub(dt.sync_sub, "Synchronizing...");
	as_admin(ARGV("time-sync"), NULL, time_synced, NULL);
}

static GtkWidget *
build_time(void)
{
	GtkWidget *page = page_new("Date & time", NULL), *list;
	const char *const formats[] = { "%d/%m/%Y", "%m/%d/%Y", "%Y-%m-%d", "%d.%m.%Y",
					"%d %b %Y", "%a %d/%m", NULL };
	char *items[2 * G_N_ELEMENTS(formats) + 1];
	time_t now = time(NULL);
	struct tm tm;
	int i;

	dt.time = label_new("", "hx-clock");
	dt.date = label_new("", "hx-dim");
	gtk_box_pack_start(GTK_BOX(page_box), dt.time, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page_box), dt.date, FALSE, FALSE, 0);
	dt.zone = current_zone();
	clock_tick(NULL);
	g_timeout_add_seconds(1, clock_tick, NULL);

	list = section("Time zone");
	dt.region = gtk_combo_box_text_new();
	for (i = 0; regions[i]; i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dt.region), regions[i], regions[i]);
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dt.region), "UTC", "UTC (no daylight saving)");
	g_signal_connect(dt.region, "changed", G_CALLBACK(on_tz_region), NULL);
	add_row(list, "Region", NULL, dt.region, NULL);
	dt.city = gtk_combo_box_text_new();
	gtk_widget_set_size_request(dt.city, 240, -1);
	gtk_combo_box_set_wrap_width(GTK_COMBO_BOX(dt.city), 3);
	g_signal_connect(dt.city, "changed", G_CALLBACK(on_tz_city), NULL);
	add_row(list, "City", "Changing it asks for your password.", dt.city, NULL);
	tz_select_current();

	list = section("Clock");
	add_row(list, "Set the time automatically", "",
		button_new("Sync now", G_CALLBACK(on_time_sync), NULL), &dt.sync_sub);
	time_sync_status();

	list = section("Taskbar clock");
	add_row(list, "Time format", "Takes effect when the desktop restarts.",
		bound_combo("shell", "clock-format", "minutes",
			    ITEMS("minutes", "12-hour (1:30 PM)",
				  "minutes-24h", "24-hour (13:30)",
				  "seconds", "12-hour with seconds",
				  "seconds-24h", "24-hour with seconds",
				  "none", "Hidden")), NULL);
	localtime_r(&now, &tm);
	for (i = 0; formats[i]; i++) {
		char buf[64];

		strftime(buf, sizeof buf, formats[i], &tm);
		items[2 * i] = (char *) formats[i];
		items[2 * i + 1] = g_strdup(buf);
	}
	items[2 * i] = NULL;
	add_row(list, "Date format", NULL,
		bound_combo("shell", "clock-date-format", "%d/%m/%Y", (const char *const *) items),
		NULL);
	for (i = 0; formats[i]; i++)
		g_free(items[2 * i + 1]);
	return page;
}

static void
time_shown(void)
{
	time_sync_status();
}

/*
 * Power
 */

static void
power_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	if (!ok)
		message(GTK_MESSAGE_ERROR, "Could not do that",
			*err ? err : "This account may not restart or shut down the computer.");
}

static void
on_power(GtkButton *b, gpointer data)
{
	gboolean off = GPOINTER_TO_INT(data);

	if (!confirm(off ? "Shut down the computer?" : "Restart the computer?",
		     "Save your work first: open programs will close.",
		     off ? "Shut down" : "Restart"))
		return;
	sync();
	run_job(off ? ARGV("sudo", "-n", "/sbin/poweroff") : ARGV("sudo", "-n", "/sbin/reboot"),
		NULL, power_done, NULL);
}

static void
on_restart_desktop(GtkButton *b, gpointer data)
{
	restart_desktop();
}

static GtkWidget *
build_power(void)
{
	GtkWidget *page = page_new("Power", NULL), *list, *b;

	list = section("Screen");
	add_row(list, "Turn off the screen when not used for",
		"Takes effect when the desktop restarts. Any key or mouse move turns it back on.",
		bound_combo("core", "idle-time", "0",
			    ITEMS("0", "Never", "60", "1 minute", "120", "2 minutes",
				  "300", "5 minutes", "600", "10 minutes",
				  "900", "15 minutes", "1800", "30 minutes",
				  "3600", "1 hour")), NULL);

	list = section("Restart and shut down");
	add_row(list, "Restart the desktop",
		"Closes all windows and starts the desktop again (applies desktop settings).",
		button_new("Restart desktop", G_CALLBACK(on_restart_desktop), NULL), NULL);
	add_row(list, "Restart the computer", NULL,
		button_new("Restart", G_CALLBACK(on_power), GINT_TO_POINTER(0)), NULL);
	b = button_new("Shut down", G_CALLBACK(on_power), GINT_TO_POINTER(1));
	add_class(b, "destructive-action");
	add_row(list, "Shut down the computer", NULL, b, NULL);
	return page;
}

/*
 * Account
 */

static struct {
	GtkWidget *cur, *new1, *new2, *host;
} acc;

static GtkWidget *
password_entry(void)
{
	GtkWidget *e = gtk_entry_new();

	gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
	gtk_widget_set_size_request(e, 240, -1);
	return e;
}

static void
password_changed(gboolean ok, const char *out, const char *err, gpointer data)
{
	char *pw = data;

	if (ok) {
		g_free(admin_pw);
		admin_pw = pw;		/* sudo now wants the new one */
		gtk_entry_set_text(GTK_ENTRY(acc.cur), "");
		gtk_entry_set_text(GTK_ENTRY(acc.new1), "");
		gtk_entry_set_text(GTK_ENTRY(acc.new2), "");
		message(GTK_MESSAGE_INFO, "Password changed",
			"Use the new password from now on (also for sudo).");
	} else {
		g_free(pw);
		if (*err)
			message(GTK_MESSAGE_ERROR, "The password was not changed", err);
	}
}

static void
on_password(GtkButton *b, gpointer data)
{
	const char *cur = gtk_entry_get_text(GTK_ENTRY(acc.cur));
	const char *n1 = gtk_entry_get_text(GTK_ENTRY(acc.new1));
	const char *n2 = gtk_entry_get_text(GTK_ENTRY(acc.new2));
	char *line;

	if (!*n1) {
		message(GTK_MESSAGE_WARNING, "Type the new password", NULL);
		return;
	}
	if (strcmp(n1, n2) != 0) {
		message(GTK_MESSAGE_WARNING, "The new passwords do not match",
			"Type the same new password in both fields.");
		return;
	}
	if (strchr(n1, '\n') || strchr(n1, ':')) {
		message(GTK_MESSAGE_WARNING, "The new password may not contain \":\"", NULL);
		return;
	}
	if (getuid() != 0) {
		if (!password_ok(cur)) {
			message(GTK_MESSAGE_ERROR, "The current password is wrong", NULL);
			return;
		}
		g_free(admin_pw);
		admin_pw = g_strdup(cur);
	}
	if (g_utf8_strlen(n1, -1) < 6 &&
	    !confirm("Use a short password?",
		     "Passwords shorter than 6 characters are easy to guess.", "Use it"))
		return;
	line = g_strdup_printf("%s\n", n1);
	as_admin(ARGV("password"), line, password_changed, g_strdup(n1));
	memset(line, 0, strlen(line));
	g_free(line);
}

static void
host_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	char *h;

	if (ok)
		message(GTK_MESSAGE_INFO, "Computer renamed",
			"Other computers on the network see the new name after it reconnects.");
	else if (*err)
		message(GTK_MESSAGE_ERROR, "The computer was not renamed", err);
	h = read_file("/etc/hostname");
	gtk_entry_set_text(GTK_ENTRY(acc.host), h ? h : "");
	g_free(h);
}

static void
on_hostname(GtkButton *b, gpointer data)
{
	const char *h = gtk_entry_get_text(GTK_ENTRY(acc.host));
	char *cur = read_file("/etc/hostname");

	if (g_strcmp0(h, cur) != 0)
		as_admin(ARGV("hostname", h), NULL, host_done, NULL);
	g_free(cur);
}

static GtkWidget *
build_account(void)
{
	GtkWidget *page = page_new("Account", NULL), *list, *box, *icon, *text;
	struct passwd *pw = getpwuid(getuid());
	struct group *wheel = getgrnam("wheel");
	gboolean admin = getuid() == 0;
	gid_t groups[64];
	int i, n = getgroups(64, groups);
	char *full = NULL, *host;

	for (i = 0; wheel && i < n; i++)
		if (groups[i] == wheel->gr_gid)
			admin = TRUE;
	if (pw && pw->pw_gecos && *pw->pw_gecos)
		full = g_strndup(pw->pw_gecos, strcspn(pw->pw_gecos, ","));

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_widget_set_margin_top(box, 8);
	icon = gtk_image_new_from_icon_name("avatar-default-symbolic", GTK_ICON_SIZE_DIALOG);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
	add_class(icon, "hx-avatar");
	gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(text), label_new(full && *full ? full : (pw ? pw->pw_name : "?"),
						    "hx-big"), FALSE, FALSE, 0);
	{
		char *s = g_strdup_printf("%s%s%s", full && *full && pw ? pw->pw_name : "",
					  full && *full && pw ? "  ·  " : "",
					  admin ? "Administrator" : "Standard user");

		gtk_box_pack_start(GTK_BOX(text), label_new(s, "hx-dim"), FALSE, FALSE, 0);
		g_free(s);
	}
	gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(page_box), box, FALSE, FALSE, 0);

	list = section("Change password");
	acc.cur = password_entry();
	acc.new1 = password_entry();
	acc.new2 = password_entry();
	add_row(list, "Current password", NULL, acc.cur, NULL);
	add_row(list, "New password", NULL, acc.new1, NULL);
	add_row(list, "Type the new password again", NULL, acc.new2, NULL);
	add_row(list, "", "Your password unlocks administrator tasks too (sudo).",
		button_new("Change password", G_CALLBACK(on_password), NULL), NULL);

	list = section("Computer name");
	acc.host = gtk_entry_new();
	host = read_file("/etc/hostname");
	gtk_entry_set_text(GTK_ENTRY(acc.host), host ? host : "");
	gtk_entry_set_max_length(GTK_ENTRY(acc.host), 63);
	g_free(host);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start(GTK_BOX(box), acc.host, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), button_new("Rename", G_CALLBACK(on_hostname), NULL),
			   FALSE, FALSE, 0);
	add_row(list, "Name on the network", "Letters, digits and hyphens.", box, NULL);
	g_free(full);
	return page;
}

/*
 * Services
 */

static struct {
	GtkWidget *dk_sub, *dk_btn, *dk_boot, *cups_sub, *cups_btn, *cups_open;
	gboolean filling;
} svc;

static void
services_refresh(void)
{
	gboolean dk = g_file_test("/etc/init.d/dockerd", G_FILE_TEST_EXISTS);
	gboolean dk_run = find_proc("dockerd") > 0;
	gboolean cups = g_file_test("/etc/init.d/S81cupsd", G_FILE_TEST_EXISTS);
	gboolean cups_run = find_proc("cupsd") > 0;

	set_sub(svc.dk_sub, !dk ? "Not installed" : dk_run ? "Running" : "Stopped");
	gtk_button_set_label(GTK_BUTTON(svc.dk_btn), dk_run ? "Stop" : "Start");
	gtk_widget_set_sensitive(svc.dk_btn, dk);
	svc.filling = TRUE;
	gtk_switch_set_active(GTK_SWITCH(svc.dk_boot),
			      g_file_test("/etc/init.d/S60dockerd", G_FILE_TEST_EXISTS));
	svc.filling = FALSE;
	gtk_widget_set_sensitive(svc.dk_boot, dk);
	set_sub(svc.cups_sub, !cups ? "Not installed" : cups_run ? "Running" : "Stopped");
	gtk_button_set_label(GTK_BUTTON(svc.cups_btn), cups_run ? "Stop" : "Start");
	gtk_widget_set_sensitive(svc.cups_btn, cups);
	gtk_widget_set_sensitive(svc.cups_open, cups_run);
}

static gboolean
services_refresh_later(gpointer data)
{
	services_refresh();
	return G_SOURCE_REMOVE;
}

static void
service_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	if (!ok && (*err || *out))
		message(GTK_MESSAGE_ERROR, "The service did not change", *err ? err : out);
	services_refresh();
	g_timeout_add_seconds(2, services_refresh_later, NULL);
}

static void
on_service(GtkButton *b, gpointer data)
{
	const char *name = data;
	gboolean running = find_proc(strcmp(name, "docker") == 0 ? "dockerd" : "cupsd") > 0;

	set_sub(strcmp(name, "docker") == 0 ? svc.dk_sub : svc.cups_sub,
		running ? "Stopping..." : "Starting...");
	as_admin(ARGV("service", name, running ? "stop" : "start"), NULL, service_done, NULL);
}

static void
on_docker_boot(GObject *sw, GParamSpec *ps, gpointer data)
{
	if (svc.filling)
		return;
	as_admin(ARGV("docker-boot", gtk_switch_get_active(GTK_SWITCH(sw)) ? "on" : "off"),
		 NULL, service_done, NULL);
}

static void
on_cups_open(GtkButton *b, gpointer data)
{
	g_spawn_command_line_async("xdg-open http://localhost:631/", NULL);
}

static GtkWidget *
build_services(void)
{
	GtkWidget *page = page_new("Services",
		"Programs that run in the background. Starting or stopping them asks "
		"for your password."), *list;

	list = section("Docker (containers, hexa-dev)");
	svc.dk_btn = button_new("Start", G_CALLBACK(on_service), "docker");
	add_row(list, "Docker", "", svc.dk_btn, &svc.dk_sub);
	svc.dk_boot = gtk_switch_new();
	g_signal_connect(svc.dk_boot, "notify::active", G_CALLBACK(on_docker_boot), NULL);
	add_row(list, "Start Docker when the computer starts",
		"Off saves memory: hexa-dev starts Docker when it needs it.", svc.dk_boot, NULL);

	list = section("Printing (CUPS)");
	svc.cups_btn = button_new("Start", G_CALLBACK(on_service), "cups");
	add_row(list, "Printing service", "", svc.cups_btn, &svc.cups_sub);
	svc.cups_open = button_new("Open", G_CALLBACK(on_cups_open), NULL);
	add_row(list, "Printers", "Add and manage printers in the browser.", svc.cups_open, NULL);
	return page;
}

/*
 * About
 */

static struct {
	GtkWidget *list, *disk_list;
	GString *text;		/* for "Copy" */
} about;

static void
about_line(const char *name, const char *value)
{
	GtkWidget *v = gtk_label_new(value ? value : "unknown");

	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_label_set_xalign(GTK_LABEL(v), 1);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 48);
	gtk_label_set_justify(GTK_LABEL(v), GTK_JUSTIFY_RIGHT);
	add_row(about.list, name, NULL, v, NULL);
	g_string_append_printf(about.text, "%s: %s\n", name, value ? value : "unknown");
}

static char *
meminfo_kb(const char *txt, const char *key)
{
	char *v = text_value(txt, key);
	char *r = v ? g_format_size_full((guint64) g_ascii_strtoull(v, NULL, 10) * 1024,
					 G_FORMAT_SIZE_IEC_UNITS) : NULL;

	g_free(v);
	return r;
}

static void
about_refresh(void)
{
	struct utsname u;
	char *os = NULL, *rel = read_file("/etc/os-release"), *cpu, *mem, *s, *host;
	char *total, *avail, *ips = NULL, *gpu = NULL, *up;
	GString *g = g_string_new(NULL);
	GDir *d;
	const char *n;
	double secs = 0;
	struct statvfs vfs;

	list_clear(about.list);
	list_clear(about.disk_list);
	g_string_truncate(about.text, 0);

	if (rel) {
		char *p = strstr(rel, "PRETTY_NAME=");

		if (p) {
			p += 12;
			os = g_strndup(p, strcspn(p, "\n"));
			g_strdelimit(os, "\"", ' ');
			g_strstrip(os);
		}
	}
	uname(&u);
	s = read_file("/proc/cpuinfo");
	cpu = text_value(s, "model name");
	g_free(s);
	if (cpu) {
		s = g_strdup_printf("%s (%ld threads)", cpu, sysconf(_SC_NPROCESSORS_ONLN));
		g_free(cpu);
		cpu = s;
	}
	s = read_file("/proc/meminfo");
	total = meminfo_kb(s, "MemTotal");
	avail = meminfo_kb(s, "MemAvailable");
	g_free(s);
	mem = g_strdup_printf("%s (%s available)", total ? total : "?", avail ? avail : "?");

	d = g_dir_open("/sys/class/drm", 0, NULL);
	while (d && (n = g_dir_read_name(d))) {
		char *p, *l;

		if (!g_str_has_prefix(n, "card") || strchr(n, '-'))
			continue;
		p = g_strdup_printf("/sys/class/drm/%s/device/driver", n);
		l = g_file_read_link(p, NULL);
		if (l) {
			char *base = g_path_get_basename(l);

			if (!strstr(g->str, base))
				g_string_append_printf(g, "%s%s", g->len ? ", " : "", base);
			g_free(base);
			g_free(l);
		}
		g_free(p);
	}
	if (d)
		g_dir_close(d);
	gpu = g_string_free(g, FALSE);

	s = run_out(ARGV("ip", "-4", "addr"));
	if (s) {
		GString *a = g_string_new(NULL);
		char **lines = g_strsplit(s, "\n", -1), **l;

		for (l = lines; *l; l++) {
			char **f = g_strsplit_set(g_strstrip(*l), " ", -1);
			int k, last = g_strv_length(f) - 1;

			if (f[0] && !strcmp(f[0], "inet") && f[1] && last > 1 &&
			    strcmp(f[last], "lo") && strcmp(f[last], "docker0"))
				g_string_append_printf(a, "%s%s (%s)", a->len ? ", " : "",
						       f[1], f[last]);
			for (k = 0; f[k]; k++)
				;
			g_strfreev(f);
		}
		g_strfreev(lines);
		ips = g_string_free(a, FALSE);
		g_free(s);
	}

	s = read_file("/proc/uptime");
	if (s)
		secs = g_ascii_strtod(s, NULL);
	g_free(s);
	{
		int days = secs / 86400, hours = ((int) secs % 86400) / 3600,
		    mins = ((int) secs % 3600) / 60;

		if (days)
			up = g_strdup_printf("%d day%s, %d hour%s", days, days == 1 ? "" : "s",
					     hours, hours == 1 ? "" : "s");
		else if (hours)
			up = g_strdup_printf("%d hour%s, %d minute%s", hours, hours == 1 ? "" : "s",
					     mins, mins == 1 ? "" : "s");
		else
			up = g_strdup_printf("%d minute%s", mins, mins == 1 ? "" : "s");
	}
	host = read_file("/etc/hostname");

	about_line("Computer name", host);
	about_line("System", os);
	about_line("Kernel", u.release);
	about_line("Processor", cpu);
	about_line("Memory", mem);
	about_line("Graphics driver", gpu && *gpu ? gpu : NULL);
	about_line("Network", ips && *ips ? ips : "Not connected");
	about_line("Running for", up);
	gtk_widget_show_all(about.list);

	if (statvfs("/", &vfs) == 0) {
		guint64 tot = (guint64) vfs.f_blocks * vfs.f_frsize;
		guint64 fr = (guint64) vfs.f_bavail * vfs.f_frsize;
		GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
		char *ts = g_format_size(tot), *fs = g_format_size(fr);
		char *sub = g_strdup_printf("%s free of %s", fs, ts);

		gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), tot ? 1.0 - (double) fr / tot : 0);
		gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_LOW, 0.80);
		gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_HIGH, 0.92);
		gtk_widget_set_size_request(bar, 260, 10);
		add_row(about.disk_list, "System disk", sub, bar, NULL);
		g_string_append_printf(about.text, "Disk: %s\n", sub);
		gtk_widget_show_all(about.disk_list);
		g_free(ts);
		g_free(fs);
		g_free(sub);
	}
	g_free(os);
	g_free(rel);
	g_free(cpu);
	g_free(mem);
	g_free(total);
	g_free(avail);
	g_free(gpu);
	g_free(ips);
	g_free(up);
	g_free(host);
}

static void
on_about_copy(GtkButton *b, gpointer data)
{
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
			       about.text->str, -1);
	gtk_button_set_label(b, "Copied");
}

static void
on_taskmgr(GtkButton *b, gpointer data)
{
	g_spawn_command_line_async("hexa-taskmgr", NULL);
}

static GtkWidget *
build_about(void)
{
	GtkWidget *page = page_new("About", NULL), *box;

	about.text = g_string_new(NULL);
	about.list = section("This computer");
	about.disk_list = section("Storage");
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(box, 14);
	gtk_box_pack_start(GTK_BOX(box), button_new("Copy", G_CALLBACK(on_about_copy), NULL),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), button_new("Task Manager", G_CALLBACK(on_taskmgr), NULL),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page_box), box, FALSE, FALSE, 0);
	return page;
}

/*
 * Default apps
 */

/* What a web browser opens; ~/.config/mimeapps.list gets them all (GIO
 * writes it), and xdg-open, Files and GTK/Electron apps follow it */
static const char *const web_types[] = {
	"x-scheme-handler/http", "x-scheme-handler/https", "text/html",
	"application/xhtml+xml", NULL
};

static struct {
	GtkWidget *browser, *sub;
	gboolean filling;
} defapps;

static void
defapps_refresh(void)
{
	GList *apps, *a;
	GAppInfo *def;
	const char *def_id;
	int n = 0;

	defapps.filling = TRUE;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(defapps.browser));
	/* a fresh look at the app folders: browsers installed from the Store */
	apps = g_app_info_get_all_for_type("x-scheme-handler/https");
	def = g_app_info_get_default_for_type("x-scheme-handler/https", FALSE);
	def_id = def ? g_app_info_get_id(def) : NULL;
	for (a = apps; a; a = a->next) {
		GAppInfo *app = a->data;
		const char *id = g_app_info_get_id(app);

		if (!id || (!g_app_info_should_show(app) && g_strcmp0(id, def_id) != 0))
			continue;
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(defapps.browser), id,
					  g_app_info_get_display_name(app));
		n++;
	}
	if (!def_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(defapps.browser), def_id))
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(defapps.browser), "firefox.desktop");
	set_sub(defapps.sub, n > 1 ? "Links you click in other apps open here." :
		"Links you click in other apps open here. Other browsers (Chrome, "
		"Brave, Edge...) show up here once installed from the Store.");
	g_list_free_full(apps, g_object_unref);
	if (def)
		g_object_unref(def);
	defapps.filling = FALSE;
}

static void
on_default_browser(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);
	GDesktopAppInfo *app;
	GError *err = NULL;
	int i;

	if (defapps.filling || !id || !(app = g_desktop_app_info_new(id)))
		return;
	for (i = 0; web_types[i] && !err; i++)
		g_app_info_set_as_default_for_type(G_APP_INFO(app), web_types[i], &err);
	if (err) {
		message(GTK_MESSAGE_ERROR, "Could not change the default browser", err->message);
		g_error_free(err);
	}
	g_object_unref(app);
}

static void
on_open_store(GtkButton *b, gpointer data)
{
	g_spawn_command_line_async("hexa-store", NULL);
}

static GtkWidget *
build_defapps(void)
{
	GtkWidget *page = page_new("Default apps",
		"The apps that open links and pages. Changes apply right away."), *list;

	list = section(NULL);
	defapps.browser = gtk_combo_box_text_new();
	gtk_widget_set_size_request(defapps.browser, 260, -1);
	g_signal_connect(defapps.browser, "changed", G_CALLBACK(on_default_browser), NULL);
	add_row(list, "Web browser", NULL, defapps.browser, &defapps.sub);
	if (g_file_test("/usr/bin/hexa-store", G_FILE_TEST_IS_EXECUTABLE))
		add_row(list, "More browsers", "Chrome, Brave, Edge, Chromium and others are in the Store.",
			button_new("Open Store", G_CALLBACK(on_open_store), NULL), NULL);
	defapps_refresh();
	return page;
}

/*
 * Window
 */

static const struct {
	const char *id, *title, *icon;
	GtkWidget *(*build)(void);
	void (*shown)(void);
} pages[] = {
	{ "network", "Network & internet", "network-wireless-symbolic", build_network, network_shown },
	{ "bluetooth", "Bluetooth", "bluetooth-symbolic", build_bluetooth, bluetooth_shown },
	{ "sound", "Sound", "audio-volume-high-symbolic", build_sound, sound_refresh },
	{ "display", "Display", "video-display-symbolic", build_display, NULL },
	{ "personalization", "Personalization", "preferences-desktop-wallpaper-symbolic",
	  build_personal, NULL },
	{ "apps", "Default apps", "applications-internet-symbolic", build_defapps, defapps_refresh },
	{ "keyboard", "Keyboard", "input-keyboard-symbolic", build_keyboard, NULL },
	{ "mouse", "Mouse & touchpad", "input-mouse-symbolic", build_mouse, NULL },
	{ "time", "Date & time", "preferences-system-time-symbolic", build_time, time_shown },
	{ "power", "Power", "system-shutdown-symbolic", build_power, NULL },
	{ "account", "Account", "avatar-default-symbolic", build_account, NULL },
	{ "services", "Services", "emblem-system-symbolic", build_services, services_refresh },
	{ "about", "About", "help-about-symbolic", build_about, about_refresh },
};

static const char css[] =
	".hx-nav, .hx-nav row { background: transparent; }"
	".hx-nav row { padding: 9px 12px; margin: 1px 8px; border-radius: 6px; }"
	".hx-nav row:hover { background: rgba(255,255,255,0.06); }"
	".hx-nav row:selected { background: rgba(34,211,238,0.16); color: #ffffff; }"
	".hx-nav row:selected image { color: #22d3ee; }"
	".hx-sidebar { background: rgba(0,0,0,0.18); }"
	".hx-title { font-size: 26px; font-weight: 300; margin-bottom: 4px; }"
	".hx-section { font-weight: bold; margin-top: 18px; margin-bottom: 4px; }"
	".hx-section-inline { font-weight: bold; }"
	".hx-card { background: rgba(255,255,255,0.04); border: 1px solid rgba(255,255,255,0.10);"
	"  border-radius: 8px; }"
	".hx-card row { border-bottom: 1px solid rgba(255,255,255,0.06); }"
	".hx-card row:last-child { border-bottom: none; }"
	".hx-row { padding: 10px 14px; min-height: 36px; }"
	".hx-dim { opacity: 0.68; font-size: 12px; }"
	".hx-strong { font-weight: bold; }"
	".hx-big { font-size: 20px; }"
	".hx-warn { color: #facc15; }"
	".hx-clock { font-size: 44px; font-weight: 200; }"
	".hx-avatar { color: #22d3ee; }"
	".hx-preview { border-radius: 6px; }";

static void
on_nav(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	int i;

	if (!row)
		return;
	i = gtk_list_box_row_get_index(row);
	if (strcmp(pages[i].id, "sound") != 0)
		mic_test_stop();
	gtk_stack_set_visible_child_name(GTK_STACK(stack), pages[i].id);
	if (pages[i].shown)
		pages[i].shown();
}

static void
on_restart_bar(GtkInfoBar *bar, int response, gpointer data)
{
	restart_desktop();
}

static void
activate(GtkApplication *app, gpointer data)
{
	GtkWidget *header, *paned, *side, *right, *l;
	GtkCssProvider *provider;
	guint i, first = 0;

	if (win) {		/* opened again: show the open window */
		gtk_window_present(GTK_WINDOW(win));
		return;
	}
	g_object_set(gtk_settings_get_default(),
		     "gtk-application-prefer-dark-theme", TRUE, NULL);
	provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);

	cfg_load();

	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "Settings");
	gtk_window_set_default_size(GTK_WINDOW(win), 1000, 660);
	header = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
	/* GtkWindow copies the header bar title into its own (taskbar label) */
	gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Settings");
	gtk_window_set_titlebar(GTK_WINDOW(win), header);

	paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	side = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(side, 230, -1);
	add_class(side, "hx-sidebar");
	nav = gtk_list_box_new();
	add_class(nav, "hx-nav");
	gtk_widget_set_margin_top(nav, 10);
	gtk_container_add(GTK_CONTAINER(side), nav);
	gtk_box_pack_start(GTK_BOX(paned), side, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(paned), gtk_separator_new(GTK_ORIENTATION_VERTICAL),
			   FALSE, FALSE, 0);

	right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	restart_bar = gtk_info_bar_new();
	gtk_info_bar_set_message_type(GTK_INFO_BAR(restart_bar), GTK_MESSAGE_INFO);
	l = gtk_label_new("Restart the desktop to apply your changes (open windows will close).");
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_show(l);
	gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(restart_bar))), l);
	gtk_info_bar_add_button(GTK_INFO_BAR(restart_bar), "Restart desktop", 1);
	g_signal_connect(restart_bar, "response", G_CALLBACK(on_restart_bar), NULL);
	gtk_widget_set_no_show_all(restart_bar, TRUE);
	gtk_box_pack_start(GTK_BOX(right), restart_bar, FALSE, FALSE, 0);

	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(stack), 120);
	gtk_box_pack_start(GTK_BOX(right), stack, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(paned), right, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(win), paned);

	for (i = 0; i < G_N_ELEMENTS(pages); i++) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

		gtk_box_pack_start(GTK_BOX(row),
			gtk_image_new_from_icon_name(pages[i].icon, GTK_ICON_SIZE_MENU),
			FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), gtk_label_new(pages[i].title), FALSE, FALSE, 0);
		gtk_container_add(GTK_CONTAINER(nav), row);
		gtk_stack_add_named(GTK_STACK(stack), pages[i].build(), pages[i].id);
		if (start_page && strcmp(start_page, pages[i].id) == 0)
			first = i;
	}
	g_signal_connect(nav, "row-selected", G_CALLBACK(on_nav), NULL);

	gtk_widget_show_all(win);
	if (restart_pending())
		show_restart_bar();
	gtk_list_box_select_row(GTK_LIST_BOX(nav),
				gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), first));
}

/* hexa-settings [PAGE], in this process or sent from another one: open the
 * window, or show the open one, on that page (network, bluetooth, sound...) */
static int
on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer data)
{
	int argc, i;
	char **argv = g_application_command_line_get_arguments(cl, &argc);
	const char *page = argc > 1 && argv[1][0] != '-' ? argv[1] : NULL;

	if (!win) {
		start_page = page ? g_strdup(page) : NULL;
		activate(GTK_APPLICATION(app), NULL);
	} else if (page) {
		for (i = 0; i < (int) G_N_ELEMENTS(pages); i++) {
			GtkListBoxRow *row;

			if (strcmp(page, pages[i].id) != 0)
				continue;
			row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), i);
			if (row == gtk_list_box_get_selected_row(GTK_LIST_BOX(nav))) {
				if (pages[i].shown)
					pages[i].shown();
			} else {
				gtk_list_box_select_row(GTK_LIST_BOX(nav), row);
			}
		}
		gtk_window_present(GTK_WINDOW(win));
	} else {
		gtk_window_present(GTK_WINDOW(win));
	}
	g_strfreev(argv);
	return 0;
}

int
main(int argc, char **argv)
{
	GtkApplication *app;
	GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
	int status;

	/* hexa-settings --set GROUP KEY VALUE: change one desktop setting with
	 * no window, e.g. --set "output eDP-1" scale 2 (used by hexa-setup) */
	if (argc == 5 && strcmp(argv[1], "--set") == 0) {
		GError *e = NULL;

		cfg_load();
		g_key_file_set_value(user_ini, argv[2], argv[3], argv[4]);
		g_mkdir_with_parents(conf_dir, 0755);
		if (!g_key_file_save_to_file(user_ini, user_ini_path, &e)) {
			fprintf(stderr, "hexa-settings: %s\n", e->message);
			return 1;
		}
		write_desktop_ini();
		return 0;
	}
	/* single instance through the session bus, when there is one; the
	 * page name goes to the running instance (on_command_line) */
	flags |= G_APPLICATION_HANDLES_COMMAND_LINE;
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;

	g_set_prgname(APP_ID);	/* GTK3 sends it as the Wayland app-id */
	app = gtk_application_new(APP_ID, flags);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
	/* only the page name is passed on: GApplication would refuse other options */
	status = g_application_run(G_APPLICATION(app),
				   argc > 1 && argv[1][0] != '-' ? 2 : 1, argv);
	mic_test_stop();	/* never leave the microphone open */
	g_object_unref(app);
	return status;
}
