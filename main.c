#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <glib.h>

static gchar *background_image_path = NULL;

static void
create_menu_bar (GtkApplication *app)
{
  GtkWidget *menu_window = gtk_application_window_new (app);
  gtk_layer_init_for_window (GTK_WINDOW (menu_window));
  gtk_layer_set_namespace (GTK_WINDOW (menu_window), "bar");
  gtk_layer_set_layer (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_LAYER_TOP);
  
  // Disable keyboard interactivity so menu bar doesn't accept focus
  gtk_layer_set_keyboard_mode (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  
  // Anchor to top edge
  gtk_layer_set_anchor (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  
  // Set exclusive zone to reserve space (height in pixels)
  gtk_layer_set_margin (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_exclusive_zone (GTK_WINDOW (menu_window), 40);
  
  // Create menu bar content
  GtkWidget *menu_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_start (menu_box, 10);
  gtk_widget_set_margin_end (menu_box, 10);
  gtk_widget_set_margin_top (menu_box, 5);
  gtk_widget_set_margin_bottom (menu_box, 5);
  gtk_widget_set_size_request (menu_box, -1, 40);
  
  GtkWidget *label = gtk_label_new ("Menu Bar");
  gtk_box_append (GTK_BOX (menu_box), label);
  
  gtk_box_append (GTK_BOX (menu_box), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  
  GtkWidget *status_label = gtk_label_new ("Status: Active");
  gtk_box_append (GTK_BOX (menu_box), status_label);
  
  gtk_widget_set_halign (menu_box, GTK_ALIGN_FILL);
  gtk_window_set_child (GTK_WINDOW (menu_window), menu_box);
  
  gtk_widget_set_visible (menu_window, TRUE);
}

static void
activate (GtkApplication *app, gpointer user_data)
{
  // Create background window
  GtkWidget *window = gtk_application_window_new (app);
  gtk_layer_init_for_window (GTK_WINDOW (window));
  gtk_layer_set_namespace (GTK_WINDOW (window), "background");
  gtk_layer_set_layer (GTK_WINDOW (window), GTK_LAYER_SHELL_LAYER_BACKGROUND);
  
  // Disable keyboard interactivity so it stays behind other layer shell windows
  gtk_layer_set_keyboard_mode (GTK_WINDOW (window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  
  // Anchor to all edges to cover the entire screen
  gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  
  // Set background image if provided
  if (background_image_path != NULL)
    {
      GtkWidget *picture = gtk_picture_new_for_filename (background_image_path);
      if (picture != NULL)
        {
          gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_FILL);
          gtk_window_set_child (GTK_WINDOW (window), picture);
        }
      else
        {
          g_printerr ("Failed to load image: %s\n", background_image_path);
        }
    }
  
  gtk_widget_set_visible (window, TRUE);
  
  // Create menu bar window
  create_menu_bar (app);
}

int
main (int argc, char **argv)
{
  // Parse command line arguments
  GOptionContext *context;
  GOptionEntry entries[] = {
    { "background-image", 'b', 0, G_OPTION_ARG_STRING, &background_image_path,
      "Path to background image", "PATH" },
    { NULL }
  };
  
  context = g_option_context_new ("- Desktop background layer shell");
  g_option_context_add_main_entries (context, entries, NULL);
  
  GError *error = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s\n", error->message);
      g_error_free (error);
      g_option_context_free (context);
      return 1;
    }
  
  g_option_context_free (context);
  
  GtkApplication *app = gtk_application_new ("org.example.layer-shell", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  
  g_free (background_image_path);
  g_object_unref (app);
  return status;
}

