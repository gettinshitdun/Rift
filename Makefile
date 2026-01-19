CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -g -Iserver/include

SERVER_SRCS = $(wildcard server/*.c)
CLIENT_SRCS = $(wildcard client/*.c)


# Convert .c lists to .o lists
SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

# Identify the core logic files that the client "borrows" from the server directory
SHARED_OBJS = server/forward.o server/frame.o

# Binaries
SERVER_BIN = rift-server
CLIENT_BIN = rift-client

all: $(SERVER_BIN) $(CLIENT_BIN)

# Link the server (links all objects found in server/ folder)
$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Link the client (links client objects + the shared logic from server folder)
$(CLIENT_BIN): $(CLIENT_OBJS) $(SHARED_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Rule for any .o file - Includes automatic dependency tracking for .h files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run-server: $(SERVER_BIN)
	./$(SERVER_BIN)

run-client: $(CLIENT_BIN)
	./$(CLIENT_BIN) expose 3000

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) server/*.o client/*.o

.PHONY: all clean run-server run-client