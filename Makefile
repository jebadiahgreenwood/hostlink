# HostLink Makefile
# Builds hostlinkd (daemon) and hostlink-cli (client)

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -DNDEBUG
LDFLAGS ?=

SRCDIR  := src
BLDDIR  := build
TSTDIR  := tests

# Include paths
INCLUDES := -I$(SRCDIR)/common -I$(SRCDIR)/common/cjson

# Common sources
COMMON_SRCS := \
	$(SRCDIR)/common/cjson/cJSON.c \
	$(SRCDIR)/common/log.c \
	$(SRCDIR)/common/util.c \
	$(SRCDIR)/common/protocol.c \
	$(SRCDIR)/common/config.c

# Daemon sources
DAEMON_SRCS := \
	$(SRCDIR)/daemon/main.c \
	$(SRCDIR)/daemon/server.c \
	$(SRCDIR)/daemon/executor.c

# Client sources
CLIENT_SRCS := \
	$(SRCDIR)/client/main.c \
	$(SRCDIR)/client/connection.c

# Object files
COMMON_OBJS := $(patsubst $(SRCDIR)/%.c, $(BLDDIR)/%.o, $(COMMON_SRCS))
DAEMON_OBJS := $(patsubst $(SRCDIR)/%.c, $(BLDDIR)/%.o, $(DAEMON_SRCS))
CLIENT_OBJS := $(patsubst $(SRCDIR)/%.c, $(BLDDIR)/%.o, $(CLIENT_SRCS))

# Test sources
TEST_PROTO_SRCS := $(TSTDIR)/test_protocol.c
TEST_CFG_SRCS   := $(TSTDIR)/test_config.c

TEST_PROTO_OBJS := $(BLDDIR)/tests/test_protocol.o
TEST_CFG_OBJS   := $(BLDDIR)/tests/test_config.o

# Targets
HOSTLINKD   := $(BLDDIR)/hostlinkd
HOSTLINK_CLI := $(BLDDIR)/hostlink-cli
TEST_PROTO  := $(BLDDIR)/tests/test_protocol
TEST_CFG    := $(BLDDIR)/tests/test_config

.PHONY: all clean tests daemon client

all: daemon client

daemon: $(HOSTLINKD)

client: $(HOSTLINK_CLI)

tests: $(TEST_PROTO) $(TEST_CFG)

# Link daemon
$(HOSTLINKD): $(COMMON_OBJS) $(DAEMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

# Link client
$(HOSTLINK_CLI): $(COMMON_OBJS) $(CLIENT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

# Link test binaries
$(TEST_PROTO): $(TEST_PROTO_OBJS) $(COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_CFG): $(TEST_CFG_OBJS) $(COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile common objects
$(BLDDIR)/common/cjson/cJSON.o: $(SRCDIR)/common/cjson/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -Wno-pedantic -Wno-error=implicit-function-declaration -Wno-error=int-to-pointer-cast -Wno-error=unused-function -c -o $@ $<

$(BLDDIR)/common/%.o: $(SRCDIR)/common/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Compile daemon objects
$(BLDDIR)/daemon/%.o: $(SRCDIR)/daemon/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Compile client objects
$(BLDDIR)/client/%.o: $(SRCDIR)/client/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Compile test objects
$(BLDDIR)/tests/%.o: $(TSTDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -rf $(BLDDIR)
