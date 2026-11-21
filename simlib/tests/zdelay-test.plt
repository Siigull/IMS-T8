#
# ZDelay test data format: Time y1 y2
#
reset  # set defaults
set grid
set data style lines
set title "test of ZDelay class"
set key
set xlabel "cas"
set ylabel "y1, y2"
plot "zdelay-test.out" title "y1" with dots, \
     "zdelay-test.out" using 1:3 title "y2"
pause -1 "Press ENTER"
