#!/bin/sh
set -eu
cc -std=c11 -Wall -Wextra -Werror -I.. ../lcd_fault_injection.c test_lcd_fault_injection.c -o lcd_fault_injection_host_tests
./lcd_fault_injection_host_tests
