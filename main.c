#include "config.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Structure to hold update data for main thread
typedef struct {
  GtkWidget *widget;
  gchar *new_output;
} UpdateData;

// Structure to hold weather update data for main thread
typedef struct {
  GtkWidget *emoji_widget;
  GtkWidget *temp_widget;
  gchar *new_emoji;
  gchar *new_temp;
} WeatherUpdateData;

// Structure to hold item widget and update info
typedef struct {
  GtkWidget *widget;
  const char *command;
  int interval;
  GThread *thread;         // Worker thread for this module
  gboolean should_stop;    // Flag to stop the thread
  gboolean thread_running; // Flag to track if thread is active
  GMutex mutex;            // Mutex for thread-safe access
  GCond cond;              // Condition variable for interruptible sleep
  gchar *previous_output;  // Previous output for change detection
} BarItemData;

static gchar *background_image_path = NULL;
static BarItemData *bar_items_data = NULL;

// Structure to hold weather widget and update info
typedef struct {
  GtkWidget *emoji_widget;
  GtkWidget *temp_widget;
  GThread *thread;
  gboolean should_stop;
  gboolean thread_running;
  GMutex mutex;
  GCond cond;
  gchar *previous_emoji;
  gchar *previous_temp;
} WeatherData;

static WeatherData *weather_data = NULL;

// Execute command and return output
static gchar *execute_command(const char *command) {
  FILE *fp = popen(command, "r");
  if (fp == NULL)
    return NULL;

  gchar *output = NULL;
  gsize len = 0;
  gchar buffer[1024];

  while (fgets(buffer, sizeof(buffer), fp) != NULL) {
    gsize buffer_len = strlen(buffer);
    output = g_realloc(output, len + buffer_len + 1);
    memcpy(output + len, buffer, buffer_len);
    len += buffer_len;
    output[len] = '\0';
  }

  pclose(fp);

  // Remove trailing newline if present
  if (output != NULL && len > 0 && output[len - 1] == '\n') {
    output[len - 1] = '\0';
  }

  return output;
}

// Idle callback to update UI from main thread (called when worker thread
// signals)
static gboolean update_ui_from_main_thread(gpointer user_data) {
  UpdateData *update_data = (UpdateData *)user_data;

  if (update_data->widget != NULL && update_data->new_output != NULL) {
    gtk_label_set_text(GTK_LABEL(update_data->widget), update_data->new_output);
  }

  // Free the update data
  g_free(update_data->new_output);
  g_free(update_data);

  return G_SOURCE_REMOVE; // Remove the idle source after execution
}

// Idle callback to update weather UI from main thread
static gboolean update_weather_ui_from_main_thread(gpointer user_data) {
  WeatherUpdateData *update_data = (WeatherUpdateData *)user_data;

  if (update_data->emoji_widget != NULL && update_data->new_emoji != NULL) {
    gtk_label_set_text(GTK_LABEL(update_data->emoji_widget),
                       update_data->new_emoji);
  }

  if (update_data->temp_widget != NULL && update_data->new_temp != NULL) {
    gtk_label_set_text(GTK_LABEL(update_data->temp_widget),
                       update_data->new_temp);
  }

  // Free the update data
  g_free(update_data->new_emoji);
  g_free(update_data->new_temp);
  g_free(update_data);

  return G_SOURCE_REMOVE;
}

// Worker thread function: polls at module interval and signals main thread on
// change
static gpointer module_worker_thread(gpointer user_data) {
  BarItemData *item_data = (BarItemData *)user_data;

  // Mark thread as running
  g_mutex_lock(&item_data->mutex);
  item_data->thread_running = TRUE;
  g_mutex_unlock(&item_data->mutex);

  // Initial update
  gchar *output = execute_command(item_data->command);
  if (output != NULL) {
    g_mutex_lock(&item_data->mutex);
    item_data->previous_output = g_strdup(output);
    g_mutex_unlock(&item_data->mutex);

    // Signal main thread to update UI
    // Read widget pointer with mutex protection (though it's set during init
    // and never modified)
    g_mutex_lock(&item_data->mutex);
    GtkWidget *widget = item_data->widget;
    g_mutex_unlock(&item_data->mutex);

    UpdateData *update_data = g_malloc(sizeof(UpdateData));
    update_data->widget = widget;
    update_data->new_output = g_strdup(output);
    g_idle_add(update_ui_from_main_thread, update_data);

    g_free(output);
  }

  // Poll at module's interval
  while (TRUE) {
    g_mutex_lock(&item_data->mutex);

    // Wait for interval or stop signal (interruptible sleep)
    gint64 end_time =
        g_get_monotonic_time() + (item_data->interval * 1000); // microseconds

    // Wait with timeout - will wake up on cond signal or timeout
    while (!item_data->should_stop) {
      if (!g_cond_wait_until(&item_data->cond, &item_data->mutex, end_time)) {
        // Timeout occurred - break to execute command
        break;
      }
      // If woken by signal and should_stop is true, break
      if (item_data->should_stop)
        break;
    }

    if (item_data->should_stop) {
      g_mutex_unlock(&item_data->mutex);
      break;
    }
    g_mutex_unlock(&item_data->mutex);

    // Execute command to get new output
    output = execute_command(item_data->command);

    if (output != NULL) {
      g_mutex_lock(&item_data->mutex);

      // Compare with previous output - only signal if changed
      if (item_data->previous_output == NULL ||
          strcmp(item_data->previous_output, output) != 0) {
        // Data changed - signal main thread to update UI
        // Read widget pointer while holding mutex
        GtkWidget *widget = item_data->widget;

        UpdateData *update_data = g_malloc(sizeof(UpdateData));
        update_data->widget = widget;
        update_data->new_output = g_strdup(output);

        // Update stored previous output
        g_free(item_data->previous_output);
        item_data->previous_output = g_strdup(output);

        g_mutex_unlock(&item_data->mutex);

        // Queue update to main thread
        g_idle_add(update_ui_from_main_thread, update_data);
      } else {
        g_mutex_unlock(&item_data->mutex);
      }

      g_free(output);
    }
  }

  // Mark thread as no longer running
  g_mutex_lock(&item_data->mutex);
  item_data->thread_running = FALSE;
  g_cond_signal(&item_data->cond); // Signal in case cleanup is waiting
  g_mutex_unlock(&item_data->mutex);

  return NULL;
}

// Weather worker thread function: polls at interval and signals main thread on
// change
static gpointer weather_worker_thread(gpointer user_data) {
  WeatherData *wdata = (WeatherData *)user_data;

  // Mark thread as running
  g_mutex_lock(&wdata->mutex);
  wdata->thread_running = TRUE;
  g_mutex_unlock(&wdata->mutex);

  // Initial update
  gchar *emoji =
      execute_command("curl -s wttr.in/ballia?format=3 | awk '{print $2}'");
  gchar *temp = execute_command(
      "curl -s wttr.in/ballia?format=3 | awk '{print $3}' | cut -d \"+\" -f2");

  if (emoji != NULL || temp != NULL) {
    g_mutex_lock(&wdata->mutex);
    if (emoji != NULL)
      wdata->previous_emoji = g_strdup(emoji);
    if (temp != NULL)
      wdata->previous_temp = g_strdup(temp);

    GtkWidget *emoji_widget = wdata->emoji_widget;
    GtkWidget *temp_widget = wdata->temp_widget;
    g_mutex_unlock(&wdata->mutex);

    WeatherUpdateData *update_data = g_malloc(sizeof(WeatherUpdateData));
    update_data->emoji_widget = emoji_widget;
    update_data->temp_widget = temp_widget;
    update_data->new_emoji = emoji ? g_strdup(emoji) : NULL;
    update_data->new_temp = temp ? g_strdup(temp) : NULL;
    g_idle_add(update_weather_ui_from_main_thread, update_data);

    g_free(emoji);
    g_free(temp);
  }

  // Poll at weather update interval
  while (TRUE) {
    g_mutex_lock(&wdata->mutex);

    // Wait for interval or stop signal
    gint64 end_time = g_get_monotonic_time() +
                      (WEATHER_UPDATE_INTERVAL * 1000); // microseconds

    while (!wdata->should_stop) {
      if (!g_cond_wait_until(&wdata->cond, &wdata->mutex, end_time)) {
        break;
      }
      if (wdata->should_stop)
        break;
    }

    if (wdata->should_stop) {
      g_mutex_unlock(&wdata->mutex);
      break;
    }
    g_mutex_unlock(&wdata->mutex);

    // Execute commands to get new weather data
    emoji =
        execute_command("curl -s wttr.in/ballia?format=3 | awk '{print $2}'");
    temp = execute_command("curl -s wttr.in/ballia?format=3 | awk '{print $3}' "
                           "| cut -d \"+\" -f2");

    if (emoji != NULL || temp != NULL) {
      g_mutex_lock(&wdata->mutex);

      gboolean emoji_changed = FALSE;
      gboolean temp_changed = FALSE;

      if (emoji != NULL && (wdata->previous_emoji == NULL ||
                            strcmp(wdata->previous_emoji, emoji) != 0)) {
        emoji_changed = TRUE;
        g_free(wdata->previous_emoji);
        wdata->previous_emoji = g_strdup(emoji);
      }

      if (temp != NULL && (wdata->previous_temp == NULL ||
                           strcmp(wdata->previous_temp, temp) != 0)) {
        temp_changed = TRUE;
        g_free(wdata->previous_temp);
        wdata->previous_temp = g_strdup(temp);
      }

      if (emoji_changed || temp_changed) {
        GtkWidget *emoji_widget = wdata->emoji_widget;
        GtkWidget *temp_widget = wdata->temp_widget;

        WeatherUpdateData *update_data = g_malloc(sizeof(WeatherUpdateData));
        update_data->emoji_widget = emoji_widget;
        update_data->temp_widget = temp_widget;
        update_data->new_emoji =
            emoji_changed && emoji ? g_strdup(emoji) : NULL;
        update_data->new_temp = temp_changed && temp ? g_strdup(temp) : NULL;

        g_mutex_unlock(&wdata->mutex);

        g_idle_add(update_weather_ui_from_main_thread, update_data);
      } else {
        g_mutex_unlock(&wdata->mutex);
      }

      g_free(emoji);
      g_free(temp);
    }
  }

  // Mark thread as no longer running
  g_mutex_lock(&wdata->mutex);
  wdata->thread_running = FALSE;
  g_cond_signal(&wdata->cond);
  g_mutex_unlock(&wdata->mutex);

  return NULL;
}

// Cleanup function to free allocated resources
static void cleanup_resources(void) {
  // Stop all worker threads and free resources
  if (bar_items_data != NULL) {
    for (int i = 0; i < BAR_ITEMS_COUNT; i++) {
      BarItemData *item_data = &bar_items_data[i];

      // Signal thread to stop
      // Read thread pointer with mutex protection to avoid race condition
      g_mutex_lock(&item_data->mutex);
      GThread *thread = item_data->thread;
      if (thread != NULL) {
        item_data->should_stop = TRUE;
        // Wake up thread if it's sleeping
        g_cond_signal(&item_data->cond);
        // Clear thread reference while holding mutex
        item_data->thread = NULL;
      }
      g_mutex_unlock(&item_data->mutex);

      if (thread != NULL) {
        // Wait for thread to finish with timeout (5 seconds)

        // Try to join with a reasonable timeout
        // Note: g_thread_join doesn't have timeout, so we use a workaround
        // by checking thread_running flag
        gint timeout = 50; // 50 * 100ms = 5 seconds
        while (timeout > 0) {
          g_mutex_lock(&item_data->mutex);
          gboolean running = item_data->thread_running;
          g_mutex_unlock(&item_data->mutex);

          if (!running)
            break;

          g_usleep(100000); // 100ms
          timeout--;
        }

        // Join the thread (should be quick now)
        g_thread_join(thread);
      }

      // Clean up mutex and condition variable
      // Only clear if thread was actually created (mutex/cond were initialized)
      if (item_data->command != NULL &&
          strcmp(item_data->command, "<separator>") != 0) {
        g_cond_clear(&item_data->cond);
        g_mutex_clear(&item_data->mutex);

        // Free stored previous output
        if (item_data->previous_output != NULL) {
          g_free(item_data->previous_output);
          item_data->previous_output = NULL;
        }
      }
    }
    g_free(bar_items_data);
    bar_items_data = NULL;
  }

  // Cleanup weather thread
  if (weather_data != NULL) {
    g_mutex_lock(&weather_data->mutex);
    GThread *thread = weather_data->thread;
    if (thread != NULL) {
      weather_data->should_stop = TRUE;
      g_cond_signal(&weather_data->cond);
      weather_data->thread = NULL;
    }
    g_mutex_unlock(&weather_data->mutex);

    if (thread != NULL) {
      // Wait for thread to finish with timeout
      gint timeout = 50; // 50 * 100ms = 5 seconds
      while (timeout > 0) {
        g_mutex_lock(&weather_data->mutex);
        gboolean running = weather_data->thread_running;
        g_mutex_unlock(&weather_data->mutex);

        if (!running)
          break;

        g_usleep(100000); // 100ms
        timeout--;
      }

      g_thread_join(thread);
    }

    // Clean up mutex and condition variable
    g_cond_clear(&weather_data->cond);
    g_mutex_clear(&weather_data->mutex);

    // Free stored previous data
    if (weather_data->previous_emoji != NULL) {
      g_free(weather_data->previous_emoji);
    }
    if (weather_data->previous_temp != NULL) {
      g_free(weather_data->previous_temp);
    }

    g_free(weather_data);
    weather_data = NULL;
  }
}

static void create_menu_bar(GtkApplication *app) {
  GtkWidget *menu_window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(menu_window));
  gtk_layer_set_namespace(GTK_WINDOW(menu_window), "bar");
  gtk_layer_set_layer(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_LAYER_TOP);

  // Disable keyboard interactivity so menu bar doesn't accept focus
  gtk_layer_set_keyboard_mode(GTK_WINDOW(menu_window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

  // Anchor to top edge
  gtk_layer_set_anchor(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_LEFT,
                       TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_RIGHT,
                       TRUE);

  // Set margins for padding (transparent area)
  gtk_layer_set_margin(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_TOP,
                       BAR_PADDING_TOP);
  gtk_layer_set_margin(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_LEFT,
                       BAR_PADDING_HORIZONTAL);
  gtk_layer_set_margin(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_RIGHT,
                       BAR_PADDING_HORIZONTAL);

  // Set exclusive zone to reserve space (height + top and bottom padding)
  gtk_layer_set_exclusive_zone(GTK_WINDOW(menu_window), BAR_HEIGHT +
                                                            BAR_PADDING_TOP +
                                                            BAR_PADDING_BOTTOM);

  // Make window background transparent
  gtk_widget_add_css_class(GTK_WIDGET(menu_window), "transparent-window");

  // Create outer container with padding (transparent - no background)
  GtkWidget *outer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(outer_box, TRUE);
  gtk_widget_set_halign(outer_box, GTK_ALIGN_FILL);

  // Create inner bar container (with background, border, etc.)
  GtkWidget *bar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_size_request(bar_box, -1, BAR_HEIGHT);
  gtk_widget_set_vexpand(bar_box, FALSE);
  gtk_widget_set_hexpand(bar_box, TRUE);
  gtk_widget_set_halign(bar_box, GTK_ALIGN_FILL);
  gtk_widget_set_valign(bar_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(bar_box, "bar");

  // Create CSS for the bar and transparent window
  GtkCssProvider *css_provider = gtk_css_provider_new();

  // Convert hex color to rgba for opacity support
  guint bg_r, bg_g, bg_b;
  guint border_r, border_g, border_b;
  sscanf(BAR_BACKGROUND_COLOR, "#%02x%02x%02x", &bg_r, &bg_g, &bg_b);
  sscanf(BAR_BORDER_COLOR, "#%02x%02x%02x", &border_r, &border_g, &border_b);

  gchar *css = g_strdup_printf(
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
      bg_r, bg_g, bg_b, BAR_BACKGROUND_OPACITY, BAR_BORDER_WIDTH, border_r,
      border_g, border_b, BAR_BACKGROUND_OPACITY, BAR_BORDER_RADIUS, BAR_HEIGHT,
      BAR_HEIGHT, BAR_HEIGHT, BAR_FONT, BAR_TEXT_SIZE, BAR_FONT, BAR_TEXT_SIZE);

  gtk_css_provider_load_from_string(css_provider, css);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free(css);
  g_object_unref(css_provider);

  // Allocate memory for item data
  bar_items_data = g_malloc0(sizeof(BarItemData) * BAR_ITEMS_COUNT);

  // Add content to bar from config
  for (int i = 0; i < BAR_ITEMS_COUNT; i++) {
    const BarItem *item = &BAR_ITEMS[i];
    BarItemData *item_data = &bar_items_data[i];
    item_data->command = item->command;
    item_data->interval = item->interval;

    if (strcmp(item->command, "<separator>") == 0) {
      // Create separator that expands
      GtkWidget *separator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand(separator, TRUE);
      gtk_widget_set_halign(separator, GTK_ALIGN_FILL);
      item_data->widget = separator;
      gtk_box_append(GTK_BOX(bar_box), separator);
    } else {
      // Create label for command output
      GtkWidget *label = gtk_label_new("");
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      item_data->widget = label;
      gtk_box_append(GTK_BOX(bar_box), label);

      // Initialize thread-related fields
      item_data->should_stop = FALSE;
      item_data->thread_running = FALSE;
      item_data->previous_output = NULL;
      item_data->thread = NULL;
      g_mutex_init(&item_data->mutex);
      g_cond_init(&item_data->cond);

      // Spawn worker thread for this module if interval > 0
      // Ensure thread is only created once
      if (item->interval > 0 && item_data->thread == NULL) {
        GError *error = NULL;
        item_data->thread = g_thread_try_new(
            "module-worker", module_worker_thread, item_data, &error);

        if (item_data->thread == NULL) {
          g_printerr("Failed to create thread for module %d: %s\n", i,
                     error ? error->message : "Unknown error");
          if (error)
            g_error_free(error);
        }
      } else {
        // No interval - execute once immediately
        gchar *output = execute_command(item_data->command);
        if (output != NULL) {
          gtk_label_set_text(GTK_LABEL(label), output);
          g_free(output);
        }
      }
    }
  }

  gtk_box_append(GTK_BOX(outer_box), bar_box);
  gtk_window_set_child(GTK_WINDOW(menu_window), outer_box);

  gtk_widget_set_visible(menu_window, TRUE);
}

static void create_day_text(GtkApplication *app) {
  // Get current date information
  time_t rawtime;
  struct tm *timeinfo;
  char day_name[32];
  char month_name[32];
  char day_number[8];

  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(day_name, sizeof(day_name), "%A", timeinfo);
  strftime(month_name, sizeof(month_name), "%B", timeinfo);
  strftime(day_number, sizeof(day_number), "%d", timeinfo);

  // Convert day name to uppercase
  for (int i = 0; day_name[i]; i++) {
    day_name[i] = g_ascii_toupper(day_name[i]);
  }

  for (int i = 0; month_name[i]; i++) {
    month_name[i] = g_ascii_toupper(month_name[i]);
  }

  // Create day text window
  GtkWidget *day_window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(day_window));
  gtk_layer_set_namespace(GTK_WINDOW(day_window), "day-text");
  gtk_layer_set_layer(GTK_WINDOW(day_window), GTK_LAYER_SHELL_LAYER_BACKGROUND);

  // Disable keyboard interactivity so it stays behind other layer shell windows
  gtk_layer_set_keyboard_mode(GTK_WINDOW(day_window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

  // Anchor to all edges to cover the entire screen
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                       TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_RIGHT,
                       TRUE);

  // Ensure no margins - should cover entire screen
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_RIGHT, 0);

  // Set exclusive zone to -1 (above background which is -2)
  gtk_layer_set_exclusive_zone(GTK_WINDOW(day_window), -1);

  // Make window background transparent
  gtk_widget_add_css_class(GTK_WIDGET(day_window), "transparent-day-window");

  // Create vertical box to stack month+day and day name
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

  // Create a centered container - both rows will be left-aligned within it
  // This ensures their left edges align, while the container itself is centered
  GtkWidget *align_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(align_container, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(align_container, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(align_container, FALSE);
  gtk_widget_add_css_class(align_container, "date-align-container");

  // Create horizontal box for month and day number
  GtkWidget *month_day_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(month_day_box, GTK_ALIGN_START);
  gtk_widget_set_hexpand(month_day_box, FALSE);

  // Create label for month name
  GtkWidget *month_label = gtk_label_new(month_name);
  gtk_widget_set_halign(month_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(month_label, "month-text");

  // Create label for day number
  GtkWidget *day_number_label = gtk_label_new(day_number);
  gtk_widget_set_halign(day_number_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(day_number_label, "day-number-text");

  // Append month and day number to horizontal box
  gtk_box_append(GTK_BOX(month_day_box), month_label);
  gtk_box_append(GTK_BOX(month_day_box), day_number_label);

  // Create label for day name - text centered, but container left-aligned to
  // match month+day
  GtkWidget *day_label = gtk_label_new(day_name);
  gtk_label_set_xalign(GTK_LABEL(day_label), 0.5); // Center text within label
  gtk_widget_set_halign(day_label, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(day_label, TRUE);
  gtk_widget_add_css_class(day_label, "day-text");

  // Append month+day box and day label to align container (both left-aligned)
  gtk_box_append(GTK_BOX(align_container), month_day_box);
  gtk_box_append(GTK_BOX(align_container), day_label);

  // Append align container to main vertical box
  gtk_box_append(GTK_BOX(vbox), align_container);

  // Create a container for weather that matches the width of align_container
  GtkWidget *weather_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(weather_container, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(weather_container, FALSE);
  gtk_widget_add_css_class(weather_container, "date-align-container");

  // Create weather module (aligned to right edge of day text, below day text)
  GtkWidget *weather_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(weather_box, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(weather_box, TRUE); // Expand to fill container width

  // Add expanding spacer to push weather content to the right
  GtkWidget *weather_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(weather_spacer, TRUE);
  gtk_box_append(GTK_BOX(weather_box), weather_spacer);

  // Add weather box to weather container
  gtk_box_append(GTK_BOX(weather_container), weather_box);

  // Create CSS for the day text, month text, and day number text
  GtkCssProvider *css_provider = gtk_css_provider_new();

  gchar *css = g_strdup_printf(
      ".transparent-day-window {"
      "  background-color: transparent;"
      "}"
      ".date-align-container {"
      "  min-width: 800px;"
      "}"
      ".day-text {"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  color: rgba(255, 255, 255, 1.0);"
      "  background-color: transparent;"
      "  letter-spacing: %dpx;"
      "  margin-top: %dpx;"
      "  margin-right: %dpx;"
      "  margin-bottom: %dpx;"
      "  margin-left: %dpx;"
      "}"
      ".month-text {"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  color: rgba(255, 255, 255, 1.0);"
      "  background-color: transparent;"
      "  letter-spacing: %dpx;"
      "  margin-top: %dpx;"
      "  margin-right: %dpx;"
      "  margin-bottom: %dpx;"
      "  margin-left: %dpx;"
      "}"
      ".day-number-text {"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  color: rgba(255, 255, 255, 1.0);"
      "  background-color: transparent;"
      "  letter-spacing: %dpx;"
      "  margin-top: %dpx;"
      "  margin-right: %dpx;"
      "  margin-bottom: %dpx;"
      "  margin-left: %dpx;"
      "}"
      ".weather-emoji {"
      "  color: rgba(255, 255, 255, 1.0);"
      "  background-color: transparent;"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  letter-spacing: %dpx;"
      "  margin-top: %dpx;"
      "  margin-right: %dpx;"
      "  margin-bottom: %dpx;"
      "  margin-left: %dpx;"
      "}"
      ".weather-temp {"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  color: rgba(255, 255, 255, 1.0);"
      "  background-color: transparent;"
      "  letter-spacing: %dpx;"
      "  margin-top: %dpx;"
      "  margin-right: %dpx;"
      "  margin-bottom: %dpx;"
      "  margin-left: %dpx;"
      "}",
      DAY_TEXT_FONT, DAY_TEXT_SIZE, DAY_TEXT_LETTER_SPACING,
      DAY_TEXT_MARGIN_TOP, DAY_TEXT_MARGIN_RIGHT, DAY_TEXT_MARGIN_BOTTOM,
      DAY_TEXT_MARGIN_LEFT, MONTH_TEXT_FONT, MONTH_TEXT_SIZE,
      MONTH_TEXT_LETTER_SPACING, MONTH_TEXT_MARGIN_TOP, MONTH_TEXT_MARGIN_RIGHT,
      MONTH_TEXT_MARGIN_BOTTOM, MONTH_TEXT_MARGIN_LEFT, DAY_NUMBER_TEXT_FONT,
      DAY_NUMBER_TEXT_SIZE, DAY_NUMBER_TEXT_LETTER_SPACING,
      DAY_NUMBER_TEXT_MARGIN_TOP, DAY_NUMBER_TEXT_MARGIN_RIGHT,
      DAY_NUMBER_TEXT_MARGIN_BOTTOM, DAY_NUMBER_TEXT_MARGIN_LEFT,
      WEATHER_EMOJI_FONT, WEATHER_EMOJI_SIZE, WEATHER_EMOJI_LETTER_SPACING,
      WEATHER_EMOJI_MARGIN_TOP, WEATHER_EMOJI_MARGIN_RIGHT,
      WEATHER_EMOJI_MARGIN_BOTTOM, WEATHER_EMOJI_MARGIN_LEFT, WEATHER_TEMP_FONT,
      WEATHER_TEMP_SIZE, WEATHER_TEMP_LETTER_SPACING, WEATHER_TEMP_MARGIN_TOP,
      WEATHER_TEMP_MARGIN_RIGHT, WEATHER_TEMP_MARGIN_BOTTOM,
      WEATHER_TEMP_MARGIN_LEFT);

  gtk_css_provider_load_from_string(css_provider, css);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free(css);
  g_object_unref(css_provider);

  // Create label for weather emoji
  GtkWidget *weather_emoji_label = gtk_label_new("");
  gtk_widget_set_halign(weather_emoji_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(weather_emoji_label, "weather-emoji");

  // Create label for weather temperature
  GtkWidget *weather_temp_label = gtk_label_new("");
  gtk_widget_set_halign(weather_temp_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(weather_temp_label, "weather-temp");

  // Append emoji and temperature to weather box
  gtk_box_append(GTK_BOX(weather_box), weather_emoji_label);
  gtk_box_append(GTK_BOX(weather_box), weather_temp_label);

  // Append weather container to main vertical box
  gtk_box_append(GTK_BOX(vbox), weather_container);

  // Initialize weather data structure
  weather_data = g_malloc0(sizeof(WeatherData));
  weather_data->emoji_widget = weather_emoji_label;
  weather_data->temp_widget = weather_temp_label;
  weather_data->should_stop = FALSE;
  weather_data->thread_running = FALSE;
  weather_data->previous_emoji = NULL;
  weather_data->previous_temp = NULL;
  weather_data->thread = NULL;
  g_mutex_init(&weather_data->mutex);
  g_cond_init(&weather_data->cond);

  // Spawn weather worker thread
  GError *error = NULL;
  weather_data->thread = g_thread_try_new(
      "weather-worker", weather_worker_thread, weather_data, &error);

  if (weather_data->thread == NULL) {
    g_printerr("Failed to create weather thread: %s\n",
               error ? error->message : "Unknown error");
    if (error)
      g_error_free(error);
  }

  gtk_window_set_child(GTK_WINDOW(day_window), vbox);
  gtk_widget_set_visible(day_window, TRUE);
}

static void activate(GtkApplication *app, gpointer user_data) {
  // Create background window
  GtkWidget *window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(window));
  gtk_layer_set_namespace(GTK_WINDOW(window), "background");
  gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_BACKGROUND);

  // Disable keyboard interactivity so it stays behind other layer shell windows
  gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

  // Anchor to all edges to cover the entire screen
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

  // Ensure no margins - background should cover entire screen
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, 0);

  // Set exclusive zone to -2 to ensure background always covers full screen
  // and doesn't respect exclusive zones from other windows
  gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -2);

  // Set background image if provided
  if (background_image_path != NULL) {
    GtkWidget *picture = gtk_picture_new_for_filename(background_image_path);
    if (picture != NULL) {
      gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_FILL);
      gtk_window_set_child(GTK_WINDOW(window), picture);
    } else {
      g_printerr("Failed to load image: %s\n", background_image_path);
    }
  }

  gtk_widget_set_visible(window, TRUE);

  // Create day text window (on layer -1, above background)
  create_day_text(app);

  // Create menu bar window
  create_menu_bar(app);
}

int main(int argc, char **argv) {
  // Parse command line arguments
  GOptionContext *context;
  GOptionEntry entries[] = {{"background-image", 'b', 0, G_OPTION_ARG_STRING,
                             &background_image_path, "Path to background image",
                             "PATH"},
                            {NULL}};

  context = g_option_context_new("- Desktop background layer shell");
  g_option_context_add_main_entries(context, entries, NULL);

  GError *error = NULL;
  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    g_printerr("Option parsing failed: %s\n", error->message);
    g_error_free(error);
    g_option_context_free(context);
    return 1;
  }

  g_option_context_free(context);

  GtkApplication *app = gtk_application_new("org.example.layer-shell",
                                            G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  // Connect shutdown signal to ensure cleanup on application termination
  g_signal_connect(app, "shutdown", G_CALLBACK(cleanup_resources), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  // Cleanup allocated resources (in case shutdown signal didn't fire)
  cleanup_resources();
  g_free(background_image_path);
  g_object_unref(app);
  return status;
}
