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
#define BAR_TEXT_SIZE 11
#define BAR_FONT "CodeNewRoman Nerd Font"

// Day text configuration
#define DAY_TEXT_FONT "Anurati"
#define DAY_TEXT_SIZE 90
#define DAY_TEXT_LETTER_SPACING 5

// Bar items configuration
typedef struct {
  const char *command;  // Shell command to execute, or "<separator>" for spacer
  int interval;         // Update interval in milliseconds (0 for separator)
} BarItem;

// Define the items array
static const BarItem BAR_ITEMS[] = {
  {"focused=$(hyprctl activeworkspace 2>/dev/null | grep -o '[0-9]\\+' | head -1); hyprctl workspaces 2>/dev/null | awk -v f=\"$focused\" 'BEGIN {empty=\"󱓼 \"; has_windows=\"󱨈 \"; active=\"󱓻 \"} /^workspace ID/ {ws=$3} /^[[:space:]]*windows:/ {if($2>0 && ws) arr[ws]=1} END {for(i=1;i<=9;i++) {if(i==f) printf \"%s\", active; else if(arr[i]) printf \"%s\", has_windows; else printf \"%s\", empty}}'", 500 },
  { "title=$(hyprctl activewindow 2>/dev/null | grep 'title:' | sed 's/.*title: //' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'); if [ ${#title} -gt 70 ]; then title=\"${title:0:70}...\"; fi; printf \"$title\"", 500 },
  { "<separator>", 0 },
  { "status", 500 }
};

#define BAR_ITEMS_COUNT (sizeof(BAR_ITEMS) / sizeof(BAR_ITEMS[0]))

#endif // CONFIG_H

