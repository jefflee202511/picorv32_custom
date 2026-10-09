#!/bin/sh
rm icebreaker_fw.*
rm icebreaker.bin
make icebreaker_fw.bin
make icebreaker.bin

sleep 10

make icebprog

