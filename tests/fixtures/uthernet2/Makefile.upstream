# Rebuilds src/test/resources/ip65test.bin, the 6502 program Ip65Test runs.
#
# Needs cc65 (https://github.com/cc65/cc65) on the PATH and an IP65 checkout
# (https://github.com/cc65/ip65) with its TCP stack and Apple II Uthernet II
# driver built:
#
#   make -C $IP65/ip65 ip65_tcp.lib
#   make -C $IP65/drivers a2uther2.lib
#   make IP65=/path/to/ip65
#
# The checked-in binary was built from IP65 commit 25a9c5aa9480ddab0e506108ec7f1ce538165d28
# with cc65 77b6484.

IP65 ?= ../../../../ip65

../resources/ip65test.bin: ip65test.s ip65test.cfg
	ca65 --cpu 6502 -o ip65test.o ip65test.s
	ld65 -C ip65test.cfg -o $@ ip65test.o $(IP65)/ip65/ip65_tcp.lib $(IP65)/drivers/a2uther2.lib
	rm -f ip65test.o
