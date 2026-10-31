CC       = gcc
CFLAGS   = -Wall -Wextra -g -Iinclude -Ithird_party
LDFLAGS  = -lpthread -lsqlite3

OUT_DIR  = out
TARGET   = $(OUT_DIR)/iot-gateway

SRCS     = $(wildcard src/*.c) $(wildcard third_party/*.c)
OBJS     = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) | $(OUT_DIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(OUT_DIR):
	mkdir -p $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)
	rm -rf $(OUT_DIR)

.PHONY: all clean