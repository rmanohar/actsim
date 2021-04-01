#!/bin/sh

ARCH=`$ACT_HOME/scripts/getarch`
OS=`$ACT_HOME/scripts/getos`
EXT=${ARCH}_${OS}
ACTTOOL=../actsim.$EXT 

if [ $# -eq 0 ]
then
	list=inf[0-9]*.act
else
	list="$@"
fi

if [ ! -d runs ]
then
	mkdir runs
fi

for i in $list
do
	$ACTTOOL $i 'test<>' > runs/$i.stdout 2> runs/$i.stderr <<EOF
get Reset
step 10000
get Reset
EOF
done
