#include "config.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Forward declaration for MonitorData (needed by UpdateData and BarItemData)
typedef struct _MonitorData MonitorData;

// Structure to hold bar item update data for main thread
typedef struct {
  int item_index;
  gchar *new_output;
  MonitorData *owner_monitor; // NULL → update all monitors; non-NULL → specific monitor only
} UpdateData;

// Structure to hold weather update data for main thread
typedef struct {
  gchar *new_emoji;
  gchar *new_temp;
} WeatherUpdateData;

// Structure to hold date update data for main thread
typedef struct {
  gchar *new_day;
  gchar *new_month;
  gchar *new_day_number;
} DateUpdateData;

// Structure to hold bar item worker state (identified by index, no widget ptr)
typedef struct {
  int item_index;
  gchar *command;           // owned (g_strdup'd)
  int interval;
  GThread *thread;
  gboolean should_stop;
  gboolean thread_running;
  GMutex mutex;
  GCond cond;
  gchar *previous_output;
  MonitorData *owner_monitor; // non-NULL → UpdateData targets only this monitor
} BarItemData;

static gchar *background_image_path = NULL;

// Structure to hold weather worker state
typedef struct {
  GThread *thread;
  gboolean should_stop;
  gboolean thread_running;
  GMutex mutex;
  GCond cond;
  gchar *previous_emoji;
  gchar *previous_temp;
} WeatherData;

static WeatherData *weather_data = NULL;

// Structure to hold date worker state
typedef struct {
  GThread *thread;
  gboolean should_stop;
  gboolean thread_running;
  GMutex mutex;
  GCond cond;
  gchar *previous_day;
  gchar *previous_month;
  gchar *previous_day_number;
} DateData;

static DateData *date_data = NULL;

// Per-monitor widget state (forward declared at top for mutual references)
struct _MonitorData {
  GdkMonitor *monitor;
  GtkWidget  *bg_window;
  GtkWidget  *day_window;
  GtkWidget  *bar_window;
  GtkWidget  *day_label, *month_label, *day_number_label;
  GtkWidget  *weather_emoji_label, *weather_temp_label;
  GtkWidget **bar_item_widgets; // g_malloc0'd, length BAR_ITEMS_COUNT
  BarItemData *bar_items_data;  // allocated per-monitor, length BAR_ITEMS_COUNT
};

static GList   *g_monitors = NULL;
static GMutex   g_monitors_mutex;
static gboolean g_monitors_mutex_initialized = FALSE;

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

// Idle callback: update bar item label on one or all monitors
static gboolean update_ui_from_main_thread(gpointer user_data) {
  UpdateData *ud = (UpdateData *)user_data;

  g_mutex_lock(&g_monitors_mutex);
  if (ud->owner_monitor != NULL) {
    MonitorData *md = ud->owner_monitor;
    if (md->bar_item_widgets && md->bar_item_widgets[ud->item_index])
      gtk_label_set_text(GTK_LABEL(md->bar_item_widgets[ud->item_index]), ud->new_output);
  } else {
    for (GList *l = g_monitors; l; l = l->next) {
      MonitorData *md = l->data;
      if (md->bar_item_widgets != NULL &&
          md->bar_item_widgets[ud->item_index] != NULL) {
        gtk_label_set_text(GTK_LABEL(md->bar_item_widgets[ud->item_index]),
                           ud->new_output);
      }
    }
  }
  g_mutex_unlock(&g_monitors_mutex);

  g_free(ud->new_output);
  g_free(ud);
  return G_SOURCE_REMOVE;
}

// Idle callback: update weather labels on all monitors
static gboolean update_weather_ui_from_main_thread(gpointer user_data) {
  WeatherUpdateData *ud = (WeatherUpdateData *)user_data;

  g_mutex_lock(&g_monitors_mutex);
  for (GList *l = g_monitors; l; l = l->next) {
    MonitorData *md = l->data;
    if (ud->new_emoji != NULL && md->weather_emoji_label != NULL) {
      gtk_label_set_text(GTK_LABEL(md->weather_emoji_label), ud->new_emoji);
    }
    if (ud->new_temp != NULL && md->weather_temp_label != NULL) {
      gtk_label_set_text(GTK_LABEL(md->weather_temp_label), ud->new_temp);
    }
  }
  g_mutex_unlock(&g_monitors_mutex);

  g_free(ud->new_emoji);
  g_free(ud->new_temp);
  g_free(ud);
  return G_SOURCE_REMOVE;
}

// Idle callback: update date labels on all monitors
static gboolean update_date_ui_from_main_thread(gpointer user_data) {
  DateUpdateData *ud = (DateUpdateData *)user_data;

  g_mutex_lock(&g_monitors_mutex);
  for (GList *l = g_monitors; l; l = l->next) {
    MonitorData *md = l->data;
    if (ud->new_day != NULL && md->day_label != NULL) {
      gtk_label_set_text(GTK_LABEL(md->day_label), ud->new_day);
    }
    if (ud->new_month != NULL && md->month_label != NULL) {
      gtk_label_set_text(GTK_LABEL(md->month_label), ud->new_month);
    }
    if (ud->new_day_number != NULL && md->day_number_label != NULL) {
      gtk_label_set_text(GTK_LABEL(md->day_number_label), ud->new_day_number);
    }
  }
  g_mutex_unlock(&g_monitors_mutex);

  g_free(ud->new_day);
  g_free(ud->new_month);
  g_free(ud->new_day_number);
  g_free(ud);
  return G_SOURCE_REMOVE;
}

// Worker thread: polls at interval, queues label updates to all monitors on change
static gpointer module_worker_thread(gpointer user_data) {
  BarItemData *item_data = (BarItemData *)user_data;

  g_mutex_lock(&item_data->mutex);
  item_data->thread_running = TRUE;
  g_mutex_unlock(&item_data->mutex);

  // Initial update
  gchar *output = execute_command(item_data->command);
  g_mutex_lock(&item_data->mutex);
  item_data->previous_output = output ? g_strdup(output) : g_strdup("");
  g_mutex_unlock(&item_data->mutex);

  UpdateData *update_data = g_malloc(sizeof(UpdateData));
  update_data->item_index = item_data->item_index;
  update_data->new_output = output ? g_strdup(output) : g_strdup("");
  update_data->owner_monitor = item_data->owner_monitor;
  g_idle_add(update_ui_from_main_thread, update_data);

  if (output != NULL)
    g_free(output);

  // Poll at module's interval
  while (TRUE) {
    g_mutex_lock(&item_data->mutex);

    gint64 end_time =
        g_get_monotonic_time() + (item_data->interval * 1000);

    while (!item_data->should_stop) {
      if (!g_cond_wait_until(&item_data->cond, &item_data->mutex, end_time))
        break;
      if (item_data->should_stop)
        break;
    }

    if (item_data->should_stop) {
      g_mutex_unlock(&item_data->mutex);
      break;
    }
    g_mutex_unlock(&item_data->mutex);

    output = execute_command(item_data->command);

    g_mutex_lock(&item_data->mutex);

    const char *current_output = (output == NULL) ? "" : output;
    const char *previous_output =
        (item_data->previous_output == NULL) ? "" : item_data->previous_output;

    gboolean current_is_blank = (strlen(current_output) == 0);
    gboolean previous_is_blank = (strlen(previous_output) == 0);

    gboolean should_update = FALSE;
    if (current_is_blank && previous_is_blank) {
      should_update = FALSE;
    } else {
      should_update = (strcmp(previous_output, current_output) != 0);
    }

    if (should_update) {
      UpdateData *ud = g_malloc(sizeof(UpdateData));
      ud->item_index = item_data->item_index;
      ud->new_output = output ? g_strdup(output) : g_strdup("");
      ud->owner_monitor = item_data->owner_monitor;

      g_free(item_data->previous_output);
      item_data->previous_output = output ? g_strdup(output) : g_strdup("");

      g_mutex_unlock(&item_data->mutex);
      g_idle_add(update_ui_from_main_thread, ud);
    } else {
      g_mutex_unlock(&item_data->mutex);
    }

    if (output != NULL)
      g_free(output);
  }

  g_mutex_lock(&item_data->mutex);
  item_data->thread_running = FALSE;
  g_cond_signal(&item_data->cond);
  g_mutex_unlock(&item_data->mutex);

  return NULL;
}

// Weather worker thread
static gpointer weather_worker_thread(gpointer user_data) {
  WeatherData *wdata = (WeatherData *)user_data;

  g_mutex_lock(&wdata->mutex);
  wdata->thread_running = TRUE;
  g_mutex_unlock(&wdata->mutex);

  // Initial update
  gchar *emoji = execute_command(WEATHER_EMOJI_COMMAND);
  gchar *temp = execute_command(WEATHER_TEMP_COMMAND);

  if (emoji != NULL || temp != NULL) {
    g_mutex_lock(&wdata->mutex);
    if (emoji != NULL)
      wdata->previous_emoji = g_strdup(emoji);
    if (temp != NULL)
      wdata->previous_temp = g_strdup(temp);
    g_mutex_unlock(&wdata->mutex);

    WeatherUpdateData *ud = g_malloc(sizeof(WeatherUpdateData));
    ud->new_emoji = emoji ? g_strdup(emoji) : NULL;
    ud->new_temp = temp ? g_strdup(temp) : NULL;
    g_idle_add(update_weather_ui_from_main_thread, ud);

    g_free(emoji);
    g_free(temp);
  }

  while (TRUE) {
    g_mutex_lock(&wdata->mutex);

    gint64 end_time =
        g_get_monotonic_time() + (WEATHER_UPDATE_INTERVAL * 1000);

    while (!wdata->should_stop) {
      if (!g_cond_wait_until(&wdata->cond, &wdata->mutex, end_time))
        break;
      if (wdata->should_stop)
        break;
    }

    if (wdata->should_stop) {
      g_mutex_unlock(&wdata->mutex);
      break;
    }
    g_mutex_unlock(&wdata->mutex);

    emoji = execute_command(WEATHER_EMOJI_COMMAND);
    temp = execute_command(WEATHER_TEMP_COMMAND);

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
        WeatherUpdateData *ud = g_malloc(sizeof(WeatherUpdateData));
        ud->new_emoji = emoji_changed && emoji ? g_strdup(emoji) : NULL;
        ud->new_temp = temp_changed && temp ? g_strdup(temp) : NULL;
        g_mutex_unlock(&wdata->mutex);
        g_idle_add(update_weather_ui_from_main_thread, ud);
      } else {
        g_mutex_unlock(&wdata->mutex);
      }

      g_free(emoji);
      g_free(temp);
    }
  }

  g_mutex_lock(&wdata->mutex);
  wdata->thread_running = FALSE;
  g_cond_signal(&wdata->cond);
  g_mutex_unlock(&wdata->mutex);

  return NULL;
}

// Date worker thread
static gpointer date_worker_thread(gpointer user_data) {
  DateData *ddata = (DateData *)user_data;

  g_mutex_lock(&ddata->mutex);
  ddata->thread_running = TRUE;
  g_mutex_unlock(&ddata->mutex);

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

  for (int i = 0; day_name[i]; i++)
    day_name[i] = g_ascii_toupper(day_name[i]);
  for (int i = 0; month_name[i]; i++)
    month_name[i] = g_ascii_toupper(month_name[i]);

  gchar *day = g_strdup(day_name);
  gchar *month = g_strdup(month_name);
  gchar *day_num = g_strdup(day_number);

  if (day != NULL || month != NULL || day_num != NULL) {
    g_mutex_lock(&ddata->mutex);
    if (day != NULL)
      ddata->previous_day = g_strdup(day);
    if (month != NULL)
      ddata->previous_month = g_strdup(month);
    if (day_num != NULL)
      ddata->previous_day_number = g_strdup(day_num);
    g_mutex_unlock(&ddata->mutex);

    DateUpdateData *ud = g_malloc(sizeof(DateUpdateData));
    ud->new_day = day ? g_strdup(day) : NULL;
    ud->new_month = month ? g_strdup(month) : NULL;
    ud->new_day_number = day_num ? g_strdup(day_num) : NULL;
    g_idle_add(update_date_ui_from_main_thread, ud);

    g_free(day);
    g_free(month);
    g_free(day_num);
  }

  while (TRUE) {
    g_mutex_lock(&ddata->mutex);

    gint64 end_time =
        g_get_monotonic_time() + (DATE_UPDATE_INTERVAL * 1000);

    while (!ddata->should_stop) {
      if (!g_cond_wait_until(&ddata->cond, &ddata->mutex, end_time))
        break;
      if (ddata->should_stop)
        break;
    }

    if (ddata->should_stop) {
      g_mutex_unlock(&ddata->mutex);
      break;
    }
    g_mutex_unlock(&ddata->mutex);

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(day_name, sizeof(day_name), "%A", timeinfo);
    strftime(month_name, sizeof(month_name), "%B", timeinfo);
    strftime(day_number, sizeof(day_number), "%d", timeinfo);

    for (int i = 0; day_name[i]; i++)
      day_name[i] = g_ascii_toupper(day_name[i]);
    for (int i = 0; month_name[i]; i++)
      month_name[i] = g_ascii_toupper(month_name[i]);

    day = g_strdup(day_name);
    month = g_strdup(month_name);
    day_num = g_strdup(day_number);

    if (day != NULL || month != NULL || day_num != NULL) {
      g_mutex_lock(&ddata->mutex);

      gboolean day_changed = FALSE;
      gboolean month_changed = FALSE;
      gboolean day_number_changed = FALSE;

      if (day != NULL && (ddata->previous_day == NULL ||
                          strcmp(ddata->previous_day, day) != 0)) {
        day_changed = TRUE;
        g_free(ddata->previous_day);
        ddata->previous_day = g_strdup(day);
      }

      if (month != NULL && (ddata->previous_month == NULL ||
                            strcmp(ddata->previous_month, month) != 0)) {
        month_changed = TRUE;
        g_free(ddata->previous_month);
        ddata->previous_month = g_strdup(month);
      }

      if (day_num != NULL &&
          (ddata->previous_day_number == NULL ||
           strcmp(ddata->previous_day_number, day_num) != 0)) {
        day_number_changed = TRUE;
        g_free(ddata->previous_day_number);
        ddata->previous_day_number = g_strdup(day_num);
      }

      if (day_changed || month_changed || day_number_changed) {
        DateUpdateData *ud = g_malloc(sizeof(DateUpdateData));
        ud->new_day = day_changed && day ? g_strdup(day) : NULL;
        ud->new_month = month_changed && month ? g_strdup(month) : NULL;
        ud->new_day_number =
            day_number_changed && day_num ? g_strdup(day_num) : NULL;
        g_mutex_unlock(&ddata->mutex);
        g_idle_add(update_date_ui_from_main_thread, ud);
      } else {
        g_mutex_unlock(&ddata->mutex);
      }

      g_free(day);
      g_free(month);
      g_free(day_num);
    }
  }

  g_mutex_lock(&ddata->mutex);
  ddata->thread_running = FALSE;
  g_cond_signal(&ddata->cond);
  g_mutex_unlock(&ddata->mutex);

  return NULL;
}

// Initialize CSS once (bar + day-text styles)
static void init_css(void) {
  // Bar CSS
  GtkCssProvider *bar_provider = gtk_css_provider_new();
  guint bg_r, bg_g, bg_b;
  guint border_r, border_g, border_b;
  sscanf(BAR_BACKGROUND_COLOR, "#%02x%02x%02x", &bg_r, &bg_g, &bg_b);
  sscanf(BAR_BORDER_COLOR, "#%02x%02x%02x", &border_r, &border_g, &border_b);

  gchar *bar_css = g_strdup_printf(
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
      BAR_FONT, BAR_TEXT_SIZE, BAR_FONT, BAR_TEXT_SIZE);

  gtk_css_provider_load_from_string(bar_provider, bar_css);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(bar_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free(bar_css);
  g_object_unref(bar_provider);

  // Day-text + weather CSS
  GtkCssProvider *day_provider = gtk_css_provider_new();
  gchar *day_css = g_strdup_printf(
      ".transparent-day-window {"
      "  background-color: transparent;"
      "}"
      ".date-align-container {}"
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

  gtk_css_provider_load_from_string(day_provider, day_css);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(day_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free(day_css);
  g_object_unref(day_provider);
}

// Create background window pinned to a specific monitor
static GtkWidget *create_background_window(GtkApplication *app,
                                           GdkMonitor *monitor) {
  GtkWidget *window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(window));
  gtk_layer_set_namespace(GTK_WINDOW(window), "background");
  gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_BACKGROUND);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  gtk_layer_set_monitor(GTK_WINDOW(window), monitor);

  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, 0);

  gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -2);

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
  return window;
}

// Create day-text window pinned to a specific monitor; fills md's label ptrs
static GtkWidget *create_day_text_window(GtkApplication *app,
                                         GdkMonitor *monitor,
                                         MonitorData *md) {
  GtkWidget *day_window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(day_window));
  gtk_layer_set_namespace(GTK_WINDOW(day_window), "day-text");
  gtk_layer_set_layer(GTK_WINDOW(day_window), GTK_LAYER_SHELL_LAYER_BACKGROUND);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(day_window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  gtk_layer_set_monitor(GTK_WINDOW(day_window), monitor);

  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                       TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_LEFT,
                       TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_RIGHT,
                       TRUE);

  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin(GTK_WINDOW(day_window), GTK_LAYER_SHELL_EDGE_RIGHT, 0);

  gtk_layer_set_exclusive_zone(GTK_WINDOW(day_window), -1);
  gtk_widget_add_css_class(GTK_WIDGET(day_window), "transparent-day-window");

  // Layout
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

  GtkWidget *align_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(align_container, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(align_container, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(align_container, FALSE);
  gtk_widget_add_css_class(align_container, "date-align-container");

  GtkWidget *month_day_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(month_day_box, GTK_ALIGN_START);
  gtk_widget_set_hexpand(month_day_box, FALSE);

  GtkWidget *month_label = gtk_label_new("");
  gtk_widget_set_halign(month_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(month_label, "month-text");

  GtkWidget *day_number_label = gtk_label_new("");
  gtk_widget_set_halign(day_number_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(day_number_label, "day-number-text");

  gtk_box_append(GTK_BOX(month_day_box), month_label);
  gtk_box_append(GTK_BOX(month_day_box), day_number_label);

  GtkWidget *day_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(day_label), 0.5);
  gtk_widget_set_halign(day_label, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(day_label, TRUE);
  gtk_widget_add_css_class(day_label, "day-text");

  gtk_box_append(GTK_BOX(align_container), month_day_box);
  gtk_box_append(GTK_BOX(align_container), day_label);
  gtk_box_append(GTK_BOX(vbox), align_container);

  GtkWidget *weather_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(weather_container, GTK_ALIGN_END);
  gtk_widget_set_hexpand(weather_container, TRUE);
  gtk_widget_add_css_class(weather_container, "date-align-container");

  GtkWidget *weather_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  GtkWidget *weather_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(weather_spacer, TRUE);
  gtk_box_append(GTK_BOX(weather_box), weather_spacer);

  gtk_box_append(GTK_BOX(weather_container), weather_box);

  GtkWidget *weather_emoji_label = gtk_label_new("");
  gtk_widget_set_halign(weather_emoji_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(weather_emoji_label, "weather-emoji");

  GtkWidget *weather_temp_label = gtk_label_new("");
  gtk_widget_set_halign(weather_temp_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(weather_temp_label, "weather-temp");

  gtk_box_append(GTK_BOX(weather_box), weather_emoji_label);
  gtk_box_append(GTK_BOX(weather_box), weather_temp_label);
  gtk_box_append(GTK_BOX(vbox), weather_container);

  // Store label pointers in MonitorData
  md->day_label = day_label;
  md->month_label = month_label;
  md->day_number_label = day_number_label;
  md->weather_emoji_label = weather_emoji_label;
  md->weather_temp_label = weather_temp_label;

  gtk_window_set_child(GTK_WINDOW(day_window), vbox);
  gtk_widget_set_visible(day_window, TRUE);
  return day_window;
}

// Create bar window pinned to a specific monitor; fills md->bar_item_widgets
static GtkWidget *create_bar_window(GtkApplication *app, GdkMonitor *monitor,
                                    MonitorData *md) {
  GtkWidget *menu_window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(menu_window));
  gtk_layer_set_namespace(GTK_WINDOW(menu_window), "bar");
  gtk_layer_set_layer(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(menu_window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  gtk_layer_set_monitor(GTK_WINDOW(menu_window), monitor);

  gtk_layer_set_anchor(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_TOP,
                       TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_LEFT,
                       TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_RIGHT,
                       TRUE);

  gtk_layer_set_margin(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_TOP,
                       BAR_PADDING_TOP);
  gtk_layer_set_margin(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_LEFT,
                       BAR_PADDING_HORIZONTAL);
  gtk_layer_set_margin(GTK_WINDOW(menu_window), GTK_LAYER_SHELL_EDGE_RIGHT,
                       BAR_PADDING_HORIZONTAL);

  gtk_layer_set_exclusive_zone(GTK_WINDOW(menu_window),
                               BAR_HEIGHT + BAR_PADDING_TOP + BAR_PADDING_BOTTOM);
  gtk_widget_add_css_class(GTK_WIDGET(menu_window), "transparent-window");

  GtkWidget *outer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(outer_box, TRUE);
  gtk_widget_set_halign(outer_box, GTK_ALIGN_FILL);

  GtkWidget *bar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_size_request(bar_box, -1, BAR_HEIGHT);
  gtk_widget_set_vexpand(bar_box, FALSE);
  gtk_widget_set_hexpand(bar_box, TRUE);
  gtk_widget_set_halign(bar_box, GTK_ALIGN_FILL);
  gtk_widget_set_valign(bar_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(bar_box, "bar");

  // Add bar items; store label widgets in md->bar_item_widgets
  for (size_t i = 0; i < BAR_ITEMS_COUNT; i++) {
    const BarItem *item = &BAR_ITEMS[i];

    if (strcmp(item->command, "<separator>") == 0) {
      GtkWidget *separator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand(separator, TRUE);
      gtk_widget_set_halign(separator, GTK_ALIGN_FILL);
      gtk_box_append(GTK_BOX(bar_box), separator);
      // md->bar_item_widgets[i] stays NULL (g_malloc0'd)
    } else {
      GtkWidget *label = gtk_label_new("");
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      md->bar_item_widgets[i] = label;
      gtk_box_append(GTK_BOX(bar_box), label);
    }
  }

  gtk_box_append(GTK_BOX(outer_box), bar_box);
  gtk_window_set_child(GTK_WINDOW(menu_window), outer_box);
  gtk_widget_set_visible(menu_window, TRUE);
  return menu_window;
}

// Seed a newly-created monitor's labels with current worker thread state
static void seed_monitor_labels(MonitorData *md) {
  // Bar item labels
  if (md->bar_items_data != NULL && md->bar_item_widgets != NULL) {
    for (size_t i = 0; i < BAR_ITEMS_COUNT; i++) {
      BarItemData *item_data = &md->bar_items_data[i];
      if (item_data->command == NULL ||
          strcmp(item_data->command, "<separator>") == 0)
        continue;
      if (md->bar_item_widgets[i] == NULL)
        continue;
      g_mutex_lock(&item_data->mutex);
      gchar *prev = item_data->previous_output
                        ? g_strdup(item_data->previous_output)
                        : NULL;
      g_mutex_unlock(&item_data->mutex);
      if (prev != NULL) {
        gtk_label_set_text(GTK_LABEL(md->bar_item_widgets[i]), prev);
        g_free(prev);
      }
    }
  }

  // Weather labels
  if (weather_data != NULL) {
    g_mutex_lock(&weather_data->mutex);
    gchar *emoji = weather_data->previous_emoji
                       ? g_strdup(weather_data->previous_emoji)
                       : NULL;
    gchar *temp = weather_data->previous_temp
                      ? g_strdup(weather_data->previous_temp)
                      : NULL;
    g_mutex_unlock(&weather_data->mutex);
    if (emoji != NULL && md->weather_emoji_label != NULL)
      gtk_label_set_text(GTK_LABEL(md->weather_emoji_label), emoji);
    if (temp != NULL && md->weather_temp_label != NULL)
      gtk_label_set_text(GTK_LABEL(md->weather_temp_label), temp);
    g_free(emoji);
    g_free(temp);
  }

  // Date labels
  if (date_data != NULL) {
    g_mutex_lock(&date_data->mutex);
    gchar *day =
        date_data->previous_day ? g_strdup(date_data->previous_day) : NULL;
    gchar *month =
        date_data->previous_month ? g_strdup(date_data->previous_month) : NULL;
    gchar *day_num = date_data->previous_day_number
                         ? g_strdup(date_data->previous_day_number)
                         : NULL;
    g_mutex_unlock(&date_data->mutex);
    if (day != NULL && md->day_label != NULL)
      gtk_label_set_text(GTK_LABEL(md->day_label), day);
    if (month != NULL && md->month_label != NULL)
      gtk_label_set_text(GTK_LABEL(md->month_label), month);
    if (day_num != NULL && md->day_number_label != NULL)
      gtk_label_set_text(GTK_LABEL(md->day_number_label), day_num);
    g_free(day);
    g_free(month);
    g_free(day_num);
  }
}

// Stop and join bar worker threads for a single monitor, free bar_items_data
static void stop_bar_workers(MonitorData *md) {
  if (md->bar_items_data == NULL) return;
  for (size_t i = 0; i < BAR_ITEMS_COUNT; i++) {
    BarItemData *item_data = &md->bar_items_data[i];
    g_mutex_lock(&item_data->mutex);
    GThread *thread = item_data->thread;
    if (thread) {
      item_data->should_stop = TRUE;
      g_cond_signal(&item_data->cond);
      item_data->thread = NULL;
    }
    g_mutex_unlock(&item_data->mutex);
    if (thread) {
      gint timeout = 50;
      while (timeout-- > 0) {
        g_mutex_lock(&item_data->mutex);
        gboolean running = item_data->thread_running;
        g_mutex_unlock(&item_data->mutex);
        if (!running) break;
        g_usleep(100000);
      }
      g_thread_join(thread);
    }
    if (item_data->command && strcmp(item_data->command, "<separator>") != 0) {
      g_cond_clear(&item_data->cond);
      g_mutex_clear(&item_data->mutex);
      g_free(item_data->previous_output);
      item_data->previous_output = NULL;
    }
    g_free(item_data->command);
    item_data->command = NULL;
  }
  g_free(md->bar_items_data);
  md->bar_items_data = NULL;
}

// Null widget ptrs, destroy windows, remove from g_monitors, free md
static void teardown_monitor(MonitorData *md) {
  // Phase 1: NULL all widget pointers under mutex (crash guard for idle callbacks)
  g_mutex_lock(&g_monitors_mutex);
  md->day_label = NULL;
  md->month_label = NULL;
  md->day_number_label = NULL;
  md->weather_emoji_label = NULL;
  md->weather_temp_label = NULL;
  if (md->bar_item_widgets != NULL) {
    for (size_t i = 0; i < BAR_ITEMS_COUNT; i++)
      md->bar_item_widgets[i] = NULL;
  }
  g_mutex_unlock(&g_monitors_mutex);

  // Phase 2: Destroy windows
  if (md->bar_window != NULL) {
    gtk_window_destroy(GTK_WINDOW(md->bar_window));
    md->bar_window = NULL;
  }
  if (md->day_window != NULL) {
    gtk_window_destroy(GTK_WINDOW(md->day_window));
    md->day_window = NULL;
  }
  if (md->bg_window != NULL) {
    gtk_window_destroy(GTK_WINDOW(md->bg_window));
    md->bg_window = NULL;
  }

  // Phase 3: Remove from list and free
  g_mutex_lock(&g_monitors_mutex);
  g_monitors = g_list_remove(g_monitors, md);
  g_mutex_unlock(&g_monitors_mutex);

  stop_bar_workers(md); // safe to call even if already stopped (NULL-check inside)
  g_free(md->bar_item_widgets);
  g_free(md);
}

// Forward declaration (defined after setup_monitor)
static void init_bar_workers(MonitorData *md, const gchar *connector);

// Create all three windows for a monitor and register it in g_monitors
static MonitorData *setup_monitor(GtkApplication *app, GdkMonitor *monitor) {
  MonitorData *md = g_malloc0(sizeof(MonitorData));
  md->monitor = monitor;
  md->bar_item_widgets = g_malloc0(sizeof(GtkWidget *) * BAR_ITEMS_COUNT);

  const gchar *connector = gdk_monitor_get_connector(monitor);

  md->bg_window = create_background_window(app, monitor);
  md->day_window = create_day_text_window(app, monitor, md);
  md->bar_window = create_bar_window(app, monitor, md);

  seed_monitor_labels(md);

  init_bar_workers(md, connector);

  g_mutex_lock(&g_monitors_mutex);
  g_monitors = g_list_append(g_monitors, md);
  g_mutex_unlock(&g_monitors_mutex);

  return md;
}

// Reconcile g_monitors against current GListModel on hotplug events
static void on_monitors_changed(GListModel *model, guint position,
                                guint removed, guint added,
                                gpointer user_data) {
  (void)position;
  (void)removed;
  (void)added;

  GtkApplication *app = GTK_APPLICATION(user_data);
  guint n = g_list_model_get_n_items(model);

  // Find tracked monitors no longer in the model
  GList *to_remove = NULL;
  g_mutex_lock(&g_monitors_mutex);
  for (GList *l = g_monitors; l; l = l->next) {
    MonitorData *md = l->data;
    gboolean found = FALSE;
    for (guint i = 0; i < n; i++) {
      GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(model, i));
      if (m == md->monitor) {
        found = TRUE;
        g_object_unref(m);
        break;
      }
      g_object_unref(m);
    }
    if (!found)
      to_remove = g_list_append(to_remove, md);
  }
  g_mutex_unlock(&g_monitors_mutex);

  for (GList *l = to_remove; l; l = l->next)
    teardown_monitor((MonitorData *)l->data);
  g_list_free(to_remove);

  // Find monitors in the model not yet tracked
  for (guint i = 0; i < n; i++) {
    GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(model, i));
    gboolean found = FALSE;
    g_mutex_lock(&g_monitors_mutex);
    for (GList *l = g_monitors; l; l = l->next) {
      if (((MonitorData *)l->data)->monitor == m) {
        found = TRUE;
        break;
      }
    }
    g_mutex_unlock(&g_monitors_mutex);
    if (!found)
      setup_monitor(app, m);
    g_object_unref(m);
  }
}

// Allocate per-monitor bar_items_data and start one worker thread per non-separator item
static void init_bar_workers(MonitorData *md, const gchar *connector) {
  md->bar_items_data = g_malloc0(sizeof(BarItemData) * BAR_ITEMS_COUNT);

  for (size_t i = 0; i < BAR_ITEMS_COUNT; i++) {
    const BarItem *item = &BAR_ITEMS[i];
    BarItemData *item_data = &md->bar_items_data[i];
    item_data->item_index = (int)i;
    item_data->interval = item->interval;
    item_data->owner_monitor = item->pass_monitor_name ? md : NULL;

    if (item->pass_monitor_name && connector)
      item_data->command = g_strdup_printf("%s %s", item->command, connector);
    else
      item_data->command = g_strdup(item->command);

    if (strcmp(item->command, "<separator>") == 0)
      continue;

    item_data->should_stop = FALSE;
    item_data->thread_running = FALSE;
    item_data->previous_output = NULL;
    item_data->thread = NULL;
    g_mutex_init(&item_data->mutex);
    g_cond_init(&item_data->cond);

    if (item->interval > 0) {
      GError *error = NULL;
      item_data->thread = g_thread_try_new(
          "module-worker", module_worker_thread, item_data, &error);
      if (!item_data->thread) {
        g_printerr("Failed to create thread for module %zu: %s\n", i,
                   error ? error->message : "Unknown error");
        if (error) g_error_free(error);
      }
    }
  }
}

// Allocate date_data and start the date worker thread
static void init_date_worker(void) {
  date_data = g_malloc0(sizeof(DateData));
  date_data->should_stop = FALSE;
  date_data->thread_running = FALSE;
  date_data->previous_day = NULL;
  date_data->previous_month = NULL;
  date_data->previous_day_number = NULL;
  date_data->thread = NULL;
  g_mutex_init(&date_data->mutex);
  g_cond_init(&date_data->cond);

  GError *error = NULL;
  date_data->thread =
      g_thread_try_new("date-worker", date_worker_thread, date_data, &error);
  if (date_data->thread == NULL) {
    g_printerr("Failed to create date thread: %s\n",
               error ? error->message : "Unknown error");
    if (error)
      g_error_free(error);
  }
}

// Allocate weather_data and start the weather worker thread
static void init_weather_worker(void) {
  weather_data = g_malloc0(sizeof(WeatherData));
  weather_data->should_stop = FALSE;
  weather_data->thread_running = FALSE;
  weather_data->previous_emoji = NULL;
  weather_data->previous_temp = NULL;
  weather_data->thread = NULL;
  g_mutex_init(&weather_data->mutex);
  g_cond_init(&weather_data->cond);

  GError *error = NULL;
  weather_data->thread = g_thread_try_new(
      "weather-worker", weather_worker_thread, weather_data, &error);
  if (weather_data->thread == NULL) {
    g_printerr("Failed to create weather thread: %s\n",
               error ? error->message : "Unknown error");
    if (error)
      g_error_free(error);
  }
}

// Cleanup: Phase 1 stop/join all threads; Phase 2 teardown all monitors
static void cleanup_resources(void) {
  // Phase 1: Stop bar workers on all monitors
  g_mutex_lock(&g_monitors_mutex);
  GList *monitors_snapshot = g_list_copy(g_monitors);
  g_mutex_unlock(&g_monitors_mutex);
  for (GList *l = monitors_snapshot; l; l = l->next)
    stop_bar_workers((MonitorData *)l->data);
  g_list_free(monitors_snapshot);

  // Stop and join weather thread
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
      gint timeout = 50;
      while (timeout > 0) {
        g_mutex_lock(&weather_data->mutex);
        gboolean running = weather_data->thread_running;
        g_mutex_unlock(&weather_data->mutex);
        if (!running)
          break;
        g_usleep(100000);
        timeout--;
      }
      g_thread_join(thread);
    }

    g_cond_clear(&weather_data->cond);
    g_mutex_clear(&weather_data->mutex);
    if (weather_data->previous_emoji != NULL) {
      g_free(weather_data->previous_emoji);
      weather_data->previous_emoji = NULL;
    }
    if (weather_data->previous_temp != NULL) {
      g_free(weather_data->previous_temp);
      weather_data->previous_temp = NULL;
    }
    g_free(weather_data);
    weather_data = NULL;
  }

  // Stop and join date thread
  if (date_data != NULL) {
    g_mutex_lock(&date_data->mutex);
    GThread *thread = date_data->thread;
    if (thread != NULL) {
      date_data->should_stop = TRUE;
      g_cond_signal(&date_data->cond);
      date_data->thread = NULL;
    }
    g_mutex_unlock(&date_data->mutex);

    if (thread != NULL) {
      gint timeout = 50;
      while (timeout > 0) {
        g_mutex_lock(&date_data->mutex);
        gboolean running = date_data->thread_running;
        g_mutex_unlock(&date_data->mutex);
        if (!running)
          break;
        g_usleep(100000);
        timeout--;
      }
      g_thread_join(thread);
    }

    g_cond_clear(&date_data->cond);
    g_mutex_clear(&date_data->mutex);
    if (date_data->previous_day != NULL) {
      g_free(date_data->previous_day);
      date_data->previous_day = NULL;
    }
    if (date_data->previous_month != NULL) {
      g_free(date_data->previous_month);
      date_data->previous_month = NULL;
    }
    if (date_data->previous_day_number != NULL) {
      g_free(date_data->previous_day_number);
      date_data->previous_day_number = NULL;
    }
    g_free(date_data);
    date_data = NULL;
  }

  // Phase 2: Teardown all monitors (threads fully stopped, no idle callbacks
  // in flight)
  while (g_monitors != NULL)
    teardown_monitor((MonitorData *)g_monitors->data);

  if (g_monitors_mutex_initialized) {
    g_mutex_clear(&g_monitors_mutex);
    g_monitors_mutex_initialized = FALSE;
  }
}

static void activate(GtkApplication *app) {
  g_mutex_init(&g_monitors_mutex);
  g_monitors_mutex_initialized = TRUE;

  init_css();
  init_date_worker();
  init_weather_worker();

  GdkDisplay *display = gdk_display_get_default();
  GListModel *mon_list = gdk_display_get_monitors(display);
  g_signal_connect(mon_list, "items-changed",
                   G_CALLBACK(on_monitors_changed), app);

  guint n = g_list_model_get_n_items(mon_list);
  for (guint i = 0; i < n; i++) {
    GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(mon_list, i));
    setup_monitor(app, m);
    g_object_unref(m);
  }
}

int main(int argc, char **argv) {
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
  g_signal_connect(app, "shutdown", G_CALLBACK(cleanup_resources), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  // Cleanup in case shutdown signal didn't fire
  cleanup_resources();
  g_free(background_image_path);
  g_object_unref(app);
  return status;
}
