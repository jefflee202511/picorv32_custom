#!/bin/sh

chmod a+rw $1
chmod a-x $1
chmod o-w $1
chown jefflee $1
chgrp jefflee $1
