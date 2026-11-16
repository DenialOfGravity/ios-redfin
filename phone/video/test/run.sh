#!/bin/sh
# Host tests for phone/video (Linux/WSL cc): venus_rails.c with a mock phone, venusctl argument handling.
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
$CC -O1 -g -Wall -Wextra -Wno-unused-parameter -DRAILS_HOST_TEST -o /tmp/vrails_test rails_test.c ../venus_rails.c
/tmp/vrails_test
$CC -O1 -g -Wall -Wno-unused-parameter -Dmain=venusctl_main -c -o /tmp/venusctl_host.o ../venusctl.c
$CC -O1 -g -Wall -Wno-unused-parameter -Wno-unused-result -o /tmp/vargs_test args_test.c /tmp/venusctl_host.o -lpthread
/tmp/vargs_test
