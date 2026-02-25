#ifndef CONFIG_H
#define CONFIG_H

// Bar configuration
#define BAR_PADDING_HORIZONTAL 0
#define BAR_PADDING_TOP 0
#define BAR_PADDING_BOTTOM 0
#define BAR_HEIGHT 30
#define BAR_BORDER_RADIUS 0
#define BAR_BORDER_WIDTH 0.0
#define BAR_BACKGROUND_COLOR "#1D2021"
#define BAR_BORDER_COLOR "#EBDBB2"
#define BAR_BACKGROUND_OPACITY 1.0
#define BAR_TEXT_SIZE 11
#define BAR_FONT "CodeNewRoman Nerd Font"

// Day text configuration
#define DAY_TEXT_FONT "Anurati"
#define DAY_TEXT_SIZE 90
#define DAY_TEXT_LETTER_SPACING 5
#define DAY_TEXT_MARGIN_TOP 20
#define DAY_TEXT_MARGIN_RIGHT 0
#define DAY_TEXT_MARGIN_BOTTOM 0
#define DAY_TEXT_MARGIN_LEFT 0

// Month text configuration
#define MONTH_TEXT_FONT "Anurati"
#define MONTH_TEXT_SIZE 30
#define MONTH_TEXT_LETTER_SPACING 5
#define MONTH_TEXT_MARGIN_TOP 0
#define MONTH_TEXT_MARGIN_RIGHT 0
#define MONTH_TEXT_MARGIN_BOTTOM 0
#define MONTH_TEXT_MARGIN_LEFT 0

// Day number text configuration
#define DAY_NUMBER_TEXT_FONT "Computerfont"
#define DAY_NUMBER_TEXT_SIZE 36
#define DAY_NUMBER_TEXT_LETTER_SPACING 5
#define DAY_NUMBER_TEXT_MARGIN_TOP -10
#define DAY_NUMBER_TEXT_MARGIN_RIGHT 0
#define DAY_NUMBER_TEXT_MARGIN_BOTTOM 0
#define DAY_NUMBER_TEXT_MARGIN_LEFT 10
#define DATE_UPDATE_INTERVAL 60000 // 1 minute in milliseconds

// Weather text configuration
#define WEATHER_EMOJI_FONT "Apple Color Emoji"
#define WEATHER_EMOJI_SIZE 30
#define WEATHER_EMOJI_LETTER_SPACING 0
#define WEATHER_EMOJI_MARGIN_TOP 10
#define WEATHER_EMOJI_MARGIN_RIGHT 0
#define WEATHER_EMOJI_MARGIN_BOTTOM 0
#define WEATHER_EMOJI_MARGIN_LEFT 5
#define WEATHER_TEMP_FONT "Computerfont"
#define WEATHER_TEMP_SIZE 36
#define WEATHER_TEMP_LETTER_SPACING 5
#define WEATHER_TEMP_MARGIN_TOP 10
#define WEATHER_TEMP_MARGIN_RIGHT 0
#define WEATHER_TEMP_MARGIN_BOTTOM 0
#define WEATHER_TEMP_MARGIN_LEFT 5
#define WEATHER_UPDATE_INTERVAL 300000 // 5 minutes in milliseconds
#define WEATHER_EMOJI_COMMAND                                                  \
  "curl -s wttr.in/ballia?format=3 | awk '{print $2}'"
#define WEATHER_TEMP_COMMAND                                                   \
  "curl -s wttr.in/ballia?format=3 | awk '{print $3}' | cut -d \"+\" -f2"

// Bar items configuration
typedef struct {
  const char *command; // Shell command to execute, or "<separator>" for spacer
  int interval;        // Update interval in milliseconds (0 for separator)
} BarItem;

// Define the items array
static const BarItem BAR_ITEMS[] = {{"mango-tags", 300},
                                    {"mango-window-title", 300},
                                    {"<separator>", 0},
                                    {"status", 500}};

#define BAR_ITEMS_COUNT (sizeof(BAR_ITEMS) / sizeof(BAR_ITEMS[0]))

#endif // CONFIG_H
