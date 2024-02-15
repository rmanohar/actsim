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
SHLIB=libactsimext_sh_$(EXT).so

SUBDIRS=act

TARGETLIBS=$(SHLIB)
TARGETCONF=actsim.conf
TARGETINCS=simlib_file.h
TARGETACTSUBDIR=sim

CPPSTD=c++17

OBJS=src/random.os src/rom.os src/file.os src/rand_r.os src/buffer.os src/logger.os src/sinks.os

SRCS=$(OBJS:.os=.cc)

include $(ACT_HOME)/scripts/Makefile.std

.PHONY: regression cleantest

regression_lib:
	cd test && ./generate_regression_truth.sh

cleantest_lib:
	cd test && ./cleantest.sh


$(SHLIB): $(OBJS)
	$(ACT_HOME)/scripts/linkso $(SHLIB) $(OBJS) $(SHLIBCOMMON)

-include Makefile.deps
