/*
 * hexa-hello: the smallest hexaOS app (GTK 3), a template for new ones.
 * The application id must match the Flatpak id, so the taskbar shows its icon.
 */
#include <gtk/gtk.h>

static void
on_click(GtkButton *button, gpointer label)
{
	static int clicks;
	char *text;

	clicks++;
	text = g_strdup_printf("Clicked %d time%s", clicks, clicks == 1 ? "" : "s");

	gtk_label_set_text(GTK_LABEL(label), text);
	g_free(text);
}

static void
activate(GtkApplication *app, gpointer data)
{
	GtkWidget *win, *box, *label, *button;

	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "Hello");
	gtk_window_set_default_size(GTK_WINDOW(win), 360, 200);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	label = gtk_label_new("Hello from a hexaOS Flatpak app!");
	button = gtk_button_new_with_label("Click me");
	g_signal_connect(button, "clicked", G_CALLBACK(on_click), label);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(win), box);
	gtk_widget_show_all(win);
}

int
main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new("com.hexaos.Hello", G_APPLICATION_DEFAULT_FLAGS);
	int status;

	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
