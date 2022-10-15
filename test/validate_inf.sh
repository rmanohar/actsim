#!/bin/sh

ARCH=`$ACT_HOME/scripts/getarch`
OS=`$ACT_HOME/scripts/getos`
EXT=${ARCH}_${OS}
if [ ! x$ACT_TEST_INSTALL = x ] || [ ! -f ../actsim.$EXT ]; then
  ACTTOOL=$ACT_HOME/bin/actsim
  echo "testing installation"
  echo
else
  ACTTOOL=../actsim.$EXT
fi

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
	$ACTTOOL $i test > runs/$i.stdout 2> runs/$i.stderr <<EOF
step 10000
EOF
done
