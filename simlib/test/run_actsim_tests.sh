#!/bin/sh

echo
echo "***********************************************************"
echo "*                 Testing simlib with actsim              *"
echo "***********************************************************"
echo

check_echo=0
myecho()
{
  if [ $check_echo -eq 0 ]
  then
    check_echo=1
    count=`echo -n "" | wc -c | awk '{print $1}'`
    if [ $count -gt 0 ]
    then
        check_echo=2
    fi
  fi
  if [ $check_echo -eq 1 ]
  then
    echo -n "$@"
  else
    echo "$@\c"
  fi
}

fail=0
skipped=0
count=0

myecho " "
num=0
lim=10

fn_actfile="test.act"
process_name="test"
fn_actsim_script="test.actsim"
skip_file="skip"

for subdir in ./*/
do
    if [ -d $subdir ]
    then
        cd "$subdir"

        if [ $num -lt 10 ]
        then
            myecho ".[0$num]"
        else
            myecho ".[$num]"
        fi
        
        num=`expr $num + 1`
        count=`expr $count + 1`

        ok=1

        # make sure test.act exists
        if [ ! -f $fn_actfile ]
        then
            echo
            myecho "** SKIPPED TEST $subdir: No act file found **"
            skipped=`expr $skipped + 1`
            num=`expr $num - 1`
            cd ..
            echo
            echo
            myecho " "
            continue 1
        fi

        # make sure test.actsim exists
        if [ ! -f $fn_actsim_script ]
        then
            echo
            myecho "** SKIPPED TEST $subdir: No actsim file found **"
            skipped=`expr $skipped + 1`
            num=`expr $num - 1`
            cd ..
            echo
            echo
            myecho " "
            continue 1
        fi

        # check if the test was marked to be skipped
        if [ -f $skip_file ]
        then
            echo
            myecho "** SKIPPED TEST $subdir: Test was markted to be skipped **"
            skipped=`expr $skipped + 1`
            num=`expr $num - 1`
            cd ..
            echo
            echo
            myecho " "
            continue 1
        fi

        # simulate
        $ACT_HOME/bin/actsim $fn_actfile $process_name < $fn_actsim_script > $process_name.stdout 2> $process_name.stderr

        # check if actsim exited abnormally
        if [ $? -ne 0 ]
        then
            echo
            myecho "** FAILED TEST $subdir$fn_actfile: abnormal simulator exit **"
            fail=`expr $fail + 1`
            ok=0
        fi

        # make sure regression test output exists
        if [ ! -f "$process_name.truth" ]
        then
            echo
            myecho "** FAILED TEST $subdir$fn_actfile: no regression truth file found **"
            fail=`expr $fail + 1`
            ok=0
            cd ..
            echo
            echo
            myecho " "
            continue 1
        fi

        # check regression tests
        if [ $ok -eq 1 ] && [ -f "$process_name.truth" ]
        then

            # strip timing from test output
            sed -E 's/\[[ 0-9]*\] (.*)$/\1/g' $process_name.stdout > $process_name.processed

            if ! cmp $process_name.processed $process_name.truth >/dev/null 2>/dev/null
            then
                echo 
                myecho "** FAILED REGRESSION TEST $subdir$fn_actfile: stdout mismatch **"
		diff $process_name.processed $process_name.truth
                fail=`expr $fail + 1`
                ok=0
            fi

            # remove the processed output
            rm -f $process_name.processed
        fi

        # check regression tests error
        if [ $ok -eq 1 ] && [ -f "$process_name.truth.err" ]
        then

            if ! cmp $process_name.stderr $process_name.truth.err >/dev/null 2>/dev/null
            then
                echo 
                myecho "** FAILED REGRESSION TEST $subdir$fn_actfile: stderr mismatch **"
                fail=`expr $fail + 1`
                ok=0
            fi
        fi

        # some tests generate output files. if so, check that as well
        if [ $ok -eq 1 ] && [ -f "_outfile_.0" ]
        then

            if ! cmp "_outfile_.0" "_outfile_.0.truth" >/dev/null 2>/dev/null
            then
                echo 
                myecho "** FAILED REGRESSION TEST $subdir$fn_actfile: outfile mismatch **"
                fail=`expr $fail + 1`
                ok=0
            fi
        fi

        # set the seperator tokens to only newline, so we an iterate over grep output
        oldifs=$IFS
        IFS=$'\n'

        # reset the seperator tokens
        IFS=$oldifs

        if [ $ok -eq 1 ]
        then
            if [ $num -eq $lim ]
            then
                echo 
                myecho " "
		lim=`expr $lim + 10`
            fi
        else
            echo
            echo
            myecho " "
        fi
        cd ..
    fi
done

if [ $num -ne 0 ]
then
    echo
fi

if [ $fail -ne 0 ]
then
    echo

    # check if any tests were skipped
    if [ $skipped -eq 1 ]
    then
        echo "--- 1 test was skipped ---"
        echo
    else
        echo "--- $skipped tests were skipped ---"
        echo
    fi

    if [ $fail -eq 1 ]
    then
        echo "--- Summary: 1/$count test failed ---"
    else
        echo "--- Summary: $fail/$count tests failed ---"
    fi
    exit 1
else
    echo
    echo

    # check if any tests were skipped
    if [ $skipped -ne 0 ]
    then
        if [ $skipped -eq 1 ]
        then
            echo "--- 1 test was skipped ---"
            echo
        else
            echo "--- $skipped tests were skipped ---"
            echo
        fi
    fi

    echo "SUCCESS! All tests passed."
fi
echo
