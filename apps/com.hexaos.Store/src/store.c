/*
 * hexa-store: the hexaOS Store (GTK 3, runs as the Flatpak com.hexaos.Store)
 *
 * Finds, installs, updates and removes Flatpak apps (Flathub and any other
 * remote of this user) and gives each installed app a Start menu entry.
 *
 * Every flatpak command runs on the host through "flatpak-spawn --host"
 * (permission --talk-name=org.freedesktop.Flatpak), always with --user.
 *
 * Start menu entries are [launcher] blocks in ~/.config/hexaos/apps.ini, one
 * per app, separated by blank lines and marked with "app-id=ID". hexa-session
 * adds that file to the desktop config at login; apps/hexa-launcher writes the
 * same format. Their icons are PNG copies in ~/.config/hexaos/icons/, because
 * the taskbar reads PNG only.
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>

#define APP_ID		"com.hexaos.Store"
#define FALLBACK_ICON	"/usr/share/weston/icon_window.png"
#define ICON_SIZE	48	/* in the lists */
#define LAUNCHER_ICON	64	/* PNG made for the Start menu */
#define MAX_RESULTS	80

#define ARGV(...)	((const char *const[]) { __VA_ARGS__, NULL })

static GtkWidget *win, *content, *stack, *restart_bar, *spinner, *status;
static GtkWidget *search_entry, *search_list, *installed_list, *updates_list;
static GtkWidget *update_all;
static char *conf_dir, *apps_ini, *icon_dir, *exports, *appstream_dir;
static gboolean in_sandbox;
static GHashTable *installed;	/* app id -> name, from "flatpak list" */

static void refresh_installed(void);
static void refresh_updates(void);

/*
 * Small helpers
 */

/* Removes terminal escape sequences and carriage returns, in place */
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

/* Last non-empty line of a command's error output: the useful part */
static char *
last_line(const char *text)
{
	char **lines = g_strsplit(text ? text : "", "\n", -1);
	char *found = NULL;
	int i;

	for (i = g_strv_length(lines) - 1; i >= 0 && !found; i--)
		if (*g_strstrip(lines[i]))
			found = g_strdup(lines[i]);
	g_strfreev(lines);
	return found ? found : g_strdup("");
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

/* While a command runs: spinner and TEXT at the bottom, the rest disabled */
static void
set_busy(const char *text)
{
	gtk_label_set_text(GTK_LABEL(status), text ? text : "");
	gtk_widget_set_sensitive(content, text == NULL);
	if (text)
		gtk_spinner_start(GTK_SPINNER(spinner));
	else
		gtk_spinner_stop(GTK_SPINNER(spinner));
}

/*
 * Running commands on the host
 */

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
	job->done(ok, out ? strip_ansi(out) : "", err ? strip_ansi(err) : "", job->data);
	g_free(out);
	g_free(err);
	g_clear_error(&e);
	g_object_unref(p);
	g_free(job);
}

/* Runs ARGS on the host (directly when not in a sandbox); DONE gets the output */
static void
run_host(const char *const *args, JobDone done, gpointer data)
{
	GPtrArray *argv = g_ptr_array_new();
	GError *e = NULL;
	GSubprocess *p;
	Job *job;

	if (in_sandbox) {
		g_ptr_array_add(argv, "flatpak-spawn");
		g_ptr_array_add(argv, "--host");
	}
	for (; *args; args++)
		g_ptr_array_add(argv, (gpointer) *args);
	g_ptr_array_add(argv, NULL);

	p = g_subprocess_newv((const char *const *) argv->pdata,
			      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
	g_ptr_array_free(argv, TRUE);
	if (!p) {
		done(FALSE, "", e->message, data);
		g_error_free(e);
		return;
	}
	job = g_new0(Job, 1);
	job->done = done;
	job->data = data;
	g_subprocess_communicate_utf8_async(p, NULL, NULL, job_finished, job);
}

/* Lines of tab-separated columns ("flatpak ... --columns=a,b,c") */
static char ***
parse_columns(const char *out)
{
	char **lines = g_strsplit(out, "\n", -1);
	GPtrArray *rows = g_ptr_array_new();
	int i;

	for (i = 0; lines[i]; i++)
		if (*lines[i])
			g_ptr_array_add(rows, g_strsplit(lines[i], "\t", -1));
	g_ptr_array_add(rows, NULL);
	g_strfreev(lines);
	return (char ***) g_ptr_array_free(rows, FALSE);
}

static void
free_columns(char ***rows)
{
	int i;

	for (i = 0; rows[i]; i++)
		g_strfreev(rows[i]);
	g_free(rows);
}

/* Column N of a row, or "" when the row is shorter */
static const char *
col(char **row, guint n)
{
	return n < g_strv_length(row) ? row[n] : "";
}

/*
 * Icons
 */

/* Best icon file for an app: Flathub's catalog, then what the app exports */
static char *
find_icon(const char *id)
{
	static const char *const sizes[] = { "128x128", "64x64", NULL };
	static const char *const app_sizes[] = { "256x256", "128x128", "96x96", "64x64",
						  "48x48", "32x32", NULL };
	char *file = g_strconcat(id, ".png", NULL), *path = NULL;
	GDir *remotes = g_dir_open(appstream_dir, 0, NULL);
	const char *remote;
	int i;

	while (remotes && !path && (remote = g_dir_read_name(remotes))) {
		for (i = 0; sizes[i] && !path; i++) {
			path = g_build_filename(appstream_dir, remote, "x86_64", "active",
						"icons", sizes[i], file, NULL);
			if (!g_file_test(path, G_FILE_TEST_EXISTS))
				g_clear_pointer(&path, g_free);
		}
	}
	if (remotes)
		g_dir_close(remotes);
	for (i = 0; app_sizes[i] && !path; i++) {
		path = g_build_filename(exports, "icons", "hicolor", app_sizes[i], "apps",
					file, NULL);
		if (!g_file_test(path, G_FILE_TEST_EXISTS))
			g_clear_pointer(&path, g_free);
	}
	g_free(file);
	if (!path) {
		file = g_strconcat(id, ".svg", NULL);
		path = g_build_filename(exports, "icons", "hicolor", "scalable", "apps", file, NULL);
		g_free(file);
		if (!g_file_test(path, G_FILE_TEST_EXISTS))
			g_clear_pointer(&path, g_free);
	}
	return path;
}

static GtkWidget *
icon_image(const char *id)
{
	char *path = find_icon(id);
	GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file_at_scale(path, ICON_SIZE, ICON_SIZE,
								  TRUE, NULL) : NULL;
	GtkWidget *img;

	if (pb) {
		img = gtk_image_new_from_pixbuf(pb);
		g_object_unref(pb);
	} else {
		img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
		gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_SIZE);
	}
	g_free(path);
	return img;
}

/*
 * Start menu entries (~/.config/hexaos/apps.ini)
 */

/* The file as blocks of lines (blank lines in between dropped) */
static GPtrArray *
ini_blocks(void)
{
	GPtrArray *blocks = g_ptr_array_new_with_free_func(g_free);
	char *text = NULL, **lines;
	GString *cur = g_string_new(NULL);
	int i;

	if (!g_file_get_contents(apps_ini, &text, NULL, NULL))
		text = g_strdup("# Start menu entries for Flatpak apps (written by the hexaOS "
				"Store and apps/hexa-app).\n"
				"# Added to the desktop config at login by hexa-session.\n");
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i]; i++) {
		if (*g_strstrip(lines[i])) {
			g_string_append_printf(cur, "%s\n", lines[i]);
		} else if (cur->len) {
			g_ptr_array_add(blocks, g_string_free(cur, FALSE));
			cur = g_string_new(NULL);
		}
	}
	if (cur->len)
		g_ptr_array_add(blocks, g_string_free(cur, FALSE));
	else
		g_string_free(cur, TRUE);
	g_strfreev(lines);
	g_free(text);
	return blocks;
}

static gboolean
block_is(const char *block, const char *id)
{
	char *key = g_strdup_printf("\napp-id=%s\n", id), *b = g_strconcat("\n", block, NULL);
	gboolean is = strstr(b, key) != NULL;

	g_free(key);
	g_free(b);
	return is;
}

static void
ini_save(GPtrArray *blocks)
{
	GString *s = g_string_new(NULL);
	GError *e = NULL;
	guint i;

	for (i = 0; i < blocks->len; i++)
		g_string_append_printf(s, "%s\n", (char *) blocks->pdata[i]);
	g_mkdir_with_parents(conf_dir, 0755);
	if (!g_file_set_contents(apps_ini, s->str, -1, &e)) {
		if (win)
			message(GTK_MESSAGE_ERROR, "Could not change the Start menu", e->message);
		else
			g_printerr("Could not change the Start menu: %s\n", e->message);
		g_error_free(e);
	}
	g_string_free(s, TRUE);
}

static gboolean
launcher_exists(const char *id)
{
	GPtrArray *blocks = ini_blocks();
	gboolean found = FALSE;
	guint i;

	for (i = 0; i < blocks->len && !found; i++)
		found = block_is(blocks->pdata[i], id);
	g_ptr_array_free(blocks, TRUE);
	return found;
}

static void
show_restart_bar(void)
{
	if (restart_bar)
		gtk_widget_show(restart_bar);
}

/* Drops the app's entry (and its icon); TRUE if there was one */
static gboolean
remove_launcher(const char *id)
{
	GPtrArray *blocks = ini_blocks();
	char *png = g_strdup_printf("%s/%s.png", icon_dir, id);
	gboolean found = FALSE;
	guint i;

	for (i = 0; i < blocks->len; ) {
		if (block_is(blocks->pdata[i], id)) {
			g_ptr_array_remove_index(blocks, i);
			found = TRUE;
		} else {
			i++;
		}
	}
	if (found) {
		ini_save(blocks);
		show_restart_bar();
	}
	g_unlink(png);
	g_free(png);
	g_ptr_array_free(blocks, TRUE);
	return found;
}

/* Name in the app's .desktop file, else FALLBACK */
static char *
desktop_name(const char *id, const char *fallback)
{
	char *file = g_strdup_printf("%s/applications/%s.desktop", exports, id);
	GKeyFile *kf = g_key_file_new();
	char *name = NULL;

	if (g_key_file_load_from_file(kf, file, G_KEY_FILE_NONE, NULL))
		name = g_key_file_get_locale_string(kf, "Desktop Entry", "Name", NULL, NULL);
	g_key_file_free(kf);
	g_free(file);
	if (!name || !*name) {
		g_free(name);
		name = g_strdup(fallback && *fallback ? fallback : id);
	}
	g_strdelimit(name, "\r\n", ' ');
	return name;
}

/* Writes a PNG of the app's icon for the taskbar; its path, or the generic icon */
static char *
launcher_icon(const char *id)
{
	char *src = find_icon(id), *png = NULL;
	GdkPixbuf *pb = src ? gdk_pixbuf_new_from_file_at_scale(src, LAUNCHER_ICON,
								 LAUNCHER_ICON, TRUE, NULL) : NULL;

	if (pb) {
		g_mkdir_with_parents(icon_dir, 0755);
		png = g_strdup_printf("%s/%s.png", icon_dir, id);
		if (!gdk_pixbuf_save(pb, png, "png", NULL, NULL))
			g_clear_pointer(&png, g_free);
		g_object_unref(pb);
	}
	g_free(src);
	return png ? png : g_strdup(FALLBACK_ICON);
}

static void
add_launcher(const char *id, const char *fallback_name)
{
	GPtrArray *blocks;
	char *name, *icon;

	remove_launcher(id);
	blocks = ini_blocks();
	name = desktop_name(id, fallback_name);
	icon = launcher_icon(id);
	g_ptr_array_add(blocks, g_strdup_printf("# app: %s\n[launcher]\ndisplayname=%s\n"
						"icon=%s\npath=/usr/bin/flatpak run %s\n"
						"app-id=%s\n", id, name, icon, id, id));
	ini_save(blocks);
	show_restart_bar();
	g_free(name);
	g_free(icon);
	g_ptr_array_free(blocks, TRUE);
}

/*
 * List rows
 */

static void
clear_list(GtkWidget *list)
{
	GList *rows = gtk_container_get_children(GTK_CONTAINER(list)), *l;

	for (l = rows; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(rows);
}

/* A row with a note only (empty list, error) */
static void
list_note(GtkWidget *list, const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_widget_set_margin_top(l, 24);
	gtk_widget_set_margin_bottom(l, 24);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_list_box_insert(GTK_LIST_BOX(list), l, -1);
	gtk_widget_show_all(list);
}

/* Icon, name, id/version, description; returns the box for the buttons */
static GtkWidget *
app_row(GtkWidget *list, const char *id, const char *name, const char *version,
	const char *description)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *l;
	char *markup;

	g_object_set(row, "margin", 8, NULL);
	gtk_box_pack_start(GTK_BOX(row), icon_image(id), FALSE, FALSE, 0);

	markup = g_markup_printf_escaped("<b>%s</b>", name && *name ? name : id);
	l = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(l), markup);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(text), l, FALSE, FALSE, 0);
	g_free(markup);

	if (description && *description) {
		l = gtk_label_new(description);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
		gtk_box_pack_start(GTK_BOX(text), l, FALSE, FALSE, 0);
	}

	markup = g_strdup_printf("%s%s%s", id, version && *version ? "  ·  " : "",
				 version ? version : "");
	l = gtk_label_new(markup);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_box_pack_start(GTK_BOX(text), l, FALSE, FALSE, 0);
	g_free(markup);

	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
	gtk_box_pack_end(GTK_BOX(row), buttons, FALSE, FALSE, 0);
	gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
	return buttons;
}

/* A button that remembers the app id and name */
static GtkWidget *
app_button(GtkWidget *box, const char *label, const char *id, const char *name,
	   GCallback cb, const char *css)
{
	GtkWidget *b = gtk_button_new_with_label(label);

	g_object_set_data_full(G_OBJECT(b), "id", g_strdup(id), g_free);
	g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name && *name ? name : id), g_free);
	if (css)
		gtk_style_context_add_class(gtk_widget_get_style_context(b), css);
	if (cb)
		g_signal_connect(b, "clicked", cb, NULL);
	gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
	return b;
}

#define BTN_ID(b)	((const char *) g_object_get_data(G_OBJECT(b), "id"))
#define BTN_NAME(b)	((const char *) g_object_get_data(G_OBJECT(b), "name"))

/*
 * Actions
 */

typedef struct {
	char *id, *name;
} AppRef;

static AppRef *
app_ref(GtkWidget *button)
{
	AppRef *a = g_new0(AppRef, 1);

	a->id = g_strdup(BTN_ID(button));
	a->name = g_strdup(BTN_NAME(button));
	return a;
}

static void
app_ref_free(AppRef *a)
{
	g_free(a->id);
	g_free(a->name);
	g_free(a);
}

static void
install_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	AppRef *a = data;
	char *t;

	set_busy(NULL);
	if (ok) {
		add_launcher(a->id, a->name);
		t = g_strdup_printf("%s is installed and in the Start menu.", a->name);
		gtk_label_set_text(GTK_LABEL(status), t);
		g_free(t);
	} else {
		char *why = last_line(*err ? err : out);

		t = g_strdup_printf("Could not install %s", a->name);
		message(GTK_MESSAGE_ERROR, t, why);
		g_free(t);
		g_free(why);
	}
	app_ref_free(a);
	refresh_installed();
}

static void
on_install(GtkButton *b, gpointer data)
{
	const char *remote = g_object_get_data(G_OBJECT(b), "remote");
	AppRef *a = app_ref(GTK_WIDGET(b));
	char *t = g_strdup_printf("Installing %s… (big apps take a few minutes)", a->name);

	set_busy(t);
	g_free(t);
	run_host(ARGV("flatpak", "install", "--user", "--noninteractive", "-y",
		      remote && *remote ? remote : "flathub", a->id), install_done, a);
}

static void
remove_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	AppRef *a = data;
	char *t;

	set_busy(NULL);
	if (ok) {
		remove_launcher(a->id);
		t = g_strdup_printf("%s was removed.", a->name);
		gtk_label_set_text(GTK_LABEL(status), t);
		g_free(t);
	} else {
		char *why = last_line(*err ? err : out);

		t = g_strdup_printf("Could not remove %s", a->name);
		message(GTK_MESSAGE_ERROR, t, why);
		g_free(t);
		g_free(why);
	}
	app_ref_free(a);
	refresh_installed();
}

static void
on_remove(GtkButton *b, gpointer data)
{
	AppRef *a;
	char *t;

	if (strcmp(BTN_ID(b), APP_ID) == 0 &&
	    !confirm("Remove the Store itself?",
		     "You can install it again with apps/hexa-app.", "Remove"))
		return;
	t = g_strdup_printf("Remove %s?", BTN_NAME(b));
	if (!confirm(t, "Its settings and saved data in ~/.var/app are kept.", "Remove")) {
		g_free(t);
		return;
	}
	g_free(t);
	a = app_ref(GTK_WIDGET(b));
	t = g_strdup_printf("Removing %s…", a->name);
	set_busy(t);
	g_free(t);
	run_host(ARGV("flatpak", "uninstall", "--user", "--noninteractive", "-y", a->id),
		 remove_done, a);
}

static void
ignore_done(gboolean ok, const char *out, const char *err, gpointer data)
{
}

static void
on_open(GtkButton *b, gpointer data)
{
	run_host(ARGV("flatpak", "run", BTN_ID(b)), ignore_done, NULL);
}

static void
on_menu_toggled(GtkToggleButton *t, gpointer data)
{
	const char *id = g_object_get_data(G_OBJECT(t), "id");
	const char *name = g_object_get_data(G_OBJECT(t), "name");

	if (gtk_toggle_button_get_active(t))
		add_launcher(id, name);
	else
		remove_launcher(id);
}

static void
update_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	set_busy(NULL);
	if (ok) {
		gtk_label_set_text(GTK_LABEL(status), "Everything is up to date.");
	} else {
		char *why = last_line(*err ? err : out);

		message(GTK_MESSAGE_ERROR, "Could not update", why);
		g_free(why);
	}
	refresh_installed();
	refresh_updates();
}

static void
on_update_all(GtkButton *b, gpointer data)
{
	set_busy("Updating apps and runtimes…");
	run_host(ARGV("flatpak", "update", "--user", "--noninteractive", "-y"), update_done, NULL);
}

/*
 * Pages
 */

static void
search_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	char ***rows;
	int i;

	set_busy(NULL);
	clear_list(search_list);
	if (!ok) {
		char *why = last_line(*err ? err : out);

		list_note(search_list, why);
		g_free(why);
		return;
	}
	rows = parse_columns(out);
	/* application, name, version, remotes, description */
	for (i = 0; rows[i] && i < MAX_RESULTS; i++) {
		const char *id = col(rows[i], 0);
		char **remotes = g_strsplit(col(rows[i], 3), ",", 2);
		GtkWidget *buttons;

		if (!*id || strstr(id, ".Locale") || strstr(id, ".Debug")) {
			g_strfreev(remotes);
			continue;
		}
		buttons = app_row(search_list, id, col(rows[i], 1), col(rows[i], 2),
				  col(rows[i], 4));
		if (g_hash_table_contains(installed, id)) {
			app_button(buttons, "Open", id, col(rows[i], 1), G_CALLBACK(on_open), NULL);
		} else {
			GtkWidget *b = app_button(buttons, "Install", id, col(rows[i], 1),
						  G_CALLBACK(on_install), "suggested-action");

			g_object_set_data_full(G_OBJECT(b), "remote",
					       g_strdup(remotes[0] ? remotes[0] : "flathub"), g_free);
		}
		g_strfreev(remotes);
	}
	if (!rows[0])
		list_note(search_list, "Nothing found. Try another word, in English.");
	free_columns(rows);
	gtk_widget_show_all(search_list);
}

static void
on_search(GtkEntry *e, gpointer data)
{
	char *text = g_strstrip(g_strdup(gtk_entry_get_text(e))), *t;

	if (*text) {
		t = g_strdup_printf("Searching for “%s”…", text);
		set_busy(t);
		g_free(t);
		run_host(ARGV("flatpak", "search",
			      "--columns=application,name,version,remotes,description", text),
			 search_done, NULL);
	}
	g_free(text);
}

static void
installed_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	char ***rows;
	int i;

	clear_list(installed_list);
	g_hash_table_remove_all(installed);
	if (!ok) {
		char *why = last_line(*err ? err : out);

		list_note(installed_list, why);
		g_free(why);
		return;
	}
	rows = parse_columns(out);
	/* application, name, version, origin */
	for (i = 0; rows[i]; i++) {
		const char *id = col(rows[i], 0), *name = col(rows[i], 1);
		GtkWidget *buttons, *check;

		if (!*id)
			continue;
		g_hash_table_insert(installed, g_strdup(id), g_strdup(name));
		buttons = app_row(installed_list, id, name, col(rows[i], 2), NULL);
		check = gtk_check_button_new_with_label("Start menu");
		gtk_widget_set_tooltip_text(check, "Show it in the Start menu "
					    "(after the desktop restarts)");
		g_object_set_data_full(G_OBJECT(check), "id", g_strdup(id), g_free);
		g_object_set_data_full(G_OBJECT(check), "name", g_strdup(name), g_free);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), launcher_exists(id));
		g_signal_connect(check, "toggled", G_CALLBACK(on_menu_toggled), NULL);
		gtk_box_pack_start(GTK_BOX(buttons), check, FALSE, FALSE, 6);
		app_button(buttons, "Open", id, name, G_CALLBACK(on_open), NULL);
		app_button(buttons, "Remove", id, name, G_CALLBACK(on_remove), "destructive-action");
	}
	if (!rows[0])
		list_note(installed_list, "No apps installed yet: find some in Discover.");
	free_columns(rows);
	gtk_widget_show_all(installed_list);
}

static void
refresh_installed(void)
{
	run_host(ARGV("flatpak", "list", "--user", "--app",
		      "--columns=application,name,version,origin"), installed_done, NULL);
}

static void
updates_done(gboolean ok, const char *out, const char *err, gpointer data)
{
	char ***rows;
	int i, n = 0;

	clear_list(updates_list);
	if (!ok) {
		char *why = last_line(*err ? err : out);

		list_note(updates_list, why);
		g_free(why);
		return;
	}
	rows = parse_columns(out);
	/* application, name, version */
	for (i = 0; rows[i]; i++) {
		if (!*col(rows[i], 0))
			continue;
		app_row(updates_list, col(rows[i], 0), col(rows[i], 1), col(rows[i], 2), NULL);
		n++;
	}
	if (!n)
		list_note(updates_list, "All apps are up to date.");
	gtk_button_set_label(GTK_BUTTON(update_all), n ? "Update all" : "Update runtimes");
	free_columns(rows);
	gtk_widget_show_all(updates_list);
}

static void
refresh_updates(void)
{
	clear_list(updates_list);
	list_note(updates_list, "Checking for updates…");
	run_host(ARGV("flatpak", "remote-ls", "--user", "--updates", "--app",
		      "--columns=application,name,version"), updates_done, NULL);
}

static void
on_refresh_updates(GtkButton *b, gpointer data)
{
	refresh_updates();
}

static void
on_restart_bar(GtkInfoBar *bar, int response, gpointer data)
{
	if (!confirm("Restart the desktop?",
		     "All open windows will close, so save your work first. "
		     "You come back to the desktop in a few seconds.", "Restart desktop"))
		return;
	run_host(ARGV("sh", "-c", "kill $(pidof weston)"), ignore_done, NULL);
}

static GtkWidget *
scrolled_list(GtkWidget **list)
{
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	*list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(*list), GTK_SELECTION_NONE);
	gtk_container_add(GTK_CONTAINER(sw), *list);
	return sw;
}

static GtkWidget *
page_new(void)
{
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	g_object_set(page, "margin", 12, NULL);
	return page;
}

static void
activate(GtkApplication *app, gpointer data)
{
	GtkWidget *header, *switcher, *main_box, *page, *bar, *l, *box, *b;

	if (win) {
		gtk_window_present(GTK_WINDOW(win));
		return;
	}
	g_object_set(gtk_settings_get_default(),
		     "gtk-application-prefer-dark-theme", TRUE, NULL);

	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "Store");
	gtk_window_set_default_size(GTK_WINDOW(win), 900, 640);
	header = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
	gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Store");
	gtk_window_set_titlebar(GTK_WINDOW(win), header);

	main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	restart_bar = gtk_info_bar_new();
	gtk_info_bar_set_message_type(GTK_INFO_BAR(restart_bar), GTK_MESSAGE_INFO);
	l = gtk_label_new("The Start menu changes when the desktop restarts "
			  "(open windows will close).");
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_show(l);
	gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(restart_bar))), l);
	gtk_info_bar_add_button(GTK_INFO_BAR(restart_bar), "Restart desktop", 1);
	g_signal_connect(restart_bar, "response", G_CALLBACK(on_restart_bar), NULL);
	gtk_widget_set_no_show_all(restart_bar, TRUE);
	gtk_box_pack_start(GTK_BOX(main_box), restart_bar, FALSE, FALSE, 0);

	content = stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

	/* Discover */
	page = page_new();
	search_entry = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry),
				       "Search Flathub (e.g. gimp, spotify, discord) and press Enter");
	g_signal_connect(search_entry, "activate", G_CALLBACK(on_search), NULL);
	gtk_box_pack_start(GTK_BOX(page), search_entry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page), scrolled_list(&search_list), TRUE, TRUE, 0);
	list_note(search_list, "Type the name of an app and press Enter.");
	gtk_stack_add_titled(GTK_STACK(stack), page, "discover", "Discover");

	/* Installed */
	page = page_new();
	gtk_box_pack_start(GTK_BOX(page), scrolled_list(&installed_list), TRUE, TRUE, 0);
	gtk_stack_add_titled(GTK_STACK(stack), page, "installed", "Installed");

	/* Updates */
	page = page_new();
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	update_all = gtk_button_new_with_label("Update all");
	gtk_style_context_add_class(gtk_widget_get_style_context(update_all), "suggested-action");
	g_signal_connect(update_all, "clicked", G_CALLBACK(on_update_all), NULL);
	b = gtk_button_new_with_label("Check again");
	g_signal_connect(b, "clicked", G_CALLBACK(on_refresh_updates), NULL);
	gtk_box_pack_end(GTK_BOX(box), update_all, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(box), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page), box, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page), scrolled_list(&updates_list), TRUE, TRUE, 0);
	gtk_stack_add_titled(GTK_STACK(stack), page, "updates", "Updates");

	switcher = gtk_stack_switcher_new();
	gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
	gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header), switcher);
	gtk_box_pack_start(GTK_BOX(main_box), stack, TRUE, TRUE, 0);

	/* Status line */
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	g_object_set(bar, "margin", 6, NULL);
	spinner = gtk_spinner_new();
	status = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(bar), spinner, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), status, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(main_box), bar, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
			 FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(win), main_box);
	gtk_widget_show_all(win);
	gtk_widget_grab_focus(search_entry);

	refresh_installed();
	refresh_updates();
}

int
main(int argc, char **argv)
{
	GtkApplication *app;
	const char *home = g_get_home_dir();
	int status_code;

	/* In the sandbox the XDG dirs point into ~/.var/app, so use the real ones */
	in_sandbox = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
	conf_dir = g_build_filename(home, ".config", "hexaos", NULL);
	apps_ini = g_build_filename(conf_dir, "apps.ini", NULL);
	icon_dir = g_build_filename(conf_dir, "icons", NULL);
	exports = g_build_filename(home, ".local", "share", "flatpak", "exports", "share", NULL);
	appstream_dir = g_build_filename(home, ".local", "share", "flatpak", "appstream", NULL);
	installed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	/* Without a window: flatpak run com.hexaos.Store --add-to-menu APP_ID */
	if (argc == 3 && strcmp(argv[1], "--add-to-menu") == 0) {
		add_launcher(argv[2], NULL);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--remove-from-menu") == 0)
		return remove_launcher(argv[2]) ? 0 : 1;

	app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status_code = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status_code;
}
