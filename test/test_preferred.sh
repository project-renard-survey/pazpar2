#!/bin/sh

TEST=`basename $0 .sh`
# srcdir might be set by make
srcdir=${srcdir:-"."}

#TODO set up solr target. For now use donut

${srcdir}/run_pazpar2.sh $TEST
E=$?

grep "has preferred" ${TEST}pazpar2.log | cut -f 4- -d ' ' > test_preferred.log

if [ -f test_preferred.res ] ; then
    diff test_preferred.res test_preferred.log > test_preferred.dif
    E2=$?
    if [ $E2 -ne 0 ] ; then
	echo "has preferred test failed!"
	E=$E2
    fi
else
    echo "Making test_preferred.res for first time."
    mv test_preferred.log test_preferred.res
fi

exit $E

# Local Variables:
# mode:shell-script
# sh-indentation: 2
# sh-basic-offset: 4
# End:
