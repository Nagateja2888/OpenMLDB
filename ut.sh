#! /bin/sh
set -e -u -E # this script will exit if any sub-command fails
WORK_DIR=`pwd`
test -d /tmp/rtidb && cd /tmp/rtidb && rm -rf *
cd $WORK_DIR
cd $WORK_DIR/build/bin && ls | grep test | grep -v mem | while read line; do ./$line ; done
cd $WORK_DIR/build/bin && HEAPCHECK=normal ./tablet_impl_mem_test
