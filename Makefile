# Makefile for ADVISO 2026

CC = gcc
CFLAGS = -Wall -Wextra -O2 -march=native -fopenmp -MMD -MP
INCLUDES = -Ilibs/cfd_lib -Ilibs/mongoose
LDFLAGS = -lm -pthread

PROGNAME = adviso

OBJDIR = obj
SRCDIR = server
LIBDIR = libs

SRCS = $(SRCDIR)/main.c \
	   $(LIBDIR)/mongoose/mongoose.c

OBJS = $(SRCS:%.c=$(OBJDIR)/%.o)
DEPS = $(OBJS:.o=.d)

all: $(PROGNAME)

debug: CFLAGS = -Wall -Wextra -g -Og -march=native -fopenmp -DDEBUG -MMD -MP
debug: all

$(PROGNAME): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(PROGNAME) $(LDFLAGS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(PROGNAME)

update-deps:
	@echo "Updating mongoose..."
	@cd libs/mongoose && ./fetch_latest_mongoose.sh
	@echo "Updating cfd_lib..."
	@git submodule update --remote --merge

-include $(DEPS)

.PHONY: all clean debug update-deps
