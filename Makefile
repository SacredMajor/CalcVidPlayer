# ----------------------------
# Makefile Options
# ----------------------------

NAME = CALCVID
ICON = icon.png
DESCRIPTION = "CalcVidPlayer -- Multi-title USB video player"
COMPRESSED = YES

CFLAGS   = -Wall -Wextra -Oz -std=c2x
CXXFLAGS = -Wall -Wextra -Oz

LIBS = msddrvce keypadc graphx fileioc

# ----------------------------

include $(shell cedev-config --makefile)
