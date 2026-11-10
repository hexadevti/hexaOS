/*
 * hexa-files: the hexaOS file manager (GTK 3), laid out like Windows Explorer.
 *
 *   sidebar   Home, the user folders, Trash, the whole disk (Computer) and
 *             drives mounted under /media, /mnt or /run/media
 *   toolbar   back, forward, up, the address bar (type a path) and search
 *             (filters the open folder)
 *   files     large icons or a details list (Ctrl+1 / Ctrl+2)
 *
 * Copy, cut and paste (in the background), rename, new folder or text
 * document, move to Trash (and restore from the Trash view), delete
 * permanently, properties, "open with" another app, and a terminal in the
 * open folder. Double-click opens a file with its default app.
 *
 * Usage: hexa-files [FOLDER...]   one window per folder; when the app is
 * already running, the new windows open in it.
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#define APP_ID	"org.hexaos.Files"
#define ATTRS	"standard::name,standard::display-name,standard::type," \
		"standard::size,standard::icon,standard::fast-content-type," \
		"standard::is-hidden,standard::is-backup,time::modified"

enum {
	COL_ICON,	/* GIcon */
	COL_NAME,	/* display name */
	COL_KEY,	/* collation key of the name, for sorting */
	COL_PATH,	/* full path */
	COL_IS_DIR,
	COL_HIDDEN,
	COL_SIZE,	/* bytes (files only) */
	COL_MTIME,	/* seconds since 1970 */
	COL_SIZE_TEXT,
	COL_DATE_TEXT,
	COL_TYPE_TEXT,	/* type, or the original folder in the Trash */
	N_COLS
};

typedef struct {
	GtkWidget *window, *back_btn, *fwd_btn, *up_btn, *address, *search;
	GtkWidget *sidebar, *stack, *icons, *list, *status, *free_label;
	GtkWidget *view_btn, *hidden_item, *empty_trash_item;
	GtkWidget *icons_scroll, *list_scroll;
	GtkTreeViewColumn *type_col;
	GtkListStore *store;
	GtkTreeModel *filter, *sort;
	char *cwd;			/* open folder */
	char *error;			/* why it could not be listed */
	char *search_fold;		/* search text, case-folded */
	char *pending_select;		/* select this path on the next reload */
	GQueue back, forward;		/* folders (char *) */
	gboolean show_hidden, list_view;
	GFileMonitor *monitor;
	guint reload_id, drives_id;
	char *mounts;			/* drive lines shown in the sidebar */
	GHashTable *types;		/* content type -> description */
} Win;

static GtkApplication *app;
static GPtrArray *clip;			/* copied or cut paths */
static char *clip_text;			/* what we put on the system clipboard */
static gboolean clip_cut;
static char *busy_text;			/* background copy/move/delete */

static void go(Win *w, const char *path, gboolean history);
static void update_status(Win *w);
static Win *new_window(const char *path);

/*
 * Small helpers
 */

static const char *
trash_files(void)
{
	static char *dir;

	if (!dir)
		dir = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
	return dir;
}

static char *
trash_info_path(const char *name)
{
	char *base = g_strconcat(name, ".trashinfo", NULL);
	char *path = g_build_filename(g_get_user_data_dir(), "Trash", "info",
				      base, NULL);

	g_free(base);
	return path;
}

static gboolean
in_trash(Win *w)
{
	return g_strcmp0(w->cwd, trash_files()) == 0;
}

/* The original location of a file in the Trash (from its .trashinfo) */
static char *
trash_origin(const char *name)
{
	char *info = trash_info_path(name), *esc, *orig = NULL;
	GKeyFile *kf = g_key_file_new();

	if (g_key_file_load_from_file(kf, info, G_KEY_FILE_NONE, NULL)) {
		esc = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
		if (esc)
			orig = g_uri_unescape_string(esc, NULL);
		g_free(esc);
	}
	g_key_file_free(kf);
	g_free(info);
	return orig;
}

static Win *
win_of(GtkWindow *window)
{
	return window ? g_object_get_data(G_OBJECT(window), "hexa-win") : NULL;
}

static GtkWindow *
active_window(void)
{
	return gtk_application_get_active_window(app);
}

static void
message(GtkWindow *parent, GtkMessageType type, const char *text,
	const char *detail)
{
	GtkWidget *d = gtk_message_dialog_new(parent,
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		type, GTK_BUTTONS_CLOSE, "%s", text);

	if (detail)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", detail);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static void
error_box(Win *w, const char *text, const char *detail)
{
	message(w ? GTK_WINDOW(w->window) : active_window(),
		GTK_MESSAGE_ERROR, text, detail);
}

static gboolean
confirm(Win *w, const char *text, const char *detail, const char *ok)
{
	GtkWidget *d, *btn;
	int r;

	d = gtk_message_dialog_new(GTK_WINDOW(w->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", text);
	if (detail)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", detail);
	gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
	btn = gtk_dialog_add_button(GTK_DIALOG(d), ok, GTK_RESPONSE_ACCEPT);
	gtk_style_context_add_class(gtk_widget_get_style_context(btn),
				    "destructive-action");
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
	r = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return r == GTK_RESPONSE_ACCEPT;
}

/* dir/name, or dir/"name (2).ext", "name (3).ext"... if that is taken */
static char *
unique_target(const char *dir, const char *name)
{
	char *path = g_build_filename(dir, name, NULL);
	const char *dot = strrchr(name, '.');
	char *stem, *cand;
	int n;

	if (!g_file_test(path, G_FILE_TEST_EXISTS) &&
	    !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
		return path;
	g_free(path);

	if (!dot || dot == name)	/* no extension, or a dot-file */
		dot = name + strlen(name);
	stem = g_strndup(name, dot - name);
	for (n = 2; ; n++) {
		cand = g_strdup_printf("%s (%d)%s", stem, n, dot);
		path = g_build_filename(dir, cand, NULL);
		g_free(cand);
		if (!g_file_test(path, G_FILE_TEST_EXISTS) &&
		    !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
			break;
		g_free(path);
	}
	g_free(stem);
	return path;
}

/* Is path equal to dir or inside it? */
static gboolean
path_within(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return strncmp(path, dir, n) == 0 &&
	       (path[n] == '\0' || path[n] == '/' || strcmp(dir, "/") == 0);
}

/*
 * File operations (also run in a worker thread: GIO calls only)
 */

static gboolean
is_real_dir(GFile *f)
{
	return g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				      NULL) == G_FILE_TYPE_DIRECTORY;
}

static gboolean
copy_tree(GFile *src, GFile *dst, GError **error)
{
	GFileEnumerator *en;
	GFileInfo *info;
	gboolean ok = TRUE;

	if (!is_real_dir(src))
		return g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
				   NULL, NULL, NULL, error);

	if (!g_file_make_directory(dst, NULL, error))
		return FALSE;
	en = g_file_enumerate_children(src, "standard::name",
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
	if (!en)
		return FALSE;
	while (ok && (info = g_file_enumerator_next_file(en, NULL, error))) {
		const char *name = g_file_info_get_name(info);
		GFile *s = g_file_get_child(src, name);
		GFile *d = g_file_get_child(dst, name);

		ok = copy_tree(s, d, error);
		g_object_unref(s);
		g_object_unref(d);
		g_object_unref(info);
	}
	if (ok && *error)		/* next_file failed */
		ok = FALSE;
	g_object_unref(en);
	return ok;
}

static gboolean
delete_tree(GFile *f, GError **error)
{
	GFileEnumerator *en;
	GFileInfo *info;
	gboolean ok = TRUE;

	if (is_real_dir(f)) {
		en = g_file_enumerate_children(f, "standard::name",
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
		if (!en)
			return FALSE;
		while (ok && (info = g_file_enumerator_next_file(en, NULL, error))) {
			GFile *c = g_file_get_child(f, g_file_info_get_name(info));

			ok = delete_tree(c, error);
			g_object_unref(c);
			g_object_unref(info);
		}
		if (ok && *error)
			ok = FALSE;
		g_object_unref(en);
		if (!ok)
			return FALSE;
	}
	return g_file_delete(f, NULL, error);
}

/* Rename when possible; across disks, copy and then delete the original */
static gboolean
move_file(GFile *src, GFile *dst, GError **error)
{
	GError *e = NULL;

	if (g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS |
			G_FILE_COPY_NO_FALLBACK_FOR_MOVE, NULL, NULL, NULL, &e))
		return TRUE;
	if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
	    !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
		g_propagate_error(error, e);
		return FALSE;
	}
	g_error_free(e);
	return copy_tree(src, dst, error) && delete_tree(src, error);
}

/*
 * Background operations: copy, move, delete
 */

typedef enum { OP_COPY, OP_MOVE, OP_DELETE } OpKind;

typedef struct {
	OpKind kind;
	GPtrArray *src;
	char *dest;			/* folder (copy/move) */
	char *error;
	gboolean from_cut;
} Op;

static void
op_free(Op *op)
{
	g_ptr_array_unref(op->src);
	g_free(op->dest);
	g_free(op->error);
	g_free(op);
}

static void
refresh_all_status(void)
{
	GList *l;

	for (l = gtk_application_get_windows(app); l; l = l->next) {
		Win *w = win_of(l->data);

		if (w)
			update_status(w);
	}
}

static gboolean
set_busy_cb(gpointer text)
{
	g_free(busy_text);
	busy_text = text;
	refresh_all_status();
	return G_SOURCE_REMOVE;
}

static void
op_thread(GTask *task, gpointer source, gpointer data, GCancellable *c)
{
	static const char *verbs[] = { "Copying", "Moving", "Deleting" };
	Op *op = data;
	guint i;

	for (i = 0; i < op->src->len && !op->error; i++) {
		const char *src = op->src->pdata[i];
		char *base = g_path_get_basename(src);
		char *parent = g_path_get_dirname(src);
		GFile *s = g_file_new_for_path(src);
		GError *e = NULL;

		g_main_context_invoke(NULL, set_busy_cb,
			g_strdup_printf("%s \"%s\" (%u of %u)...", verbs[op->kind],
					base, i + 1, op->src->len));

		if (op->kind == OP_DELETE) {
			if (!delete_tree(s, &e))
				op->error = g_strdup_printf("%s: %s", base, e->message);
			else if (strcmp(parent, trash_files()) == 0) {
				char *info = trash_info_path(base);

				g_unlink(info);
				g_free(info);
			}
		} else if (op->kind == OP_MOVE && strcmp(parent, op->dest) == 0) {
			/* cut and pasted into the same folder: nothing to do */
		} else if (is_real_dir(s) && path_within(op->dest, src)) {
			op->error = g_strdup_printf(
				"\"%s\" cannot be put inside itself.", base);
		} else {
			char *target = unique_target(op->dest, base);
			GFile *d = g_file_new_for_path(target);
			gboolean ok = op->kind == OP_MOVE ? move_file(s, d, &e)
							  : copy_tree(s, d, &e);

			if (!ok)
				op->error = g_strdup_printf("%s: %s", base, e->message);
			g_object_unref(d);
			g_free(target);
		}
		g_clear_error(&e);
		g_object_unref(s);
		g_free(base);
		g_free(parent);
	}
	g_task_return_boolean(task, TRUE);
}

static void
op_done(GObject *source, GAsyncResult *res, gpointer data)
{
	Op *op = g_task_get_task_data(G_TASK(res));
	static const char *what[] = { "Copying failed", "Moving failed",
				      "Deleting failed" };

	g_clear_pointer(&busy_text, g_free);
	refresh_all_status();
	if (op->kind == OP_MOVE && op->from_cut && !op->error && clip) {
		g_clear_pointer(&clip, g_ptr_array_unref);
		g_clear_pointer(&clip_text, g_free);
	}
	if (op->error)
		error_box(NULL, what[op->kind], op->error);
	g_application_release(G_APPLICATION(app));
}

/* Takes the paths array */
static void
run_op(OpKind kind, GPtrArray *src, const char *dest, gboolean from_cut)
{
	Op *op = g_new0(Op, 1);
	GTask *task = g_task_new(NULL, NULL, op_done, NULL);

	op->kind = kind;
	op->src = src;
	op->dest = g_strdup(dest);
	op->from_cut = from_cut;
	g_task_set_task_data(task, op, (GDestroyNotify) op_free);
	g_application_hold(G_APPLICATION(app));	/* finish even if the window closes */
	g_task_run_in_thread(task, op_thread);
	g_object_unref(task);
}

/*
 * Listing a folder
 */

static const char *
type_text(Win *w, const char *ct, gboolean is_dir)
{
	char *desc;

	if (is_dir)
		return "File folder";
	if (!ct)
		return "File";
	desc = g_hash_table_lookup(w->types, ct);
	if (!desc) {
		desc = g_content_type_get_description(ct);
		g_hash_table_insert(w->types, g_strdup(ct), desc);
	}
	return desc;
}

static void
add_info(Win *w, GtkListStore *store, GFileInfo *info, gboolean trash)
{
	const char *name = g_file_info_get_name(info);
	const char *disp = g_file_info_get_display_name(info);
	const char *ct = g_file_info_get_attribute_string(info,
		G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
	gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
	gboolean hidden = g_file_info_get_is_hidden(info) ||
			  g_file_info_get_is_backup(info);
	/* files that cannot be read (e.g. no permission) may come without a size */
	guint64 size = is_dir || !g_file_info_has_attribute(info,
		G_FILE_ATTRIBUTE_STANDARD_SIZE) ? 0 : (guint64) g_file_info_get_size(info);
	GDateTime *dt = g_file_info_get_modification_date_time(info);
	gint64 mtime = 0;
	char *date = NULL, *path, *key, *size_text, *origin = NULL;

	if (dt) {
		GDateTime *local = g_date_time_to_local(dt);

		mtime = g_date_time_to_unix(dt);
		date = g_date_time_format(local, "%d/%m/%Y %H:%M");
		g_date_time_unref(local);
		g_date_time_unref(dt);
	}
	if (trash) {
		char *orig = trash_origin(name);

		origin = orig ? g_path_get_dirname(orig) : g_strdup("");
		g_free(orig);
		hidden = FALSE;
	}
	path = g_build_filename(w->cwd, name, NULL);
	key = g_utf8_collate_key_for_filename(disp, -1);
	size_text = is_dir ? g_strdup("") : g_format_size(size);

	gtk_list_store_insert_with_values(store, NULL, -1,
		COL_ICON, g_file_info_get_icon(info),
		COL_NAME, disp,
		COL_KEY, key,
		COL_PATH, path,
		COL_IS_DIR, is_dir,
		COL_HIDDEN, hidden,
		COL_SIZE, size,
		COL_MTIME, mtime,
		COL_SIZE_TEXT, size_text,
		COL_DATE_TEXT, date ? date : "",
		COL_TYPE_TEXT, origin ? origin : type_text(w, ct, is_dir),
		-1);
	g_free(date);
	g_free(path);
	g_free(key);
	g_free(size_text);
	g_free(origin);
}

static gboolean
visible_func(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	Win *w = data;
	gboolean hidden, show = TRUE;
	char *name, *fold;

	gtk_tree_model_get(m, it, COL_HIDDEN, &hidden, -1);
	if (hidden && !w->show_hidden)
		return FALSE;
	if (w->search_fold && *w->search_fold) {
		gtk_tree_model_get(m, it, COL_NAME, &name, -1);
		fold = name ? g_utf8_casefold(name, -1) : g_strdup("");
		show = strstr(fold, w->search_fold) != NULL;
		g_free(fold);
		g_free(name);
	}
	return show;
}

/* Folders first (also when sorting backwards), then the chosen column */
static int
compare(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
	Win *w = g_object_get_data(G_OBJECT(m), "hexa-win");
	int col = GPOINTER_TO_INT(data), sort_col, r = 0;
	GtkSortType order = GTK_SORT_ASCENDING;
	gboolean da, db;
	char *ka, *kb;

	gtk_tree_model_get(m, a, COL_IS_DIR, &da, COL_KEY, &ka, -1);
	gtk_tree_model_get(m, b, COL_IS_DIR, &db, COL_KEY, &kb, -1);
	if (da != db) {
		if (w && w->sort)
			gtk_tree_sortable_get_sort_column_id(
				GTK_TREE_SORTABLE(w->sort), &sort_col, &order);
		r = da ? -1 : 1;
		if (order == GTK_SORT_DESCENDING)
			r = -r;
	} else if (col == COL_MTIME || col == COL_SIZE) {
		gint64 ta, tb;
		guint64 sa, sb;

		if (col == COL_MTIME) {
			gtk_tree_model_get(m, a, COL_MTIME, &ta, -1);
			gtk_tree_model_get(m, b, COL_MTIME, &tb, -1);
			r = ta < tb ? -1 : ta > tb;
		} else {
			gtk_tree_model_get(m, a, COL_SIZE, &sa, -1);
			gtk_tree_model_get(m, b, COL_SIZE, &sb, -1);
			r = sa < sb ? -1 : sa > sb;
		}
	} else if (col == COL_TYPE_TEXT) {
		char *ta, *tb;

		gtk_tree_model_get(m, a, COL_TYPE_TEXT, &ta, -1);
		gtk_tree_model_get(m, b, COL_TYPE_TEXT, &tb, -1);
		r = g_utf8_collate(ta ? ta : "", tb ? tb : "");
		g_free(ta);
		g_free(tb);
	}
	if (r == 0 && da == db)
		r = g_strcmp0(ka, kb);
	g_free(ka);
	g_free(kb);
	return r;
}

static GList *
selected_paths(Win *w)
{
	GList *rows, *l, *out = NULL;
	GtkTreeIter it;
	char *path;

	if (!w->sort)
		return NULL;
	if (w->list_view)
		rows = gtk_tree_selection_get_selected_rows(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(w->list)), NULL);
	else
		rows = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(w->icons));
	for (l = rows; l; l = l->next) {
		if (gtk_tree_model_get_iter(w->sort, &it, l->data)) {
			gtk_tree_model_get(w->sort, &it, COL_PATH, &path, -1);
			out = g_list_prepend(out, path);
		}
	}
	g_list_free_full(rows, (GDestroyNotify) gtk_tree_path_free);
	return g_list_reverse(out);
}

static void
select_paths(Win *w, GHashTable *want)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->list));
	GtkTreeIter it;
	gboolean valid, first = TRUE;
	char *path;

	if (!w->sort || g_hash_table_size(want) == 0)
		return;
	for (valid = gtk_tree_model_get_iter_first(w->sort, &it); valid;
	     valid = gtk_tree_model_iter_next(w->sort, &it)) {
		gtk_tree_model_get(w->sort, &it, COL_PATH, &path, -1);
		if (g_hash_table_contains(want, path)) {
			GtkTreePath *tp = gtk_tree_model_get_path(w->sort, &it);

			gtk_icon_view_select_path(GTK_ICON_VIEW(w->icons), tp);
			gtk_tree_selection_select_path(sel, tp);
			if (first) {
				gtk_icon_view_set_cursor(GTK_ICON_VIEW(w->icons),
							 tp, NULL, FALSE);
				gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(w->icons),
							     tp, FALSE, 0, 0);
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(w->list),
							     tp, NULL, FALSE, 0, 0);
				first = FALSE;
			}
			gtk_tree_path_free(tp);
		}
		g_free(path);
	}
}

static void
load_dir(Win *w)
{
	GtkListStore *store;
	GtkTreeModel *filter, *sort, *old_store, *old_filter, *old_sort;
	GFile *dir = g_file_new_for_path(w->cwd);
	GFileEnumerator *en;
	GFileInfo *info;
	GError *e = NULL;
	gboolean trash = in_trash(w);
	int col = COL_NAME, i;
	GtkSortType order = GTK_SORT_ASCENDING;
	static const int sort_cols[] = { COL_NAME, COL_MTIME, COL_TYPE_TEXT, COL_SIZE };

	store = gtk_list_store_new(N_COLS, G_TYPE_ICON, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
		G_TYPE_UINT64, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING);

	g_clear_pointer(&w->error, g_free);
	en = g_file_enumerate_children(dir, ATTRS, G_FILE_QUERY_INFO_NONE, NULL, &e);
	if (en) {
		while ((info = g_file_enumerator_next_file(en, NULL, NULL))) {
			add_info(w, store, info, trash);
			g_object_unref(info);
		}
		g_object_unref(en);
	} else {
		w->error = g_strdup(e->message);
		g_error_free(e);
	}
	g_object_unref(dir);

	/* New models (faster than filling a sorted one); keep the sort order */
	if (w->sort)
		gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(w->sort),
						     &col, &order);
	old_store = GTK_TREE_MODEL(w->store);
	old_filter = w->filter;
	old_sort = w->sort;

	filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);
	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter),
					       visible_func, w, NULL);
	g_object_set_data(G_OBJECT(filter), "hexa-win", w);
	sort = gtk_tree_model_sort_new_with_model(filter);
	w->store = store;
	w->filter = filter;
	w->sort = sort;
	for (i = 0; i < (int) G_N_ELEMENTS(sort_cols); i++)
		gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(sort),
			sort_cols[i], compare, GINT_TO_POINTER(sort_cols[i]), NULL);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(sort), col, order);

	gtk_icon_view_set_model(GTK_ICON_VIEW(w->icons), sort);
	gtk_tree_view_set_model(GTK_TREE_VIEW(w->list), sort);
	if (old_sort) {
		g_object_unref(old_sort);
		g_object_unref(old_filter);
		g_object_unref(old_store);
	}
	update_status(w);
}

static void
refilter(Win *w)
{
	if (w->filter)
		gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(w->filter));
	update_status(w);
}

typedef struct {
	Win *w;
	double icons_y, list_y;
} Scroll;

static gboolean
restore_scroll(gpointer data)
{
	Scroll *s = data;

	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(s->w->icons_scroll)), s->icons_y);
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(s->w->list_scroll)), s->list_y);
	return G_SOURCE_REMOVE;
}

/* Reload after a change in the folder: keeps the selection and scroll */
static gboolean
reload_cb(gpointer data)
{
	Win *w = data;
	GHashTable *want = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, NULL);
	GList *sel = selected_paths(w), *l;
	Scroll *s = g_new0(Scroll, 1);

	w->reload_id = 0;
	for (l = sel; l; l = l->next)
		g_hash_table_add(want, l->data);
	g_list_free(sel);
	if (w->pending_select) {
		g_hash_table_add(want, w->pending_select);
		w->pending_select = NULL;
	}
	s->w = w;
	s->icons_y = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(w->icons_scroll)));
	s->list_y = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(w->list_scroll)));

	load_dir(w);
	select_paths(w, want);
	g_hash_table_unref(want);
	g_idle_add_full(G_PRIORITY_LOW, restore_scroll, s, g_free);
	return G_SOURCE_REMOVE;
}

static void
schedule_reload(Win *w)
{
	if (!w->reload_id)
		w->reload_id = g_timeout_add(200, reload_cb, w);
}

static void
on_dir_changed(GFileMonitor *m, GFile *f, GFile *other,
	       GFileMonitorEvent ev, Win *w)
{
	if (ev != G_FILE_MONITOR_EVENT_CHANGED)	/* wait for CHANGES_DONE */
		schedule_reload(w);
}

static void
watch(Win *w)
{
	GFile *dir = g_file_new_for_path(w->cwd);

	if (w->monitor) {
		g_file_monitor_cancel(w->monitor);
		g_clear_object(&w->monitor);
	}
	w->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES,
					      NULL, NULL);
	if (w->monitor)
		g_signal_connect(w->monitor, "changed",
				 G_CALLBACK(on_dir_changed), w);
	g_object_unref(dir);
}

/*
 * Status bar, title, sidebar highlight
 */

static void
update_status(Win *w)
{
	GString *s = g_string_new(NULL);
	GList *sel, *l;
	int n = w->sort ? gtk_tree_model_iter_n_children(w->sort, NULL) : 0;
	int nsel = 0;
	guint64 bytes = 0;
	gboolean any_file = FALSE;

	if (busy_text)
		g_string_append_printf(s, "%s    ", busy_text);
	if (w->error) {
		g_string_append_printf(s, "Cannot open this folder: %s", w->error);
	} else {
		g_string_append_printf(s, "%d item%s", n, n == 1 ? "" : "s");
		sel = selected_paths(w);
		for (l = sel; l; l = l->next) {
			struct stat st;

			nsel++;
			if (stat(l->data, &st) == 0 && S_ISREG(st.st_mode)) {
				bytes += st.st_size;
				any_file = TRUE;
			}
		}
		g_list_free_full(sel, g_free);
		if (nsel) {
			g_string_append_printf(s, "    %d selected", nsel);
			if (any_file) {
				char *sz = g_format_size(bytes);

				g_string_append_printf(s, "  (%s)", sz);
				g_free(sz);
			}
		}
	}
	gtk_label_set_text(GTK_LABEL(w->status), s->str);
	g_string_free(s, TRUE);
}

static void
update_free_space(Win *w)
{
	GFile *f = g_file_new_for_path(w->cwd);
	GFileInfo *fs = g_file_query_filesystem_info(f,
		G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
		NULL, NULL);
	char *text = NULL;

	if (fs) {
		char *fr = g_format_size(g_file_info_get_attribute_uint64(fs,
			G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
		char *sz = g_format_size(g_file_info_get_attribute_uint64(fs,
			G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));

		text = g_strdup_printf("%s free of %s", fr, sz);
		g_free(fr);
		g_free(sz);
		g_object_unref(fs);
	}
	gtk_label_set_text(GTK_LABEL(w->free_label), text ? text : "");
	g_free(text);
	g_object_unref(f);
}

static void
update_chrome(Win *w)
{
	gboolean trash = in_trash(w);
	char *title, *parent;
	GList *rows, *l;
	GtkListBoxRow *match = NULL;

	if (trash)
		title = g_strdup("Trash");
	else if (strcmp(w->cwd, "/") == 0)
		title = g_strdup("Computer");
	else if (strcmp(w->cwd, g_get_home_dir()) == 0)
		title = g_strdup("Home");
	else
		title = g_path_get_basename(w->cwd);
	gtk_window_set_title(GTK_WINDOW(w->window), title);
	gtk_entry_set_text(GTK_ENTRY(w->address), trash ? "Trash" : w->cwd);
	g_free(title);

	gtk_widget_set_sensitive(w->back_btn, !g_queue_is_empty(&w->back));
	gtk_widget_set_sensitive(w->fwd_btn, !g_queue_is_empty(&w->forward));
	parent = g_path_get_dirname(w->cwd);
	gtk_widget_set_sensitive(w->up_btn, !trash && strcmp(parent, w->cwd) != 0);
	g_free(parent);
	gtk_widget_set_sensitive(w->empty_trash_item, trash);
	gtk_tree_view_column_set_title(w->type_col,
				       trash ? "Original location" : "Type");

	rows = gtk_container_get_children(GTK_CONTAINER(w->sidebar));
	for (l = rows; l; l = l->next) {
		const char *p = g_object_get_data(l->data, "path");

		if (p && strcmp(p, w->cwd) == 0)
			match = l->data;
	}
	g_list_free(rows);
	if (match)
		gtk_list_box_select_row(GTK_LIST_BOX(w->sidebar), match);
	else
		gtk_list_box_unselect_all(GTK_LIST_BOX(w->sidebar));
	update_free_space(w);
}

/*
 * Navigation
 */

static void
go(Win *w, const char *path, gboolean history)
{
	char *canon = g_canonicalize_filename(path, NULL);

	if (strcmp(canon, trash_files()) == 0)
		g_mkdir_with_parents(canon, 0700);
	if (!g_file_test(canon, G_FILE_TEST_IS_DIR)) {
		char *msg = g_strdup_printf("\"%s\" is not a folder that exists.",
					    canon);

		error_box(w, "Cannot open the folder", msg);
		g_free(msg);
		g_free(canon);
		return;
	}
	if (w->cwd && strcmp(w->cwd, canon) == 0) {
		g_free(canon);
		schedule_reload(w);
		return;
	}
	if (history && w->cwd) {
		g_queue_push_head(&w->back, w->cwd);
		/* clear, not free: the queues are part of Win */
		g_queue_clear_full(&w->forward, g_free);
	} else {
		g_free(w->cwd);
	}
	w->cwd = canon;

	g_clear_pointer(&w->search_fold, g_free);
	g_signal_handlers_block_matched(w->search, G_SIGNAL_MATCH_DATA,
					0, 0, NULL, NULL, w);
	gtk_entry_set_text(GTK_ENTRY(w->search), "");
	g_signal_handlers_unblock_matched(w->search, G_SIGNAL_MATCH_DATA,
					  0, 0, NULL, NULL, w);
	if (w->reload_id) {
		g_source_remove(w->reload_id);
		w->reload_id = 0;
	}
	load_dir(w);
	watch(w);
	update_chrome(w);
	gtk_widget_grab_focus(w->list_view ? w->list : w->icons);
}

static void
go_back(Win *w)
{
	char *p = g_queue_pop_head(&w->back);

	if (!p)
		return;
	g_queue_push_head(&w->forward, g_strdup(w->cwd));
	go(w, p, FALSE);
	g_free(p);
}

static void
go_forward(Win *w)
{
	char *p = g_queue_pop_head(&w->forward);

	if (!p)
		return;
	g_queue_push_head(&w->back, g_strdup(w->cwd));
	go(w, p, FALSE);
	g_free(p);
}

static void
go_up(Win *w)
{
	char *parent, *child;
	GHashTable *want;

	if (in_trash(w))
		return;
	parent = g_path_get_dirname(w->cwd);
	child = g_strdup(w->cwd);
	if (strcmp(parent, w->cwd) != 0) {
		go(w, parent, TRUE);
		/* select the folder we came from, like Explorer */
		want = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		g_hash_table_add(want, child);
		child = NULL;
		select_paths(w, want);
		g_hash_table_unref(want);
	}
	g_free(child);
	g_free(parent);
}

/*
 * Opening files
 */

static void
spawn(Win *w, const char *const *argv)
{
	GError *e = NULL;

	if (!g_spawn_async(w->cwd, (char **) argv, NULL, G_SPAWN_SEARCH_PATH,
			   NULL, NULL, NULL, &e)) {
		error_box(w, "Could not start the program", e->message);
		g_error_free(e);
	}
}

static void
launch(Win *w, GAppInfo *ai, const char *path)
{
	GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(
		gtk_widget_get_display(w->window));
	GFile *f = g_file_new_for_path(path);
	GList files = { f, NULL, NULL };
	GError *e = NULL;

	if (!g_app_info_launch(ai, &files, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
		error_box(w, "Could not open the file", e->message);
		g_error_free(e);
	}
	g_object_unref(f);
	g_object_unref(ctx);
}

static void
open_with(Win *w, const char *path)
{
	GFile *f = g_file_new_for_path(path);
	GtkWidget *d = gtk_app_chooser_dialog_new(GTK_WINDOW(w->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, f);
	GtkWidget *chooser = gtk_app_chooser_dialog_get_widget(
		GTK_APP_CHOOSER_DIALOG(d));

	gtk_app_chooser_widget_set_show_all(GTK_APP_CHOOSER_WIDGET(chooser), TRUE);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		GAppInfo *ai = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));

		if (ai) {
			launch(w, ai, path);
			g_object_unref(ai);
		}
	}
	gtk_widget_destroy(d);
	g_object_unref(f);
}

static gboolean
ends_with_ci(const char *name, const char *suffix)
{
	char *low = g_ascii_strdown(name, -1);
	gboolean r = g_str_has_suffix(low, suffix);

	g_free(low);
	return r;
}

/*
 * The default app from the .desktop files when there is one; otherwise
 * Windows programs -> Wine, web/pictures/PDF/media -> Firefox, text -> the
 * Text Editor, anything else -> "Open with".
 */
static void
open_file(Win *w, const char *path)
{
	GFile *f = g_file_new_for_path(path);
	GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
					    G_FILE_QUERY_INFO_NONE, NULL, NULL);
	const char *ct = info ? g_file_info_get_content_type(info) : NULL;
	GAppInfo *ai = ct ? g_app_info_get_default_for_type(ct, FALSE) : NULL;

	if (ai) {
		launch(w, ai, path);
		g_object_unref(ai);
	} else if (ends_with_ci(path, ".exe") || ends_with_ci(path, ".msi") ||
		   ends_with_ci(path, ".bat")) {
		const char *argv[] = { "wine", "start", "/unix", path, NULL };

		spawn(w, argv);
	} else if (ct && (g_str_has_prefix(ct, "image/") ||
			  g_str_has_prefix(ct, "video/") ||
			  g_str_has_prefix(ct, "audio/") ||
			  g_content_type_is_a(ct, "application/pdf") ||
			  g_content_type_is_a(ct, "text/html"))) {
		const char *argv[] = { "firefox", path, NULL };

		spawn(w, argv);
	} else if (ct && (g_content_type_is_a(ct, "text/plain") ||
			  g_str_has_prefix(ct, "text/") ||
			  strcmp(ct, "application/x-zerosize") == 0 ||
			  strcmp(ct, "application/json") == 0 ||
			  strcmp(ct, "application/xml") == 0)) {
		const char *argv[] = { "foot", "--app-id=hexa-editor",
				       "--title=Editor", "nano", path, NULL };

		spawn(w, argv);
	} else {
		open_with(w, path);
	}
	if (info)
		g_object_unref(info);
	g_object_unref(f);
}

static void
activate_tree_path(Win *w, GtkTreePath *tp)
{
	GtkTreeIter it;
	char *path;
	gboolean is_dir;

	if (!gtk_tree_model_get_iter(w->sort, &it, tp))
		return;
	gtk_tree_model_get(w->sort, &it, COL_PATH, &path, COL_IS_DIR, &is_dir, -1);
	if (is_dir)
		go(w, path, TRUE);
	else
		open_file(w, path);
	g_free(path);
}

static void
open_selected(Win *w)
{
	GList *sel = selected_paths(w), *l;

	if (sel && !sel->next && g_file_test(sel->data, G_FILE_TEST_IS_DIR)) {
		go(w, sel->data, TRUE);
	} else {
		for (l = sel; l; l = l->next) {
			if (g_file_test(l->data, G_FILE_TEST_IS_DIR))
				new_window(l->data);
			else
				open_file(w, l->data);
		}
	}
	g_list_free_full(sel, g_free);
}

/*
 * Clipboard
 */

static void
set_clip(Win *w, gboolean cut)
{
	GList *sel = selected_paths(w), *l;
	GString *text;

	if (!sel)
		return;
	if (clip)
		g_ptr_array_unref(clip);
	clip = g_ptr_array_new_with_free_func(g_free);
	text = g_string_new(NULL);
	for (l = sel; l; l = l->next) {
		if (text->len)
			g_string_append_c(text, '\n');
		g_string_append(text, l->data);
		g_ptr_array_add(clip, l->data);	/* takes the string */
	}
	g_list_free(sel);
	clip_cut = cut;
	g_free(clip_text);
	clip_text = g_string_free(text, FALSE);
	/* other apps (the terminal, editors) get the paths as text */
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
			       clip_text, -1);
}

/*
 * What to paste: our own copy/cut, or paths/file:// links another app put
 * on the clipboard (e.g. copied from the terminal).
 */
static GPtrArray *
clipboard_paths(gboolean *cut)
{
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	char *text = gtk_clipboard_wait_for_text(cb);
	GPtrArray *out = NULL;
	char **lines;
	int i;

	*cut = FALSE;
	if (clip && (!text || g_strcmp0(text, clip_text) == 0)) {
		out = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; i < (int) clip->len; i++)
			g_ptr_array_add(out, g_strdup(clip->pdata[i]));
		*cut = clip_cut;
		g_free(text);
		return out;
	}
	if (!text)
		return NULL;
	lines = g_strsplit(text, "\n", -1);
	out = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; lines[i]; i++) {
		char *line = g_strstrip(lines[i]), *path;

		if (!*line)
			continue;
		path = g_str_has_prefix(line, "file://")
			? g_filename_from_uri(line, NULL, NULL) : g_strdup(line);
		if (!path || !g_path_is_absolute(path) ||
		    !g_file_test(path, G_FILE_TEST_EXISTS)) {
			g_free(path);
			g_clear_pointer(&out, g_ptr_array_unref);
			break;
		}
		g_ptr_array_add(out, path);
	}
	g_strfreev(lines);
	g_free(text);
	if (out && out->len == 0)
		g_clear_pointer(&out, g_ptr_array_unref);
	return out;
}

static void
paste_into(Win *w, const char *dest)
{
	gboolean cut;
	GPtrArray *src;

	if (in_trash(w) && strcmp(dest, trash_files()) == 0)
		return;
	src = clipboard_paths(&cut);
	if (!src) {
		gtk_label_set_text(GTK_LABEL(w->status),
				   "Nothing to paste: copy or cut some files first.");
		return;
	}
	run_op(cut ? OP_MOVE : OP_COPY, src, dest, cut);
}

/*
 * Trash and delete
 */

static void
delete_permanently(Win *w, GList *paths)
{
	GPtrArray *src;
	GList *l;
	char *q, *base;
	int n = g_list_length(paths);

	if (!paths)
		return;
	base = g_path_get_basename(paths->data);
	if (n == 1)
		q = g_strdup_printf("Permanently delete \"%s\"?", base);
	else
		q = g_strdup_printf("Permanently delete these %d items?", n);
	g_free(base);
	if (confirm(w, q, "They will not go to the Trash and cannot be recovered.",
		    "_Delete")) {
		src = g_ptr_array_new_with_free_func(g_free);
		for (l = paths; l; l = l->next)
			g_ptr_array_add(src, g_strdup(l->data));
		run_op(OP_DELETE, src, NULL, FALSE);
	}
	g_free(q);
}

static void
trash_selected(Win *w)
{
	GList *sel = selected_paths(w), *l, *failed = NULL;
	GError *e = NULL;
	char *why = NULL;

	if (in_trash(w)) {
		delete_permanently(w, sel);
		g_list_free_full(sel, g_free);
		return;
	}
	for (l = sel; l; l = l->next) {
		GFile *f = g_file_new_for_path(l->data);

		if (!g_file_trash(f, NULL, &e)) {
			if (!why)
				why = g_strdup(e->message);
			failed = g_list_append(failed, g_strdup(l->data));
			g_clear_error(&e);
		}
		g_object_unref(f);
	}
	if (failed) {
		/* e.g. USB drives without a Trash folder */
		char *q = g_strdup_printf(
			"%s\n\nDelete permanently instead?", why);

		if (confirm(w, "Some items cannot go to the Trash", q, "_Delete")) {
			GPtrArray *src = g_ptr_array_new_with_free_func(g_free);

			for (l = failed; l; l = l->next)
				g_ptr_array_add(src, g_strdup(l->data));
			run_op(OP_DELETE, src, NULL, FALSE);
		}
		g_free(q);
	}
	g_free(why);
	g_list_free_full(failed, g_free);
	g_list_free_full(sel, g_free);
}

static void
restore_selected(Win *w)
{
	GList *sel = selected_paths(w), *l;
	GError *e = NULL;

	for (l = sel; l; l = l->next) {
		char *name = g_path_get_basename(l->data);
		char *orig = trash_origin(name), *parent, *base, *target;
		GFile *s, *d;

		if (!orig) {
			error_box(w, "Cannot restore", "Its original location is unknown.");
			g_free(name);
			continue;
		}
		parent = g_path_get_dirname(orig);
		base = g_path_get_basename(orig);
		g_mkdir_with_parents(parent, 0755);
		target = unique_target(parent, base);
		s = g_file_new_for_path(l->data);
		d = g_file_new_for_path(target);
		if (move_file(s, d, &e)) {
			char *info = trash_info_path(name);

			g_unlink(info);
			g_free(info);
		} else {
			error_box(w, "Cannot restore", e->message);
			g_clear_error(&e);
		}
		g_object_unref(s);
		g_object_unref(d);
		g_free(target);
		g_free(base);
		g_free(parent);
		g_free(orig);
		g_free(name);
	}
	g_list_free_full(sel, g_free);
}

static void
empty_trash(Win *w)
{
	GPtrArray *src = g_ptr_array_new_with_free_func(g_free);
	GDir *dir = g_dir_open(trash_files(), 0, NULL);
	const char *name;

	if (dir) {
		while ((name = g_dir_read_name(dir)))
			g_ptr_array_add(src, g_build_filename(trash_files(), name, NULL));
		g_dir_close(dir);
	}
	if (src->len == 0) {
		g_ptr_array_unref(src);
		message(GTK_WINDOW(w->window), GTK_MESSAGE_INFO,
			"The Trash is already empty.", NULL);
		return;
	}
	if (confirm(w, "Empty the Trash?",
		    "Everything in the Trash will be deleted permanently.",
		    "_Empty Trash"))
		run_op(OP_DELETE, src, NULL, FALSE);
	else
		g_ptr_array_unref(src);
}

/*
 * Names: rename, new folder, new document
 */

static char *
ask_name(Win *w, const char *title, const char *ok, const char *initial,
	 gboolean select_stem)
{
	GtkWidget *d, *entry, *box;
	char *result = NULL;
	const char *dot = strrchr(initial, '.');
	int end = -1;

	d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Cancel", GTK_RESPONSE_CANCEL, ok, GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), initial);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_container_add(GTK_CONTAINER(box), entry);
	gtk_widget_show_all(d);

	/* Select the name without its extension, like Explorer */
	gtk_widget_grab_focus(entry);
	if (select_stem && dot && dot != initial)
		end = g_utf8_pointer_to_offset(initial, dot);
	gtk_editable_select_region(GTK_EDITABLE(entry), 0, end);

	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
		char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));

		if (!*name || strchr(name, '/') || !strcmp(name, ".") ||
		    !strcmp(name, "..")) {
			message(GTK_WINDOW(d), GTK_MESSAGE_ERROR,
				"That name cannot be used.",
				"Names cannot be empty or contain \"/\".");
			g_free(name);
			continue;
		}
		result = name;
		break;
	}
	gtk_widget_destroy(d);
	return result;
}

static void
rename_selected(Win *w)
{
	GList *sel = selected_paths(w);
	char *base, *name;
	GFile *f, *renamed;
	GError *e = NULL;

	if (!sel || sel->next || in_trash(w)) {
		g_list_free_full(sel, g_free);
		return;
	}
	base = g_path_get_basename(sel->data);
	name = ask_name(w, "Rename", "_Rename", base,
			!g_file_test(sel->data, G_FILE_TEST_IS_DIR));
	if (name && strcmp(name, base) != 0) {
		char *target = g_build_filename(w->cwd, name, NULL);

		if (g_file_test(target, G_FILE_TEST_EXISTS)) {
			error_box(w, "Cannot rename",
				  "There is already an item with that name here.");
		} else {
			f = g_file_new_for_path(sel->data);
			renamed = g_file_set_display_name(f, name, NULL, &e);
			if (renamed) {
				g_free(w->pending_select);
				w->pending_select = g_file_get_path(renamed);
				g_object_unref(renamed);
			} else {
				error_box(w, "Cannot rename", e->message);
				g_error_free(e);
			}
			g_object_unref(f);
		}
		g_free(target);
	}
	g_free(name);
	g_free(base);
	g_list_free_full(sel, g_free);
}

static void
create_item(Win *w, gboolean folder)
{
	char *suggest_path = unique_target(w->cwd,
		folder ? "New folder" : "New text document.txt");
	char *suggest = g_path_get_basename(suggest_path);
	char *name = ask_name(w, folder ? "New Folder" : "New Text Document",
			      "C_reate", suggest, !folder);
	GError *e = NULL;

	if (name) {
		char *path = g_build_filename(w->cwd, name, NULL);
		GFile *f = g_file_new_for_path(path);
		gboolean ok;

		if (folder) {
			ok = g_file_make_directory(f, NULL, &e);
		} else {
			GFileOutputStream *out = g_file_create(f, G_FILE_CREATE_NONE,
							       NULL, &e);

			ok = out != NULL;
			if (out) {
				g_output_stream_close(G_OUTPUT_STREAM(out), NULL, NULL);
				g_object_unref(out);
			}
		}
		if (ok) {
			g_free(w->pending_select);
			w->pending_select = g_strdup(path);
			schedule_reload(w);
		} else {
			error_box(w, folder ? "Cannot create the folder"
					    : "Cannot create the document", e->message);
			g_error_free(e);
		}
		g_object_unref(f);
		g_free(path);
	}
	g_free(name);
	g_free(suggest);
	g_free(suggest_path);
}

/*
 * Properties
 */

typedef struct {
	GPtrArray *paths;
	guint64 bytes, files, dirs;
} Count;

static void
count_tree(GFile *f, Count *c)
{
	GFileInfo *info = g_file_query_info(f, "standard::type,standard::size",
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	GFileEnumerator *en;
	GFileInfo *child;

	if (!info)
		return;
	if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
		c->dirs++;
		en = g_file_enumerate_children(f, "standard::name",
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
		if (en) {
			while ((child = g_file_enumerator_next_file(en, NULL, NULL))) {
				GFile *cf = g_file_get_child(f,
					g_file_info_get_name(child));

				count_tree(cf, c);
				g_object_unref(cf);
				g_object_unref(child);
			}
			g_object_unref(en);
		}
	} else {
		c->files++;
		if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
			c->bytes += g_file_info_get_size(info);
	}
	g_object_unref(info);
}

static void
count_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	Count *c = data;
	guint i;

	for (i = 0; i < c->paths->len; i++) {
		GFile *f = g_file_new_for_path(c->paths->pdata[i]);

		count_tree(f, c);
		g_object_unref(f);
	}
	g_task_return_boolean(task, TRUE);
}

static void
count_free(Count *c)
{
	g_ptr_array_unref(c->paths);
	g_free(c);
}

static void
count_done(GObject *label, GAsyncResult *res, gpointer data)
{
	Count *c = g_task_get_task_data(G_TASK(res));
	GtkWidget *contains = data;
	char *sz, *text;

	if (!g_object_get_data(label, "gone")) {
		sz = g_format_size_full(c->bytes, G_FORMAT_SIZE_LONG_FORMAT);
		gtk_label_set_text(GTK_LABEL(label), sz);
		g_free(sz);
	}
	if (contains && !g_object_get_data(G_OBJECT(contains), "gone")) {
		/* the folders themselves are not counted */
		guint64 dirs = c->dirs >= c->paths->len ? c->dirs - c->paths->len : 0;

		text = g_strdup_printf("%" G_GUINT64_FORMAT " files, %"
				       G_GUINT64_FORMAT " folders", c->files, dirs);
		gtk_label_set_text(GTK_LABEL(contains), text);
		g_free(text);
	}
	if (contains)
		g_object_unref(contains);
}

static void
mark_gone(GtkWidget *wd)
{
	g_object_set_data(G_OBJECT(wd), "gone", GINT_TO_POINTER(1));
}

static GtkWidget *
prop_row(GtkWidget *grid, int *row, const char *name, const char *value)
{
	GtkWidget *k = gtk_label_new(name), *v = gtk_label_new(value);

	gtk_widget_set_halign(k, GTK_ALIGN_END);
	gtk_widget_set_valign(k, GTK_ALIGN_START);
	gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
	gtk_widget_set_halign(v, GTK_ALIGN_START);
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 50);
	gtk_label_set_xalign(GTK_LABEL(v), 0);
	g_signal_connect(v, "destroy", G_CALLBACK(mark_gone), NULL);
	gtk_grid_attach(GTK_GRID(grid), k, 0, *row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v, 1, *row, 1, 1);
	(*row)++;
	return v;
}

static char *
perm_text(guint32 mode)
{
	static const char *rwx[] = { "---", "--x", "-w-", "-wx", "r--", "r-x",
				     "rw-", "rwx" };

	return g_strdup_printf("%s%s%s  (%03o)", rwx[(mode >> 6) & 7],
			       rwx[(mode >> 3) & 7], rwx[mode & 7], mode & 0777);
}

static void
show_properties(Win *w)
{
	GList *sel = selected_paths(w), *l;
	GtkWidget *d, *grid, *size_label, *contains = NULL;
	GTask *task;
	Count *c;
	char *title, *text;
	int row = 0, n;
	gboolean any_dir = FALSE;

	if (!sel)			/* nothing selected: the open folder */
		sel = g_list_prepend(NULL, g_strdup(w->cwd));
	n = g_list_length(sel);
	for (l = sel; l; l = l->next)
		if (g_file_test(l->data, G_FILE_TEST_IS_DIR) &&
		    !g_file_test(l->data, G_FILE_TEST_IS_SYMLINK))
			any_dir = TRUE;

	if (n == 1) {
		char *base = g_path_get_basename(sel->data);

		title = g_strdup_printf("%s Properties", base);
		g_free(base);
	} else {
		title = g_strdup_printf("%d items Properties", n);
	}
	d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window),
		GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
	g_free(title);
	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 18);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			  grid);

	if (n == 1) {
		const char *path = sel->data;
		GFile *f = g_file_new_for_path(path);
		GFileInfo *info = g_file_query_info(f,
			"standard::*,time::modified,unix::mode,owner::user,"
			"standard::symlink-target",
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
		char *base = g_path_get_basename(path), *dir = g_path_get_dirname(path);

		prop_row(grid, &row, "Name", base);
		if (info) {
			const char *ct = g_file_info_get_content_type(info);
			const char *target = g_file_info_get_symlink_target(info);
			GDateTime *dt = g_file_info_get_modification_date_time(info);

			if (target) {
				prop_row(grid, &row, "Type", "Link");
				prop_row(grid, &row, "Points to", target);
			} else {
				char *desc = any_dir ? g_strdup("File folder")
					: (ct ? g_content_type_get_description(ct)
					      : g_strdup("File"));

				prop_row(grid, &row, "Type", desc);
				g_free(desc);
			}
			prop_row(grid, &row, "Location", dir);
			size_label = prop_row(grid, &row, "Size",
					      any_dir ? "Calculating..." : "");
			if (any_dir)
				contains = prop_row(grid, &row, "Contains",
						    "Calculating...");
			if (dt) {
				GDateTime *local = g_date_time_to_local(dt);

				text = g_date_time_format(local, "%d/%m/%Y %H:%M:%S");
				prop_row(grid, &row, "Modified", text);
				g_free(text);
				g_date_time_unref(local);
				g_date_time_unref(dt);
			}
			if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
				text = perm_text(g_file_info_get_attribute_uint32(info,
					G_FILE_ATTRIBUTE_UNIX_MODE));
				prop_row(grid, &row, "Permissions", text);
				g_free(text);
			}
			if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_OWNER_USER))
				prop_row(grid, &row, "Owner",
					 g_file_info_get_attribute_string(info,
						G_FILE_ATTRIBUTE_OWNER_USER));
			g_object_unref(info);
		} else {
			prop_row(grid, &row, "Location", dir);
			size_label = prop_row(grid, &row, "Size", "");
		}
		g_free(base);
		g_free(dir);
		g_object_unref(f);
	} else {
		char *dir = g_path_get_dirname(sel->data);

		text = g_strdup_printf("%d items", n);
		prop_row(grid, &row, "Selected", text);
		g_free(text);
		prop_row(grid, &row, "Location", dir);
		g_free(dir);
		size_label = prop_row(grid, &row, "Size", "Calculating...");
		contains = prop_row(grid, &row, "Contains", "Calculating...");
	}
	{
		GFile *f = g_file_new_for_path(sel->data);
		GFileInfo *fs = g_file_query_filesystem_info(f,
			G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
			NULL, NULL);

		if (fs) {
			char *fr = g_format_size(g_file_info_get_attribute_uint64(fs,
				G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
			char *sz = g_format_size(g_file_info_get_attribute_uint64(fs,
				G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));

			text = g_strdup_printf("%s free of %s", fr, sz);
			prop_row(grid, &row, "Disk", text);
			g_free(text);
			g_free(fr);
			g_free(sz);
			g_object_unref(fs);
		}
		g_object_unref(f);
	}

	/* Sizes (folders: everything inside) are counted in the background */
	c = g_new0(Count, 1);
	c->paths = g_ptr_array_new_with_free_func(g_free);
	for (l = sel; l; l = l->next)
		g_ptr_array_add(c->paths, g_strdup(l->data));
	if (!any_dir)
		c->dirs = 0;
	task = g_task_new(size_label, NULL, count_done,
			  contains ? g_object_ref(contains) : NULL);
	g_task_set_task_data(task, c, (GDestroyNotify) count_free);
	g_task_run_in_thread(task, count_thread);
	g_object_unref(task);

	gtk_widget_show_all(d);
	g_list_free_full(sel, g_free);
}

/*
 * Menus
 */

static void
open_terminal(Win *w)
{
	char *arg = g_strdup_printf("--working-directory=%s", w->cwd);
	const char *argv[] = { "foot", arg, NULL };

	spawn(w, argv);
	g_free(arg);
}

static void on_open(GtkMenuItem *i, Win *w) { open_selected(w); }
static void on_cut(GtkMenuItem *i, Win *w) { set_clip(w, TRUE); }
static void on_copy(GtkMenuItem *i, Win *w) { set_clip(w, FALSE); }
static void on_paste(GtkMenuItem *i, Win *w) { paste_into(w, w->cwd); }
static void on_rename(GtkMenuItem *i, Win *w) { rename_selected(w); }
static void on_trash(GtkMenuItem *i, Win *w) { trash_selected(w); }
static void on_restore(GtkMenuItem *i, Win *w) { restore_selected(w); }
static void on_props(GtkMenuItem *i, Win *w) { show_properties(w); }
static void on_new_folder(GtkMenuItem *i, Win *w) { create_item(w, TRUE); }
static void on_new_doc(GtkMenuItem *i, Win *w) { create_item(w, FALSE); }
static void on_terminal(GtkMenuItem *i, Win *w) { open_terminal(w); }
static void on_empty_trash(GtkMenuItem *i, Win *w) { empty_trash(w); }
static void on_new_window(GtkMenuItem *i, Win *w) { new_window(w->cwd); }

static void
on_delete(GtkMenuItem *i, Win *w)
{
	GList *sel = selected_paths(w);

	delete_permanently(w, sel);
	g_list_free_full(sel, g_free);
}

static void
on_paste_into(GtkMenuItem *i, Win *w)
{
	GList *sel = selected_paths(w);

	if (sel && g_file_test(sel->data, G_FILE_TEST_IS_DIR))
		paste_into(w, sel->data);
	g_list_free_full(sel, g_free);
}

static void
on_open_with(GtkMenuItem *i, Win *w)
{
	GList *sel = selected_paths(w);

	if (sel)
		open_with(w, sel->data);
	g_list_free_full(sel, g_free);
}

static void
on_terminal_here(GtkMenuItem *i, Win *w)
{
	GList *sel = selected_paths(w);

	if (sel && g_file_test(sel->data, G_FILE_TEST_IS_DIR)) {
		char *arg = g_strdup_printf("--working-directory=%s",
					    (char *) sel->data);
		const char *argv[] = { "foot", arg, NULL };

		spawn(w, argv);
		g_free(arg);
	}
	g_list_free_full(sel, g_free);
}

static GtkWidget *
add_item(GtkWidget *menu, const char *label, const char *accel,
	 GCallback cb, Win *w)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);

	if (accel) {
		guint key;
		GdkModifierType mods;
		GtkWidget *child = gtk_bin_get_child(GTK_BIN(item));

		gtk_accelerator_parse(accel, &key, &mods);
		gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key, mods);
	}
	if (cb)
		g_signal_connect(item, "activate", cb, w);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

static void
add_separator(GtkWidget *menu)
{
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void
popup_menu(Win *w, const GdkEvent *ev)
{
	GtkWidget *menu = gtk_menu_new();
	GList *sel = selected_paths(w);
	int n = g_list_length(sel);
	gboolean one_dir = n == 1 && g_file_test(sel->data, G_FILE_TEST_IS_DIR);

	if (in_trash(w)) {
		if (n) {
			add_item(menu, "_Restore", NULL, G_CALLBACK(on_restore), w);
			add_item(menu, "_Delete Permanently", "Delete",
				 G_CALLBACK(on_delete), w);
			add_separator(menu);
			add_item(menu, "_Properties", "<Alt>Return",
				 G_CALLBACK(on_props), w);
		} else {
			add_item(menu, "_Empty Trash", NULL,
				 G_CALLBACK(on_empty_trash), w);
		}
	} else if (n) {
		add_item(menu, "_Open", "Return", G_CALLBACK(on_open), w);
		if (n == 1 && !one_dir)
			add_item(menu, "Open _With...", NULL,
				 G_CALLBACK(on_open_with), w);
		if (one_dir) {
			add_item(menu, "Open in New _Window", NULL,
				 G_CALLBACK(on_open), w);
			add_item(menu, "Open in _Terminal", NULL,
				 G_CALLBACK(on_terminal_here), w);
		}
		add_separator(menu);
		add_item(menu, "Cu_t", "<Control>x", G_CALLBACK(on_cut), w);
		add_item(menu, "_Copy", "<Control>c", G_CALLBACK(on_copy), w);
		if (one_dir)
			add_item(menu, "_Paste Into Folder", NULL,
				 G_CALLBACK(on_paste_into), w);
		add_separator(menu);
		if (n == 1)
			add_item(menu, "Re_name...", "F2", G_CALLBACK(on_rename), w);
		add_item(menu, "Move to Tr_ash", "Delete", G_CALLBACK(on_trash), w);
		add_item(menu, "_Delete Permanently", "<Shift>Delete",
			 G_CALLBACK(on_delete), w);
		add_separator(menu);
		add_item(menu, "P_roperties", "<Alt>Return", G_CALLBACK(on_props), w);
	} else {
		add_item(menu, "New _Folder...", "<Control><Shift>n",
			 G_CALLBACK(on_new_folder), w);
		add_item(menu, "New _Text Document...", NULL,
			 G_CALLBACK(on_new_doc), w);
		add_separator(menu);
		add_item(menu, "_Paste", "<Control>v", G_CALLBACK(on_paste), w);
		add_separator(menu);
		add_item(menu, "Open in _Terminal", NULL, G_CALLBACK(on_terminal), w);
		add_item(menu, "P_roperties", "<Alt>Return", G_CALLBACK(on_props), w);
	}
	g_list_free_full(sel, g_free);

	gtk_widget_show_all(menu);
	/* the menu is destroyed when it closes (after the item ran) */
	g_signal_connect(menu, "deactivate",
			 G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_menu_attach_to_widget(GTK_MENU(menu), w->window, NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

/*
 * View events
 */

static gboolean
nav_button(Win *w, GdkEventButton *ev)
{
	if (ev->button == 8) {		/* mouse "back" side button */
		go_back(w);
		return TRUE;
	}
	if (ev->button == 9) {
		go_forward(w);
		return TRUE;
	}
	return FALSE;
}

static gboolean
on_icons_button(GtkWidget *v, GdkEventButton *ev, Win *w)
{
	GtkTreePath *tp;

	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (nav_button(w, ev))
		return TRUE;
	if (ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	gtk_widget_grab_focus(v);
	tp = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(v), ev->x, ev->y);
	if (tp) {
		if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(v), tp)) {
			gtk_icon_view_unselect_all(GTK_ICON_VIEW(v));
			gtk_icon_view_select_path(GTK_ICON_VIEW(v), tp);
		}
		gtk_icon_view_set_cursor(GTK_ICON_VIEW(v), tp, NULL, FALSE);
		gtk_tree_path_free(tp);
	} else {
		gtk_icon_view_unselect_all(GTK_ICON_VIEW(v));
	}
	popup_menu(w, (GdkEvent *) ev);
	return TRUE;
}

static gboolean
on_list_button(GtkWidget *v, GdkEventButton *ev, Win *w)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(v));
	GtkTreePath *tp = NULL;

	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (nav_button(w, ev))
		return TRUE;
	if (ev->window != gtk_tree_view_get_bin_window(GTK_TREE_VIEW(v)))
		return FALSE;		/* column headers */
	gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(v), ev->x, ev->y,
				      &tp, NULL, NULL, NULL);
	if (ev->button == GDK_BUTTON_PRIMARY) {
		if (!tp)		/* click on empty space: clear selection */
			gtk_tree_selection_unselect_all(sel);
		gtk_tree_path_free(tp);
		return FALSE;
	}
	if (ev->button != GDK_BUTTON_SECONDARY) {
		gtk_tree_path_free(tp);
		return FALSE;
	}
	gtk_widget_grab_focus(v);
	if (tp) {
		if (!gtk_tree_selection_path_is_selected(sel, tp)) {
			gtk_tree_selection_unselect_all(sel);
			gtk_tree_selection_select_path(sel, tp);
		}
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(v), tp, NULL, FALSE);
		gtk_tree_path_free(tp);
	} else {
		gtk_tree_selection_unselect_all(sel);
	}
	popup_menu(w, (GdkEvent *) ev);
	return TRUE;
}

static gboolean
on_view_popup(GtkWidget *v, Win *w)	/* Menu key / Shift+F10 */
{
	popup_menu(w, NULL);
	return TRUE;
}

static void
on_item_activated(GtkIconView *v, GtkTreePath *tp, Win *w)
{
	activate_tree_path(w, tp);
}

static void
on_row_activated(GtkTreeView *v, GtkTreePath *tp, GtkTreeViewColumn *c, Win *w)
{
	activate_tree_path(w, tp);
}

static void
on_selection_changed(gpointer obj, Win *w)
{
	update_status(w);
}

static void
set_list_view(Win *w, gboolean list)
{
	GHashTable *want = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, NULL);
	GList *sel = selected_paths(w), *l;

	for (l = sel; l; l = l->next)
		g_hash_table_add(want, l->data);
	g_list_free(sel);

	w->list_view = list;
	gtk_stack_set_visible_child_name(GTK_STACK(w->stack),
					 list ? "list" : "icons");
	gtk_button_set_image(GTK_BUTTON(w->view_btn), gtk_image_new_from_icon_name(
		list ? "view-grid-symbolic" : "view-list-symbolic",
		GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(w->view_btn,
		list ? "Large icons (Ctrl+1)" : "Details (Ctrl+2)");
	gtk_icon_view_unselect_all(GTK_ICON_VIEW(w->icons));
	gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(
		GTK_TREE_VIEW(w->list)));
	select_paths(w, want);
	g_hash_table_unref(want);
	gtk_widget_grab_focus(list ? w->list : w->icons);
	update_status(w);
}

static void
select_all(Win *w)
{
	if (w->list_view)
		gtk_tree_selection_select_all(gtk_tree_view_get_selection(
			GTK_TREE_VIEW(w->list)));
	else
		gtk_icon_view_select_all(GTK_ICON_VIEW(w->icons));
}

/*
 * Toolbar and keyboard
 */

static void on_back(GtkButton *b, Win *w) { go_back(w); }
static void on_forward(GtkButton *b, Win *w) { go_forward(w); }
static void on_up(GtkButton *b, Win *w) { go_up(w); }
static void on_view_toggle(GtkButton *b, Win *w) { set_list_view(w, !w->list_view); }

static void
on_address(GtkEntry *e, Win *w)
{
	const char *t = gtk_entry_get_text(e);
	char *path;

	while (*t == ' ')
		t++;
	if (!*t)
		return;
	if (g_ascii_strcasecmp(t, "trash") == 0)
		path = g_strdup(trash_files());
	else if (t[0] == '~')
		path = g_build_filename(g_get_home_dir(), t + 1, NULL);
	else if (g_str_has_prefix(t, "file://"))
		path = g_filename_from_uri(t, NULL, NULL);
	else if (!g_path_is_absolute(t))
		path = g_build_filename(w->cwd, t, NULL);
	else
		path = g_strdup(t);

	if (path && g_file_test(path, G_FILE_TEST_IS_DIR)) {
		go(w, path, TRUE);
	} else if (path && g_file_test(path, G_FILE_TEST_EXISTS)) {
		open_file(w, path);
		update_chrome(w);
	} else {
		char *msg = g_strdup_printf("\"%s\" was not found.", t);

		error_box(w, "Cannot find the location", msg);
		g_free(msg);
		update_chrome(w);
	}
	g_free(path);
}

static void
on_search_changed(GtkSearchEntry *e, Win *w)
{
	g_free(w->search_fold);
	w->search_fold = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(e)), -1);
	refilter(w);
}

static void
on_hidden_toggled(GtkCheckMenuItem *item, Win *w)
{
	w->show_hidden = gtk_check_menu_item_get_active(item);
	refilter(w);
}

static void
on_sidebar_activated(GtkListBox *box, GtkListBoxRow *row, Win *w)
{
	const char *path = g_object_get_data(G_OBJECT(row), "path");

	if (path)
		go(w, path, TRUE);
}

static gboolean
on_key(GtkWidget *win, GdkEventKey *ev, Win *w)
{
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win));
	gboolean typing = GTK_IS_ENTRY(focus);
	gboolean ctrl = mods == GDK_CONTROL_MASK, alt = mods == GDK_MOD1_MASK;
	guint key = gdk_keyval_to_lower(ev->keyval);

	/* everywhere */
	if (alt && key == GDK_KEY_Left) { go_back(w); return TRUE; }
	if (alt && key == GDK_KEY_Right) { go_forward(w); return TRUE; }
	if (alt && key == GDK_KEY_Up) { go_up(w); return TRUE; }
	if (mods == 0 && key == GDK_KEY_F5) { schedule_reload(w); return TRUE; }
	if (ctrl && key == GDK_KEY_l) {
		gtk_widget_grab_focus(w->address);
		return TRUE;
	}
	if (ctrl && key == GDK_KEY_f) {
		gtk_widget_grab_focus(w->search);
		return TRUE;
	}
	if (ctrl && key == GDK_KEY_h) {
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w->hidden_item),
					       !w->show_hidden);
		return TRUE;
	}
	if (ctrl && key == GDK_KEY_1) { set_list_view(w, FALSE); return TRUE; }
	if (ctrl && key == GDK_KEY_2) { set_list_view(w, TRUE); return TRUE; }
	if (ctrl && key == GDK_KEY_n) { new_window(w->cwd); return TRUE; }
	if (ctrl && key == GDK_KEY_w) { gtk_widget_destroy(w->window); return TRUE; }
	if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && key == GDK_KEY_n) {
		create_item(w, TRUE);
		return TRUE;
	}
	if (key == GDK_KEY_Escape && typing) {
		update_chrome(w);	/* undo address edits */
		gtk_widget_grab_focus(w->list_view ? w->list : w->icons);
		return focus == w->address;
	}
	if (typing)
		return FALSE;		/* let the entries edit text */

	/* in the file views */
	if (mods == 0 && key == GDK_KEY_BackSpace) { go_back(w); return TRUE; }
	if (mods == 0 && (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete)) {
		trash_selected(w);
		return TRUE;
	}
	if (mods == GDK_SHIFT_MASK && (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete)) {
		on_delete(NULL, w);
		return TRUE;
	}
	if (mods == 0 && key == GDK_KEY_F2) { rename_selected(w); return TRUE; }
	if (ctrl && key == GDK_KEY_c) { set_clip(w, FALSE); return TRUE; }
	if (ctrl && key == GDK_KEY_x) { set_clip(w, TRUE); return TRUE; }
	if (ctrl && key == GDK_KEY_v) { paste_into(w, w->cwd); return TRUE; }
	if (ctrl && key == GDK_KEY_a) { select_all(w); return TRUE; }
	if (alt && (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter)) {
		show_properties(w);
		return TRUE;
	}
	if (mods == 0 && (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter)) {
		open_selected(w);
		return TRUE;
	}
	return FALSE;
}

/*
 * Sidebar
 */

static GtkWidget *
sidebar_row(const char *icon, const char *label, const char *path)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *lbl = gtk_label_new(label);

	gtk_container_add(GTK_CONTAINER(box),
			  gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU));
	gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0);
	gtk_container_add(GTK_CONTAINER(box), lbl);
	gtk_container_add(GTK_CONTAINER(row), box);
	gtk_widget_set_tooltip_text(row, path);
	g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
	return row;
}

static GtkWidget *
sidebar_header(const char *text)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *lbl = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(lbl), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "hf-section");
	gtk_container_add(GTK_CONTAINER(row), lbl);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
	return row;
}

static void
add_place(Win *w, GUserDirectory which, const char *fallback,
	  const char *icon, const char *label)
{
	const char *dir = g_get_user_special_dir(which);
	char *path = NULL;

	if (dir && strcmp(dir, g_get_home_dir()) != 0)
		path = g_strdup(dir);
	else
		path = g_build_filename(g_get_home_dir(), fallback, NULL);
	if (g_file_test(path, G_FILE_TEST_IS_DIR))
		gtk_container_add(GTK_CONTAINER(w->sidebar),
				  sidebar_row(icon, label, path));
	g_free(path);
}

/* /proc/self/mounts escapes spaces and such as \040 */
static char *
unescape_mount(const char *s)
{
	GString *out = g_string_new(NULL);

	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] && s[3]) {
			g_string_append_c(out, (char) (((s[1] - '0') << 6) |
						       ((s[2] - '0') << 3) | (s[3] - '0')));
			s += 4;
		} else {
			g_string_append_c(out, *s++);
		}
	}
	return g_string_free(out, FALSE);
}

/* Mounted drives: anything mounted under /media, /mnt or /run/media */
static char *
read_drives(void)
{
	char *data = NULL, **lines;
	GString *out = g_string_new(NULL);
	int i;

	if (!g_file_get_contents("/proc/self/mounts", &data, NULL, NULL))
		return g_string_free(out, FALSE);
	lines = g_strsplit(data, "\n", -1);
	for (i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], " ", 4);

		if (g_strv_length(f) >= 3 &&
		    (g_str_has_prefix(f[1], "/media/") ||
		     g_str_has_prefix(f[1], "/mnt/") ||
		     g_str_has_prefix(f[1], "/run/media/"))) {
			char *mp = unescape_mount(f[1]);

			/* not files mounted on their own */
			if (g_file_test(mp, G_FILE_TEST_IS_DIR))
				g_string_append_printf(out, "%s\n", mp);
			g_free(mp);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(data);
	return g_string_free(out, FALSE);
}

static void
fill_drives(Win *w, const char *drives)
{
	GList *rows = gtk_container_get_children(GTK_CONTAINER(w->sidebar)), *l;
	char **mps = g_strsplit(drives, "\n", -1);
	int i;

	for (l = rows; l; l = l->next)
		if (g_object_get_data(l->data, "drive"))
			gtk_widget_destroy(l->data);
	g_list_free(rows);

	for (i = 0; mps[i]; i++) {
		GtkWidget *row;
		char *name;

		if (!*mps[i])
			continue;
		name = g_path_get_basename(mps[i]);
		row = sidebar_row("drive-removable-media-symbolic", name, mps[i]);
		g_object_set_data(G_OBJECT(row), "drive", GINT_TO_POINTER(1));
		gtk_container_add(GTK_CONTAINER(w->sidebar), row);
		gtk_widget_show_all(row);
		g_free(name);
	}
	g_strfreev(mps);
}

static gboolean
check_drives(gpointer data)
{
	Win *w = data;
	char *now = read_drives();

	if (g_strcmp0(now, w->mounts) != 0) {
		fill_drives(w, now);
		g_free(w->mounts);
		w->mounts = now;
		update_chrome(w);
		/* the open drive was removed */
		if (!g_file_test(w->cwd, G_FILE_TEST_IS_DIR))
			go(w, g_get_home_dir(), TRUE);
	} else {
		g_free(now);
	}
	return G_SOURCE_CONTINUE;
}

static GtkWidget *
build_sidebar(Win *w)
{
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

	w->sidebar = gtk_list_box_new();
	gtk_style_context_add_class(gtk_widget_get_style_context(w->sidebar),
				    "hf-sidebar");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->sidebar),
					GTK_SELECTION_SINGLE);
	g_signal_connect(w->sidebar, "row-activated",
			 G_CALLBACK(on_sidebar_activated), w);

	gtk_container_add(GTK_CONTAINER(w->sidebar), sidebar_header("Places"));
	gtk_container_add(GTK_CONTAINER(w->sidebar),
		sidebar_row("user-home-symbolic", "Home", g_get_home_dir()));
	add_place(w, G_USER_DIRECTORY_DESKTOP, "Desktop",
		  "user-desktop-symbolic", "Desktop");
	add_place(w, G_USER_DIRECTORY_DOCUMENTS, "Documents",
		  "folder-documents-symbolic", "Documents");
	add_place(w, G_USER_DIRECTORY_DOWNLOAD, "Downloads",
		  "folder-download-symbolic", "Downloads");
	add_place(w, G_USER_DIRECTORY_MUSIC, "Music",
		  "folder-music-symbolic", "Music");
	add_place(w, G_USER_DIRECTORY_PICTURES, "Pictures",
		  "folder-pictures-symbolic", "Pictures");
	add_place(w, G_USER_DIRECTORY_VIDEOS, "Videos",
		  "folder-videos-symbolic", "Videos");
	gtk_container_add(GTK_CONTAINER(w->sidebar),
		sidebar_row("user-trash-symbolic", "Trash", trash_files()));
	gtk_container_add(GTK_CONTAINER(w->sidebar), sidebar_header("Devices"));
	gtk_container_add(GTK_CONTAINER(w->sidebar),
		sidebar_row("drive-harddisk-symbolic", "Computer", "/"));

	w->mounts = read_drives();
	fill_drives(w, w->mounts);
	w->drives_id = g_timeout_add_seconds(2, check_drives, w);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), w->sidebar);
	gtk_widget_set_size_request(scroll, 180, -1);
	return scroll;
}

/*
 * File views
 */

static GtkWidget *
build_icons(Win *w)
{
	GtkCellRenderer *r;

	w->icons = gtk_icon_view_new();
	gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(w->icons),
					 GTK_SELECTION_MULTIPLE);
	gtk_icon_view_set_item_width(GTK_ICON_VIEW(w->icons), 92);
	gtk_icon_view_set_item_padding(GTK_ICON_VIEW(w->icons), 4);
	gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(w->icons), 4);
	gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(w->icons), 4);
	gtk_icon_view_set_margin(GTK_ICON_VIEW(w->icons), 10);
	gtk_icon_view_set_tooltip_column(GTK_ICON_VIEW(w->icons), -1);

	r = gtk_cell_renderer_pixbuf_new();
	g_object_set(r, "stock-size", GTK_ICON_SIZE_DIALOG, NULL);	/* 48 px */
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(w->icons), r, FALSE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(w->icons), r, "gicon", COL_ICON);

	r = gtk_cell_renderer_text_new();
	g_object_set(r, "xalign", 0.5, "alignment", PANGO_ALIGN_CENTER,
		     "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 88,
		     "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(w->icons), r, FALSE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(w->icons), r, "text", COL_NAME);

	g_signal_connect(w->icons, "item-activated", G_CALLBACK(on_item_activated), w);
	g_signal_connect(w->icons, "selection-changed",
			 G_CALLBACK(on_selection_changed), w);
	g_signal_connect(w->icons, "button-press-event",
			 G_CALLBACK(on_icons_button), w);
	g_signal_connect(w->icons, "popup-menu", G_CALLBACK(on_view_popup), w);

	w->icons_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(w->icons_scroll), w->icons);
	return w->icons_scroll;
}

static GtkTreeViewColumn *
text_column(Win *w, const char *title, int text_col, int sort_col, float xalign)
{
	GtkCellRenderer *r = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *c;

	g_object_set(r, "xalign", xalign, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	c = gtk_tree_view_column_new_with_attributes(title, r, "text", text_col, NULL);
	gtk_tree_view_column_set_sort_column_id(c, sort_col);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(w->list), c);
	return c;
}

static GtkWidget *
build_list(Win *w)
{
	GtkCellRenderer *r;
	GtkTreeViewColumn *c;
	GtkTreeSelection *sel;

	w->list = gtk_tree_view_new();
	gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(w->list), TRUE);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(w->list), COL_NAME);
	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->list));
	gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);

	c = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(c, "Name");
	r = gtk_cell_renderer_pixbuf_new();
	gtk_tree_view_column_pack_start(c, r, FALSE);
	gtk_tree_view_column_add_attribute(c, r, "gicon", COL_ICON);
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_add_attribute(c, r, "text", COL_NAME);
	gtk_tree_view_column_set_sort_column_id(c, COL_NAME);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_expand(c, TRUE);
	gtk_tree_view_column_set_min_width(c, 240);
	gtk_tree_view_append_column(GTK_TREE_VIEW(w->list), c);

	text_column(w, "Date modified", COL_DATE_TEXT, COL_MTIME, 0);
	w->type_col = text_column(w, "Type", COL_TYPE_TEXT, COL_TYPE_TEXT, 0);
	gtk_tree_view_column_set_fixed_width(w->type_col, 200);
	gtk_tree_view_column_set_sizing(w->type_col, GTK_TREE_VIEW_COLUMN_FIXED);
	text_column(w, "Size", COL_SIZE_TEXT, COL_SIZE, 1);

	g_signal_connect(w->list, "row-activated", G_CALLBACK(on_row_activated), w);
	g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), w);
	g_signal_connect(w->list, "button-press-event",
			 G_CALLBACK(on_list_button), w);
	g_signal_connect(w->list, "popup-menu", G_CALLBACK(on_view_popup), w);

	w->list_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(w->list_scroll), w->list);
	return w->list_scroll;
}

/*
 * Window
 */

static const char css[] =
	".hf-sidebar { background-color: alpha(@theme_fg_color, 0.04); }"
	".hf-sidebar row { padding: 5px 12px; }"
	".hf-section { font-size: 11px; font-weight: bold; opacity: 0.55;"
	"  padding: 10px 2px 2px 2px; }"
	".hf-toolbar { padding: 6px; border-bottom: 1px solid alpha(@theme_fg_color, 0.1); }"
	".hf-status { padding: 4px 10px; font-size: 12px; opacity: 0.8; }"
	".hf-statusbar { border-top: 1px solid alpha(@theme_fg_color, 0.1); }";

static GtkWidget *
icon_button(const char *icon, const char *tip, GCallback cb, Win *w)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);

	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_focus_on_click(b, FALSE);
	g_signal_connect(b, "clicked", cb, w);
	return b;
}

static void
on_window_destroy(GtkWidget *wd, Win *w)
{
	if (w->reload_id)
		g_source_remove(w->reload_id);
	if (w->drives_id)
		g_source_remove(w->drives_id);
	if (w->monitor) {
		g_file_monitor_cancel(w->monitor);
		g_object_unref(w->monitor);
	}
	if (w->sort) {
		g_object_unref(w->sort);
		g_object_unref(w->filter);
		g_object_unref(w->store);
	}
	g_queue_clear_full(&w->back, g_free);
	g_queue_clear_full(&w->forward, g_free);
	g_hash_table_unref(w->types);
	g_free(w->cwd);
	g_free(w->error);
	g_free(w->search_fold);
	g_free(w->pending_select);
	g_free(w->mounts);
	g_free(w);
}

static Win *
new_window(const char *path)
{
	Win *w = g_new0(Win, 1);
	GtkWidget *header, *menu_btn, *menu, *vbox, *toolbar, *paned, *right;
	GtkWidget *statusbar, *item;

	w->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_queue_init(&w->back);
	g_queue_init(&w->forward);

	w->window = gtk_application_window_new(app);
	g_object_set_data(G_OBJECT(w->window), "hexa-win", w);
	gtk_window_set_default_size(GTK_WINDOW(w->window), 1000, 640);
	g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), w);
	g_signal_connect(w->window, "key-press-event", G_CALLBACK(on_key), w);

	/* Header bar: title, view switch, menu */
	header = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
	gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Files");

	menu = gtk_menu_new();
	add_item(menu, "New _Window", "<Control>n", G_CALLBACK(on_new_window), w);
	add_item(menu, "New _Folder...", "<Control><Shift>n",
		 G_CALLBACK(on_new_folder), w);
	add_item(menu, "New _Text Document...", NULL, G_CALLBACK(on_new_doc), w);
	add_item(menu, "_Paste", "<Control>v", G_CALLBACK(on_paste), w);
	add_separator(menu);
	add_item(menu, "Open in _Terminal", NULL, G_CALLBACK(on_terminal), w);
	w->hidden_item = gtk_check_menu_item_new_with_mnemonic("Show _Hidden Files");
	{
		guint key;
		GdkModifierType mods;

		gtk_accelerator_parse("<Control>h", &key, &mods);
		gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(
			GTK_BIN(w->hidden_item))), key, mods);
	}
	g_signal_connect(w->hidden_item, "toggled", G_CALLBACK(on_hidden_toggled), w);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), w->hidden_item);
	add_separator(menu);
	w->empty_trash_item = add_item(menu, "_Empty Trash", NULL,
				       G_CALLBACK(on_empty_trash), w);
	add_separator(menu);
	item = add_item(menu, "_Close Window", "<Control>w", NULL, w);
	g_signal_connect_swapped(item, "activate",
				 G_CALLBACK(gtk_widget_destroy), w->window);
	gtk_widget_show_all(menu);

	menu_btn = gtk_menu_button_new();
	gtk_button_set_image(GTK_BUTTON(menu_btn), gtk_image_new_from_icon_name(
		"open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(menu_btn), menu);
	gtk_widget_set_tooltip_text(menu_btn, "Menu");
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_btn);
	w->view_btn = icon_button("view-list-symbolic", "Details (Ctrl+2)",
				  G_CALLBACK(on_view_toggle), w);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), w->view_btn);
	gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

	/* Toolbar: back, forward, up, address, search */
	toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_style_context_add_class(gtk_widget_get_style_context(toolbar),
				    "hf-toolbar");
	w->back_btn = icon_button("go-previous-symbolic", "Back (Alt+Left)",
				  G_CALLBACK(on_back), w);
	w->fwd_btn = icon_button("go-next-symbolic", "Forward (Alt+Right)",
				 G_CALLBACK(on_forward), w);
	w->up_btn = icon_button("go-up-symbolic", "Up to the parent folder (Alt+Up)",
				G_CALLBACK(on_up), w);
	gtk_container_add(GTK_CONTAINER(toolbar), w->back_btn);
	gtk_container_add(GTK_CONTAINER(toolbar), w->fwd_btn);
	gtk_container_add(GTK_CONTAINER(toolbar), w->up_btn);
	w->address = gtk_entry_new();
	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(w->address),
		GTK_ENTRY_ICON_PRIMARY, "folder-symbolic");
	gtk_widget_set_hexpand(w->address, TRUE);
	gtk_widget_set_margin_start(w->address, 4);
	g_signal_connect(w->address, "activate", G_CALLBACK(on_address), w);
	gtk_container_add(GTK_CONTAINER(toolbar), w->address);
	w->search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(w->search), "Search this folder");
	gtk_entry_set_width_chars(GTK_ENTRY(w->search), 24);
	g_signal_connect(w->search, "search-changed", G_CALLBACK(on_search_changed), w);
	gtk_container_add(GTK_CONTAINER(toolbar), w->search);

	/* Sidebar | views, status bar below */
	w->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(w->stack), build_icons(w), "icons");
	gtk_stack_add_named(GTK_STACK(w->stack), build_list(w), "list");
	gtk_widget_set_hexpand(w->stack, TRUE);
	gtk_widget_set_vexpand(w->stack, TRUE);

	statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(statusbar),
				    "hf-statusbar");
	w->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(w->status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(w->status), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(w->status, TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(w->status),
				    "hf-status");
	w->free_label = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(w->free_label),
				    "hf-status");
	gtk_container_add(GTK_CONTAINER(statusbar), w->status);
	gtk_container_add(GTK_CONTAINER(statusbar), w->free_label);

	right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(right), w->stack);
	gtk_container_add(GTK_CONTAINER(right), statusbar);

	paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(paned), build_sidebar(w), FALSE, FALSE);
	gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
	gtk_paned_set_position(GTK_PANED(paned), 200);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(vbox), toolbar);
	gtk_container_add(GTK_CONTAINER(vbox), paned);
	gtk_widget_set_vexpand(paned, TRUE);
	gtk_container_add(GTK_CONTAINER(w->window), vbox);

	gtk_widget_show_all(w->window);
	gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "icons");
	go(w, path && g_file_test(path, G_FILE_TEST_IS_DIR) ? path
						     : g_get_home_dir(), FALSE);
	return w;
}

static void
startup(GApplication *gapp, gpointer data)
{
	GtkCssProvider *provider;

	g_object_set(gtk_settings_get_default(),
		     "gtk-application-prefer-dark-theme", TRUE, NULL);
	provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

static void
activate(GApplication *gapp, gpointer data)
{
	new_window(g_get_home_dir());
}

/* hexa-files PATH...: a window per folder (a file: its folder) */
static void
open_files(GApplication *gapp, GFile **files, int n, const char *hint,
	   gpointer data)
{
	int i;

	for (i = 0; i < n; i++) {
		char *path = g_file_get_path(files[i]);

		if (path && !g_file_test(path, G_FILE_TEST_IS_DIR)) {
			char *dir = g_path_get_dirname(path);

			g_free(path);
			path = dir;
		}
		new_window(path);
		g_free(path);
	}
}

int
main(int argc, char **argv)
{
	GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
	int status;

	/* one process for all windows, through the session bus when there is one */
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;

	g_set_prgname(APP_ID);	/* GTK3 sends it as the Wayland app-id */
	app = gtk_application_new(APP_ID, flags);
	g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(open_files), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
