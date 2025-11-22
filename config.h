#ifndef CONFIG_H
#define CONFIG_H

// Bar configuration
#define BAR_PADDING_HORIZONTAL 5
#define BAR_PADDING_TOP 5
#define BAR_PADDING_BOTTOM 0
#define BAR_HEIGHT 30
#define BAR_BORDER_RADIUS 30
#define BAR_BORDER_WIDTH 2.0
#define BAR_BACKGROUND_COLOR "#1D2021"
#define BAR_BORDER_COLOR "#EBDBB2"
#define BAR_BACKGROUND_OPACITY 1.0
#define BAR_TEXT_SIZE 10
#define BAR_FONT "CodeNewRoman Nerd Font"

// Bar items configuration
typedef struct {
  const char *command;  // Shell command to execute, or "<separator>" for spacer
  float interval;       // Update interval in seconds (0 for separator)
} BarItem;

#define BAR_ITEMS_COUNT 4
extern const BarItem BAR_ITEMS[BAR_ITEMS_COUNT];

#endif // CONFIG_H

