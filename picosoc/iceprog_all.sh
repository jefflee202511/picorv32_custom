#!/bin/sh
echo "============================="
echo "       Start programing ..   "
echo "============================="
IS_BITS=""
IS_FW=""
if [ -z "$1" ]; then
	echo " none of args "
	IS_BITS="1"
	IS_FW="1"
else
	# bin/sh case = is comparator
	# bin/bash case == is comparator
	if [ "$1" = "bits" ]; then
	    IS_BITS="1"
        elif [ "$1" = "fw" ]; then
            IS_FW="1"
        fi


	
fi

	if [ "$IS_BITS" != "" ]; then
		echo " set  Bitstream = $IS_BITS "
	fi

        if [ "$IS_FW" != "" ]; then
                echo " set  Firmware = $IS_FW "
        fi

	#exit


        iceprog -t
	iceprog -t


        if [ "$IS_BITS" = "1" ]; then
            iceprog   icebreaker.bin
        fi

	iceprog -t


        sync
        sleep 0


        
	if [ "$IS_FW" = "1" ]; then
	    iceprog   -s -o 1M icebreaker_fw.bin
	fi







echo "       End programing ! "
