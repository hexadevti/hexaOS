/*
 * hexa-portal: a small desktop portal for hexaOS (org.freedesktop.portal.Desktop).
 *
 * Flatpak apps cannot start host programs, so to open a link or a file they
 * ask the portal on the session bus. hexaOS has no xdg-desktop-portal; this
 * implements the two interfaces apps need for that:
 *
 *   OpenURI   links -> xdg-open (the default browser of Settings > Default
 *             apps, or the app that registered the link type); files and
 *             folders passed as file descriptors -> xdg-open / Files
 *   Settings  answers the appearance questions apps ask at start-up
 *             (no dark mode preference), instead of a missing service
 *
 * D-Bus starts it on the first call (dbus-1/services/...Desktop.service).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define BUS_NAME "org.freedesktop.portal.Desktop"
#define OBJ_PATH "/org/freedesktop/portal/desktop"
#define XDG_OPEN "/usr/bin/xdg-open"

static const char introspection_xml[] =
	"<node>"
	" <interface name='org.freedesktop.portal.OpenURI'>"
	"  <method name='OpenURI'>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='s' name='uri' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='o' name='handle' direction='out'/>"
	"  </method>"
	"  <method name='OpenFile'>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='h' name='fd' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='o' name='handle' direction='out'/>"
	"  </method>"
	"  <method name='OpenDirectory'>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='h' name='fd' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='o' name='handle' direction='out'/>"
	"  </method>"
	"  <method name='SchemeSupported'>"
	"   <arg type='s' name='scheme' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='b' name='supported' direction='out'/>"
	"  </method>"
	"  <property name='version' type='u' access='read'/>"
	" </interface>"
	" <interface name='org.freedesktop.portal.Settings'>"
	"  <method name='ReadAll'>"
	"   <arg type='as' name='namespaces' direction='in'/>"
	"   <arg type='a{sa{sv}}' name='value' direction='out'/>"
	"  </method>"
	"  <method name='Read'>"
	"   <arg type='s' name='namespace' direction='in'/>"
	"   <arg type='s' name='key' direction='in'/>"
	"   <arg type='v' name='value' direction='out'/>"
	"  </method>"
	"  <method name='ReadOne'>"
	"   <arg type='s' name='namespace' direction='in'/>"
	"   <arg type='s' name='key' direction='in'/>"
	"   <arg type='v' name='value' direction='out'/>"
	"  </method>"
	"  <signal name='SettingChanged'>"
	"   <arg type='s' name='namespace'/>"
	"   <arg type='s' name='key'/>"
	"   <arg type='v' name='value'/>"
	"  </signal>"
	"  <property name='version' type='u' access='read'/>"
	" </interface>"
	"</node>";

static GMainLoop *loop;

/*
 * Requests: the caller waits for a Response signal on
 * /org/freedesktop/portal/desktop/request/SENDER/TOKEN (SENDER is its bus name
 * without ':' and with '.' -> '_', TOKEN its "handle_token" option).
 */
static char *
request_path(const char *sender, GVariant *options)
{
	static unsigned counter;
	const char *token = NULL;
	char *s, *p, *path, buf[32];

	if (!g_variant_lookup(options, "handle_token", "&s", &token) || !*token)
		token = NULL;
	for (p = (char *)token; token && *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '_') {
			token = NULL;
			break;
		}
	if (!token) {
		g_snprintf(buf, sizeof buf, "hexa%u", ++counter);
		token = buf;
	}
	s = g_strdup(sender[0] == ':' ? sender + 1 : sender);
	for (p = s; *p; p++)
		if (*p == '.')
			*p = '_';
	path = g_strdup_printf(OBJ_PATH "/request/%s/%s", s, token);
	g_free(s);
	return path;
}

/* Answers the call with the request handle, then reports the result */
static void
finish_request(GDBusMethodInvocation *inv, GVariant *options, guint32 response)
{
	GDBusConnection *bus = g_dbus_method_invocation_get_connection(inv);
	const char *sender = g_dbus_method_invocation_get_sender(inv);
	char *path = request_path(sender ? sender : "unknown", options);

	g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", path));
	g_dbus_connection_emit_signal(bus, sender, path, "org.freedesktop.portal.Request",
				      "Response", g_variant_new("(u@a{sv})", response,
				      g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)), NULL);
	g_free(path);
}

static gboolean
run(const char *prog, const char *arg)
{
	const char *argv[] = { prog, arg, NULL };
	GError *err = NULL;

	if (!g_spawn_async(NULL, (char **)argv, NULL,
			   G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err)) {
		g_warning("%s: %s", prog, err->message);
		g_error_free(err);
		return FALSE;
	}
	return TRUE;
}

static void
open_uri(GDBusMethodInvocation *inv, GVariant *params)
{
	const char *parent, *uri;
	GVariant *options;
	char *scheme;

	g_variant_get(params, "(&s&s@a{sv})", &parent, &uri, &options);
	scheme = g_uri_parse_scheme(uri);
	if (!scheme || g_ascii_strcasecmp(scheme, "file") == 0) {
		/* local files come as file descriptors (OpenFile), as in xdg-desktop-portal */
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			scheme ? "Use OpenFile for local files" : "Not a URI: %s", uri);
	} else {
		finish_request(inv, options, run(XDG_OPEN, uri) ? 0 : 2);
	}
	g_free(scheme);
	g_variant_unref(options);
}

/* The host path of a file descriptor sent by the app */
static char *
fd_path(GDBusMethodInvocation *inv, int index)
{
	GUnixFDList *list = g_dbus_message_get_unix_fd_list(
		g_dbus_method_invocation_get_message(inv));
	char link[64], *path;
	ssize_t n;
	int fd;

	if (!list || (fd = g_unix_fd_list_get(list, index, NULL)) < 0)
		return NULL;
	g_snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
	path = g_malloc(4096);
	n = readlink(link, path, 4095);
	close(fd);
	if (n <= 0 || path[0] != '/' || (path[n] = 0, g_str_has_suffix(path, " (deleted)")) ||
	    !g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_free(path);
		return NULL;
	}
	return path;
}

static void
open_file(GDBusMethodInvocation *inv, GVariant *params, gboolean directory)
{
	const char *parent;
	GVariant *options;
	gint32 index;
	char *path, *dir;
	gboolean ok;

	g_variant_get(params, "(&sh@a{sv})", &parent, &index, &options);
	if (!(path = fd_path(inv, index))) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
						      "Bad file descriptor");
		g_variant_unref(options);
		return;
	}
	if (directory && !g_file_test(path, G_FILE_TEST_IS_DIR)) {
		/* the folder that holds the file */
		dir = g_path_get_dirname(path);
		ok = run(XDG_OPEN, dir);
		g_free(dir);
	} else {
		ok = run(XDG_OPEN, path);
	}
	finish_request(inv, options, ok ? 0 : 2);
	g_free(path);
	g_variant_unref(options);
}

static gboolean
scheme_supported(const char *scheme)
{
	GAppInfo *app;

	if (!g_ascii_strcasecmp(scheme, "http") || !g_ascii_strcasecmp(scheme, "https"))
		return TRUE;
	if ((app = g_app_info_get_default_for_uri_scheme(scheme))) {
		g_object_unref(app);
		return TRUE;
	}
	return FALSE;
}

/* org.freedesktop.appearance: no dark/light preference, normal contrast */
static GVariant *
setting(const char *ns, const char *key)
{
	if (g_strcmp0(ns, "org.freedesktop.appearance"))
		return NULL;
	if (!g_strcmp0(key, "color-scheme") || !g_strcmp0(key, "contrast"))
		return g_variant_new_uint32(0);
	return NULL;
}

static gboolean
namespace_wanted(const char *ns, const char *const *wanted)
{
	if (!wanted || !*wanted)
		return TRUE;
	for (; *wanted; wanted++) {
		size_t n = strlen(*wanted);

		if (!**wanted || !strcmp(*wanted, ns) ||
		    (n && (*wanted)[n - 1] == '*' && !strncmp(*wanted, ns, n - 1)))
			return TRUE;
	}
	return FALSE;
}

static void
settings_read_all(GDBusMethodInvocation *inv, GVariant *params)
{
	const char **wanted;
	GVariantBuilder all, ns;

	g_variant_get(params, "(^a&s)", &wanted);
	g_variant_builder_init(&all, G_VARIANT_TYPE("a{sa{sv}}"));
	if (namespace_wanted("org.freedesktop.appearance", wanted)) {
		g_variant_builder_init(&ns, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&ns, "{sv}", "color-scheme", g_variant_new_uint32(0));
		g_variant_builder_add(&ns, "{sv}", "contrast", g_variant_new_uint32(0));
		g_variant_builder_add(&all, "{s@a{sv}}", "org.freedesktop.appearance",
				      g_variant_builder_end(&ns));
	}
	g_free(wanted);
	g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sa{sv}})",
					      g_variant_builder_end(&all)));
}

static void
settings_read(GDBusMethodInvocation *inv, GVariant *params, gboolean one)
{
	const char *ns, *key;
	GVariant *v;

	g_variant_get(params, "(&s&s)", &ns, &key);
	if (!(v = setting(ns, key))) {
		g_dbus_method_invocation_return_dbus_error(inv,
			"org.freedesktop.portal.Error.NotFound", "Requested setting not found");
		return;
	}
	/* Read (the old method) wraps the value in one more variant */
	if (!one)
		v = g_variant_new_variant(v);
	g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
}

static void
method_call(GDBusConnection *bus, const char *sender, const char *path,
	    const char *iface, const char *method, GVariant *params,
	    GDBusMethodInvocation *inv, gpointer data)
{
	if (!strcmp(iface, "org.freedesktop.portal.OpenURI")) {
		if (!strcmp(method, "OpenURI"))
			open_uri(inv, params);
		else if (!strcmp(method, "OpenFile"))
			open_file(inv, params, FALSE);
		else if (!strcmp(method, "OpenDirectory"))
			open_file(inv, params, TRUE);
		else if (!strcmp(method, "SchemeSupported")) {
			const char *scheme;

			g_variant_get(params, "(&s@a{sv})", &scheme, NULL);
			g_dbus_method_invocation_return_value(inv,
				g_variant_new("(b)", scheme_supported(scheme)));
		}
	} else {
		if (!strcmp(method, "ReadAll"))
			settings_read_all(inv, params);
		else
			settings_read(inv, params, !strcmp(method, "ReadOne"));
	}
}

static GVariant *
get_property(GDBusConnection *bus, const char *sender, const char *path,
	     const char *iface, const char *prop, GError **err, gpointer data)
{
	return g_variant_new_uint32(!strcmp(iface, "org.freedesktop.portal.OpenURI") ? 4 : 2);
}

static const GDBusInterfaceVTable vtable = { method_call, get_property, NULL };

static void
on_bus(GDBusConnection *bus, const char *name, gpointer data)
{
	GDBusNodeInfo *info = data;
	GError *err = NULL;
	int i;

	for (i = 0; info->interfaces[i]; i++)
		if (!g_dbus_connection_register_object(bus, OBJ_PATH, info->interfaces[i],
						       &vtable, NULL, NULL, &err)) {
			g_printerr("hexa-portal: %s\n", err->message);
			g_clear_error(&err);
		}
}

static void
on_name_lost(GDBusConnection *bus, const char *name, gpointer data)
{
	/* another portal is running (or no session bus) */
	g_main_loop_quit(loop);
}

int
main(int argc, char **argv)
{
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);

	loop = g_main_loop_new(NULL, FALSE);
	g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		       on_bus, NULL, on_name_lost, info, NULL);
	g_main_loop_run(loop);
	return 0;
}
