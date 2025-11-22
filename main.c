#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <glib.h>
#include <stdio.h>
#include "config.h"

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
  
  // Set margins for padding (transparent area)
  gtk_layer_set_margin (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_TOP, BAR_PADDING_TOP);
  gtk_layer_set_margin (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_LEFT, BAR_PADDING_HORIZONTAL);
  gtk_layer_set_margin (GTK_WINDOW (menu_window), GTK_LAYER_SHELL_EDGE_RIGHT, BAR_PADDING_HORIZONTAL);
  
  // Set exclusive zone to reserve space (height + top and bottom padding)
  gtk_layer_set_exclusive_zone (GTK_WINDOW (menu_window), BAR_HEIGHT + BAR_PADDING_TOP + BAR_PADDING_BOTTOM);
  
  // Make window background transparent
  gtk_widget_add_css_class (GTK_WIDGET (menu_window), "transparent-window");
  
  // Create outer container with padding (transparent - no background)
  GtkWidget *outer_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (outer_box, TRUE);
  gtk_widget_set_halign (outer_box, GTK_ALIGN_FILL);
  
  // Create inner bar container (with background, border, etc.)
  GtkWidget *bar_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_size_request (bar_box, -1, BAR_HEIGHT);
  gtk_widget_set_vexpand (bar_box, FALSE);
  gtk_widget_set_hexpand (bar_box, TRUE);
  gtk_widget_set_halign (bar_box, GTK_ALIGN_FILL);
  gtk_widget_set_valign (bar_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (bar_box, "bar");
  
  // Create CSS for the bar and transparent window
  GtkCssProvider *css_provider = gtk_css_provider_new ();
  
  // Convert hex color to rgba for opacity support
  guint bg_r, bg_g, bg_b;
  guint border_r, border_g, border_b;
  sscanf (BAR_BACKGROUND_COLOR, "#%02x%02x%02x", &bg_r, &bg_g, &bg_b);
  sscanf (BAR_BORDER_COLOR, "#%02x%02x%02x", &border_r, &border_g, &border_b);
  
  gchar *css = g_strdup_printf (
    ".transparent-window {"
    "  background-color: transparent;"
    "}"
    ".bar {"
    "  background-color: rgba(%u, %u, %u, %.2f);"
    "  border: %.2fpx solid rgba(%u, %u, %u, %.2f);"
    "  border-radius: %dpx;"
    "  padding-left: 10px;"
    "  padding-right: 10px;"
    "  padding-top: 0px;"
    "  padding-bottom: 0px;"
    "  min-height: %dpx;"
    "  max-height: %dpx;"
    "  height: %dpx;"
    "  overflow: hidden;"
    "  font-family: %s;"
    "  font-size: %dpt;"
    "}"
    ".bar label {"
    "  font-family: %s;"
    "  font-size: %dpt;"
    "  margin-top: 0px;"
    "  margin-bottom: 0px;"
    "}",
    bg_r, bg_g, bg_b, BAR_BACKGROUND_OPACITY,
    BAR_BORDER_WIDTH,
    border_r, border_g, border_b, BAR_BACKGROUND_OPACITY,
    BAR_BORDER_RADIUS,
    BAR_HEIGHT,
    BAR_HEIGHT,
    BAR_HEIGHT,
    BAR_FONT,
    BAR_TEXT_SIZE,
    BAR_FONT,
    BAR_TEXT_SIZE
  );
  
  gtk_css_provider_load_from_data (css_provider, css, -1);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                               GTK_STYLE_PROVIDER (css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free (css);
  g_object_unref (css_provider);
  
  // Add content to bar
  GtkWidget *label = gtk_label_new ("Menu Bar");
  gtk_box_append (GTK_BOX (bar_box), label);
  
  gtk_box_append (GTK_BOX (bar_box), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  
  GtkWidget *status_label = gtk_label_new ("Status: Active");
  gtk_box_append (GTK_BOX (bar_box), status_label);
  
  gtk_box_append (GTK_BOX (outer_box), bar_box);
  gtk_window_set_child (GTK_WINDOW (menu_window), outer_box);
  
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
  
  // Ensure no margins - background should cover entire screen
  gtk_layer_set_margin (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_margin (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin (GTK_WINDOW (window), GTK_LAYER_SHELL_EDGE_RIGHT, 0);
  
  // Set exclusive zone to -1 to ensure background always covers full screen
  // and doesn't respect exclusive zones from other windows
  gtk_layer_set_exclusive_zone (GTK_WINDOW (window), -1);
  
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

