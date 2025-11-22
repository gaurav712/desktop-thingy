CC = gcc
CFLAGS = -Wall -g $(shell pkg-config --cflags gtk4-layer-shell-0 gtk4)
LDFLAGS = $(shell pkg-config --libs gtk4-layer-shell-0 gtk4)

TARGET = layer-shell
SOURCE = main.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean

