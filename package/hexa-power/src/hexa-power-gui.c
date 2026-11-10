/*
 * hexa-power-gui: the hexaOS power window (GTK 3)
 *
 * Opened by "hexa-power" on the desktop (Start menu > Power, the Start button's
 * right-click menu, or typing hexa-power in a terminal). Three big choices, as
 * on Windows: Shut down, Restart, and Restart the desktop. Esc or Cancel closes
 * it. In text mode hexa-power shows its text menu instead.
 *
 * Restart and shut down need no password for the wheel group
 * (/etc/sudoers.d/power); otherwise the window asks for one.
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_ID		"org.hexaos.Power"
/* Wayland app-id: the taskbar and Start menu know the window by it */
#define WINDOW_APP_ID	"hexa-power"

enum action { ACT_POWEROFF, ACT_REBOOT, ACT_DESKTOP };

static GtkWidget *win, *buttons, *status;

static const char css[] =
	".hx-title { font-size: 22px; font-weight: 300; }"
	".hx-dim { opacity: 0.68; }"
	".hx-choice { padding: 18px 10px 14px 10px; min-width: 150px; border-radius: 8px; }"
	".hx-choice image { margin-bottom: 8px; }"
	".hx-choice.hx-off image { color: #f87171; }"
	".hx-choice.hx-reboot image { color: #22d3ee; }"
	".hx-choice.hx-desktop image { color: #93c5fd; }"
	".hx-choice-name { font-weight: bold; font-size: 14px; }"
	".hx-choice-text { opacity: 0.68; font-size: 11px; }";

static void
add_class(GtkWidget *w, const char *name)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), name);
}

static void
show_error(const char *title, const char *text)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
					      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
					      "%s", title);

	if (text && *text)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", text);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* The user's password for sudo, or NULL when cancelled */
static char *
ask_password(void)
{
	GtkWidget *d, *area, *label, *entry;
	char *pw = NULL;

	d = gtk_dialog_new_with_buttons("Password", GTK_WINDOW(win), GTK_DIALOG_MODAL,
					"Cancel", GTK_RESPONSE_CANCEL,
					"OK", GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	area = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(area), 14);
	gtk_box_set_spacing(GTK_BOX(area), 10);
	label = gtk_label_new("This account needs its password to do that.");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	entry = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
	gtk_widget_show_all(d);

	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(d);
	return pw;
}

/* Runs /sbin/CMD as root: directly as root, otherwise through sudo (first
 * without a password, then asking for one). FALSE with *err set on failure. */
static gboolean
run_as_root(const char *cmd, char **err)
{
	char *path = g_strconcat("/sbin/", cmd, NULL);
	char *pw, *input;
	int wait_status = 0;
	gboolean ok;
	GError *e = NULL;

	*err = NULL;
	if (getuid() == 0) {
		const char *argv[] = { path, NULL };

		ok = g_spawn_sync(NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
				  NULL, err, &wait_status, &e) &&
		     g_spawn_check_wait_status(wait_status, NULL);
		goto out;
	}

	{
		const char *argv[] = { "sudo", "-n", path, NULL };

		ok = g_spawn_sync(NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH,
				  NULL, NULL, NULL, err, &wait_status, &e) &&
		     g_spawn_check_wait_status(wait_status, NULL);
	}
	if (ok || e)
		goto out;

	/* Not allowed without a password: ask for it */
	pw = ask_password();
	if (!pw) {
		g_free(*err);
		*err = NULL;
		goto out;
	}
	input = g_strconcat(pw, "\n", NULL);
	memset(pw, 0, strlen(pw));
	g_free(pw);
	g_free(*err);
	*err = NULL;
	{
		const char *argv[] = { "sudo", "-S", "-p", "", path, NULL };
		GSubprocess *p;

		p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE |
				      G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
				      G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
		if (p) {
			ok = g_subprocess_communicate_utf8(p, input, NULL, NULL, err, &e) &&
			     g_subprocess_get_successful(p);
			g_object_unref(p);
		}
	}
	memset(input, 0, strlen(input));
	g_free(input);
	if (!ok && !e && (!*err || !**err)) {
		g_free(*err);
		*err = g_strdup("Wrong password, or this account may not "
				"restart or shut down the computer.");
	}

out:
	if (e) {
		g_free(*err);
		*err = g_strdup(e->message);
		g_error_free(e);
	}
	if (*err)
		g_strstrip(*err);
	g_free(path);
	return ok;
}

/* pid of our own process named NAME (e.g. weston), or -1 */
static pid_t
find_own_proc(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	pid_t found = -1;

	if (!d)
		return -1;
	while ((de = readdir(d)) && found < 0) {
		char path[300], comm[64] = "";
		FILE *f;
		struct stat st;

		if (!isdigit((unsigned char) de->d_name[0]))
			continue;
		snprintf(path, sizeof path, "/proc/%s", de->d_name);
		if (stat(path, &st) != 0 || st.st_uid != getuid())
			continue;
		snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof comm, f)) {
			comm[strcspn(comm, "\n")] = '\0';
			if (strcmp(comm, name) == 0)
				found = atoi(de->d_name);
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

static void
busy(const char *text)
{
	gtk_widget_set_sensitive(buttons, FALSE);
	gtk_label_set_text(GTK_LABEL(status), text);
	gtk_widget_show(status);
	/* draw it before the system goes down */
	while (gtk_events_pending())
		gtk_main_iteration();
}

static void
idle_again(void)
{
	gtk_widget_set_sensitive(buttons, TRUE);
	gtk_widget_hide(status);
}

static void
on_choice(GtkButton *b, gpointer data)
{
	enum action act = GPOINTER_TO_INT(data);
	char *err = NULL;
	pid_t pid;

	switch (act) {
	case ACT_POWEROFF:
	case ACT_REBOOT:
		busy(act == ACT_POWEROFF ? "Shutting down…" : "Restarting…");
		sync();
		if (run_as_root(act == ACT_POWEROFF ? "poweroff" : "reboot", &err))
			break;		/* the system is going down */
		idle_again();
		if (err)	/* NULL: the password was not given */
			show_error(act == ACT_POWEROFF ? "Could not shut down"
						       : "Could not restart", err);
		g_free(err);
		break;
	case ACT_DESKTOP:
		pid = find_own_proc("weston");
		if (pid > 0 && kill(pid, SIGTERM) == 0) {
			busy("Restarting the desktop…");
			break;
		}
		show_error("Could not restart the desktop",
			   "The desktop (weston) is not running for this account.");
		break;
	}
}

static GtkWidget *
choice_new(const char *icon, const char *name, const char *text,
	   const char *css_class, enum action act)
{
	GtkWidget *b = gtk_button_new(), *box, *img, *l;

	add_class(b, "hx-choice");
	add_class(b, css_class);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
	gtk_image_set_pixel_size(GTK_IMAGE(img), 40);
	gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
	l = gtk_label_new(name);
	add_class(l, "hx-choice-name");
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	l = gtk_label_new(text);
	add_class(l, "hx-choice-text");
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 20);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	g_signal_connect(b, "clicked", G_CALLBACK(on_choice), GINT_TO_POINTER(act));
	return b;
}

static gboolean
on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	if (ev->keyval == GDK_KEY_Escape) {
		gtk_widget_destroy(win);
		return TRUE;
	}
	return FALSE;
}

static void
on_cancel(GtkButton *b, gpointer data)
{
	gtk_widget_destroy(win);
}

static void
activate(GtkApplication *app, gpointer data)
{
	GtkWidget *header, *box, *l, *off, *bottom, *cancel;
	GtkCssProvider *provider;

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

	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "Power");
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_icon_name(GTK_WINDOW(win), "system-shutdown");
	header = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
	gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Power");
	gtk_window_set_titlebar(GTK_WINDOW(win), header);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(box), 22);
	gtk_container_add(GTK_CONTAINER(win), box);

	l = gtk_label_new("What do you want to do?");
	add_class(l, "hx-title");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	l = gtk_label_new("Save your work first: open programs will close.");
	add_class(l, "hx-dim");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_margin_bottom(l, 14);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);

	buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
	off = choice_new("system-shutdown-symbolic", "Shut down",
			 "Turn the computer off", "hx-off", ACT_POWEROFF);
	gtk_box_pack_start(GTK_BOX(buttons), off, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(buttons),
		choice_new("system-reboot-symbolic", "Restart",
			   "Turn the computer off and on again", "hx-reboot",
			   ACT_REBOOT), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(buttons),
		choice_new("view-refresh-symbolic", "Restart desktop",
			   "Close all windows and start the desktop again",
			   "hx-desktop", ACT_DESKTOP), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

	bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_set_margin_top(bottom, 16);
	status = gtk_label_new("");
	add_class(status, "hx-dim");
	gtk_widget_set_no_show_all(status, TRUE);
	gtk_box_pack_start(GTK_BOX(bottom), status, FALSE, FALSE, 0);
	cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), NULL);
	gtk_box_pack_end(GTK_BOX(bottom), cancel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	gtk_widget_grab_focus(off);
}

int
main(int argc, char **argv)
{
	GtkApplication *app;
	GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
	int status_code;

	/* single instance through the session bus, when there is one */
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;

	g_set_prgname(WINDOW_APP_ID);	/* GTK3 sends it as the Wayland app-id */
	app = gtk_application_new(APP_ID, flags);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status_code = g_application_run(G_APPLICATION(app), 1, argv);
	g_object_unref(app);
	return status_code;
}
