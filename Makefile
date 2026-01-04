# Compiler and flags
CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -O2 -g
INCLUDES:= -Iinclude

# Binaries
SERVER  := rift-server
CLIENT  := rift-client

# Automatically find all .c files
SERVER_SRCS := $(wildcard server/*.c)
CLIENT_SRCS := $(wildcard client/*.c)

# Object files
SERVER_OBJS := $(SERVER_SRCS:.c=.o)
CLIENT_OBJS := $(CLIENT_SRCS:.c=.o)

# Default target
all: $(SERVER) $(CLIENT)

# Build server
$(SERVER): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Build client
$(CLIENT): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile .c → .o
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(SERVER) $(CLIENT) $(SERVER_OBJS) $(CLIENT_OBJS)

# Run server (example)
run-server: $(SERVER)
	./$(SERVER)

# Run client (example)
run-client: $(CLIENT)
	./$(CLIENT)

.PHONY: all clean run-server run-client
