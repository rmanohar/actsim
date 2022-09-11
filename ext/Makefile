#
#
# Simple .o generation
#
#

runtest:
runtestsub:
installincsub: all
installsub: all
install: all
install_dirs: all
install_inc: all
dependsub: all
depend: all

all: lxt2_write.o

lxt2_write.o: lxt2_write.c  lxt2_write.h wavealloca.h
	$(CC) -I. -c lxt2_write.c

realcleansub: realclean
cleansub: clean
realclean: clean

clean: 
	-rm lxt2_write.o

