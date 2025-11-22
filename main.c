#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "config.h"

// Structure to hold update data for main thread
typedef struct {
  GtkWidget *widget;
  gchar *new_output;
} UpdateData;

// Structure to hold item widget and update info
typedef struct {
  GtkWidget *widget;
  const char *command;
  int interval;
  GThread *thread;           // Worker thread for this module
  gboolean should_stop;      // Flag to stop the thread
  gboolean thread_running;   // Flag to track if thread is active
  GMutex mutex;              // Mutex for thread-safe access
  GCond cond;                // Condition variable for interruptible sleep
  gchar *previous_output;     // Previous output for change detection
} BarItemData;

static gchar *background_image_path = NULL;
static BarItemData *bar_items_data = NULL;

// Execute command and return output
static gchar *
execute_command (const char *command)
{
  FILE *fp = popen (command, "r");
  if (fp == NULL)
    return NULL;
  
  gchar *output = NULL;
  gsize len = 0;
  gchar buffer[1024];
  
  while (fgets (buffer, sizeof (buffer), fp) != NULL)
    {
      gsize buffer_len = strlen (buffer);
      output = g_realloc (output, len + buffer_len + 1);
      memcpy (output + len, buffer, buffer_len);
      len += buffer_len;
      output[len] = '\0';
    }
  
  pclose (fp);
  
  // Remove trailing newline if present
  if (output != NULL && len > 0 && output[len - 1] == '\n')
    {
      output[len - 1] = '\0';
    }
  
  return output;
}

// Idle callback to update UI from main thread (called when worker thread signals)
static gboolean
update_ui_from_main_thread (gpointer user_data)
{
  UpdateData *update_data = (UpdateData *) user_data;
  
  if (update_data->widget != NULL && update_data->new_output != NULL)
    {
      gtk_label_set_text (GTK_LABEL (update_data->widget), update_data->new_output);
    }
  
  // Free the update data
  g_free (update_data->new_output);
  g_free (update_data);
  
  return G_SOURCE_REMOVE;  // Remove the idle source after execution
}

// Worker thread function: polls at module interval and signals main thread on change
static gpointer
module_worker_thread (gpointer user_data)
{
  BarItemData *item_data = (BarItemData *) user_data;
  
  // Mark thread as running
  g_mutex_lock (&item_data->mutex);
  item_data->thread_running = TRUE;
  g_mutex_unlock (&item_data->mutex);
  
  // Initial update
  gchar *output = execute_command (item_data->command);
  if (output != NULL)
    {
      g_mutex_lock (&item_data->mutex);
      item_data->previous_output = g_strdup (output);
      g_mutex_unlock (&item_data->mutex);
      
      // Signal main thread to update UI
      // Read widget pointer with mutex protection (though it's set during init and never modified)
      g_mutex_lock (&item_data->mutex);
      GtkWidget *widget = item_data->widget;
      g_mutex_unlock (&item_data->mutex);
      
      UpdateData *update_data = g_malloc (sizeof (UpdateData));
      update_data->widget = widget;
      update_data->new_output = g_strdup (output);
      g_idle_add (update_ui_from_main_thread, update_data);
      
      g_free (output);
    }
  
  // Poll at module's interval
  while (TRUE)
    {
      g_mutex_lock (&item_data->mutex);
      
      // Wait for interval or stop signal (interruptible sleep)
      gint64 end_time = g_get_monotonic_time () + (item_data->interval * 1000);  // microseconds
      
      // Wait with timeout - will wake up on cond signal or timeout
      while (!item_data->should_stop)
        {
          if (!g_cond_wait_until (&item_data->cond, &item_data->mutex, end_time))
            {
              // Timeout occurred - break to execute command
              break;
            }
          // If woken by signal and should_stop is true, break
          if (item_data->should_stop)
            break;
        }
      
      if (item_data->should_stop)
        {
          g_mutex_unlock (&item_data->mutex);
          break;
        }
      g_mutex_unlock (&item_data->mutex);
      
      // Execute command to get new output
      output = execute_command (item_data->command);
      
      if (output != NULL)
        {
          g_mutex_lock (&item_data->mutex);
          
          // Compare with previous output - only signal if changed
          if (item_data->previous_output == NULL || 
              strcmp (item_data->previous_output, output) != 0)
            {
              // Data changed - signal main thread to update UI
              // Read widget pointer while holding mutex
              GtkWidget *widget = item_data->widget;
              
              UpdateData *update_data = g_malloc (sizeof (UpdateData));
              update_data->widget = widget;
              update_data->new_output = g_strdup (output);
              
              // Update stored previous output
              g_free (item_data->previous_output);
              item_data->previous_output = g_strdup (output);
              
              g_mutex_unlock (&item_data->mutex);
              
              // Queue update to main thread
              g_idle_add (update_ui_from_main_thread, update_data);
            }
          else
            {
              g_mutex_unlock (&item_data->mutex);
            }
          
          g_free (output);
        }
    }
  
  // Mark thread as no longer running
  g_mutex_lock (&item_data->mutex);
  item_data->thread_running = FALSE;
  g_cond_signal (&item_data->cond);  // Signal in case cleanup is waiting
  g_mutex_unlock (&item_data->mutex);
  
  return NULL;
}

// Cleanup function to free allocated resources
static void
cleanup_resources (void)
{
  // Stop all worker threads and free resources
  if (bar_items_data != NULL)
    {
      for (int i = 0; i < BAR_ITEMS_COUNT; i++)
        {
          BarItemData *item_data = &bar_items_data[i];
          
          // Signal thread to stop
          // Read thread pointer with mutex protection to avoid race condition
          g_mutex_lock (&item_data->mutex);
          GThread *thread = item_data->thread;
          if (thread != NULL)
            {
              item_data->should_stop = TRUE;
              // Wake up thread if it's sleeping
              g_cond_signal (&item_data->cond);
              // Clear thread reference while holding mutex
              item_data->thread = NULL;
            }
          g_mutex_unlock (&item_data->mutex);
          
          if (thread != NULL)
            {
              // Wait for thread to finish with timeout (5 seconds)
              
              // Try to join with a reasonable timeout
              // Note: g_thread_join doesn't have timeout, so we use a workaround
              // by checking thread_running flag
              gint timeout = 50;  // 50 * 100ms = 5 seconds
              while (timeout > 0)
                {
                  g_mutex_lock (&item_data->mutex);
                  gboolean running = item_data->thread_running;
                  g_mutex_unlock (&item_data->mutex);
                  
                  if (!running)
                    break;
                  
                  g_usleep (100000);  // 100ms
                  timeout--;
                }
              
              // Join the thread (should be quick now)
              g_thread_join (thread);
            }
          
          // Clean up mutex and condition variable
          // Only clear if thread was actually created (mutex/cond were initialized)
          if (item_data->command != NULL && strcmp (item_data->command, "<separator>") != 0)
            {
              g_cond_clear (&item_data->cond);
              g_mutex_clear (&item_data->mutex);
              
              // Free stored previous output
              if (item_data->previous_output != NULL)
                {
                  g_free (item_data->previous_output);
                  item_data->previous_output = NULL;
                }
            }
        }
      g_free (bar_items_data);
      bar_items_data = NULL;
    }
}

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
  
  gtk_css_provider_load_from_string (css_provider, css);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                               GTK_STYLE_PROVIDER (css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free (css);
  g_object_unref (css_provider);
  
  // Allocate memory for item data
  bar_items_data = g_malloc0 (sizeof (BarItemData) * BAR_ITEMS_COUNT);
  
  // Add content to bar from config
  for (int i = 0; i < BAR_ITEMS_COUNT; i++)
    {
      const BarItem *item = &BAR_ITEMS[i];
      BarItemData *item_data = &bar_items_data[i];
      item_data->command = item->command;
      item_data->interval = item->interval;
      
      if (strcmp (item->command, "<separator>") == 0)
        {
          // Create separator that expands
          GtkWidget *separator = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
          gtk_widget_set_hexpand (separator, TRUE);
          gtk_widget_set_halign (separator, GTK_ALIGN_FILL);
          item_data->widget = separator;
          gtk_box_append (GTK_BOX (bar_box), separator);
        }
      else
        {
          // Create label for command output
          GtkWidget *label = gtk_label_new ("");
          gtk_widget_set_halign (label, GTK_ALIGN_START);
          item_data->widget = label;
          gtk_box_append (GTK_BOX (bar_box), label);
          
          // Initialize thread-related fields
          item_data->should_stop = FALSE;
          item_data->thread_running = FALSE;
          item_data->previous_output = NULL;
          item_data->thread = NULL;
          g_mutex_init (&item_data->mutex);
          g_cond_init (&item_data->cond);
          
          // Spawn worker thread for this module if interval > 0
          // Ensure thread is only created once
          if (item->interval > 0 && item_data->thread == NULL)
            {
              GError *error = NULL;
              item_data->thread = g_thread_try_new (
                "module-worker",
                module_worker_thread,
                item_data,
                &error
              );
              
              if (item_data->thread == NULL)
                {
                  g_printerr ("Failed to create thread for module %d: %s\n", i, 
                             error ? error->message : "Unknown error");
                  if (error)
                    g_error_free (error);
                }
            }
          else
            {
              // No interval - execute once immediately
              gchar *output = execute_command (item_data->command);
              if (output != NULL)
                {
                  gtk_label_set_text (GTK_LABEL (label), output);
                  g_free (output);
                }
            }
        }
    }
  
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
  
  // Connect shutdown signal to ensure cleanup on application termination
  g_signal_connect (app, "shutdown", G_CALLBACK (cleanup_resources), NULL);
  
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  
  // Cleanup allocated resources (in case shutdown signal didn't fire)
  cleanup_resources ();
  g_free (background_image_path);
  g_object_unref (app);
  return status;
}

