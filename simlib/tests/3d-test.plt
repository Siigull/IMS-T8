#
# Model: 3d-test
#
set grid
set title "Simple sattelite, Earth, and Moon dynamics"
set nokey
set xlabel "x [1000 km]"
set ylabel "y [1000 km]"
set zlabel "z [1000 km]"
set style data lines
plot "3d-test.out" using 2:3, "" using 5:6
pause -1 "enter"
splot "3d-test.out" using 2:3:4, "" using 5:6:7
pause -1 "enter"
