# ----------------------------
# Makefile Options
# ----------------------------

NAME = CALCVID
ICON = icon.png
DESCRIPTION = "CalcVidPlayer Multi-Video Player"
COMPRESSED = YES

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# ----------------------------

include $(shell cedev-config --makefile)
