CC := gcc
CFLAGS := -Wall -Wextra -O2 -g
LDLIBS := -lsqlite3 -lreadline -pthread
TARGET := todo
SRC := main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)