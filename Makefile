# CinemaPlus Makefile
# Requires CEdev (CE Toolchain) installed and CEDEV env var set.

NAME = CINPLUS
DESCRIPTION = "CinemaPlus Multi-Video Player"

COMPRESSED = NO
ARCHIVED = NO

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# Link order matters: list your libs here
LIBS = msddrvce fatdrvce keypadc graphx

include $(shell cedev-config --makefile)
