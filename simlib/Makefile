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

TARGETLIBS=$(SHLIB)
TARGETCONF=actsim.conf
TARGETACT=util.act rand.act scoreboards.act sources.act sinks.act _all_.act
TARGETACTSUBDIR=sim

OBJS=random.os rom.os file.os rand_r.os

SRCS=$(OBJS:.os=.c)

include $(ACT_HOME)/scripts/Makefile.std

$(SHLIB): $(OBJS)
	$(ACT_HOME)/scripts/linkso $(SHLIB) $(OBJS) $(SHLIBCOMMON)

-include Makefile.deps
