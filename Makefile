#-------------------------------------------------------------------------
#
#  Copyright (c) 2020 Rajit Manohar
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin Street, Fifth Floor,
#  Boston, MA  02110-1301, USA.
#
#-------------------------------------------------------------------------
EXE=actsim.$(EXT)

SUBDIRS=simlib ext
TARGETS=$(EXE)
TARGETINCS=actsim_ext.h
TARGETINCSUBDIR=act

OBJS=actsim.o main.o chpsim.o prssim.o state.o channel.o xycesim.o

SRCS=$(OBJS:.o=.cc)

include config.mk

ifdef N_CIR_XyceCInterface_INCLUDE
include xyce.in
endif

include $(ACT_HOME)/scripts/Makefile.std

$(EXE): $(OBJS) $(ACTPASSDEPEND) ext/lxt2_write.o
	$(CXX) $(SH_EXE_OPTIONS) $(CFLAGS) $(OBJS) ext/lxt2_write.o -o $(EXE) $(LIBACTPASS) $(LIBASIM) $(LIBACTSCMCLI) -lm -ldl -ledit $(LIBXYCE) -lz

-include Makefile.deps
