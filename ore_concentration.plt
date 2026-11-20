############################################################################
# Model: ore_concentration (IMS project, Zone 1)
#
# Usage:  gnuplot ore_concentration.plt
#
# Data columns in ore_concentration.dat:
#   1 time[h]  2 Q_CONC[t]  3 q_wip(blocked)  4 q_crush  5 q_flot
#   6 cum_conc[t]  7 cum_out[t]
#
# The .dat file also contains SIMLIB statistics after the time series
# (non-comment lines); the awk filter below selects only the 7-column
# numeric rows.
#
reset

set style data lines
set grid
set xlabel "Time [h]"

set title "Zone 1: Q_CONC buffer level vs. cumulative flows"
set ylabel "Q_CONC level [t]"
set y2label "Cumulative [t]"
set y2tics
plot "< awk 'NF==7 && $1+0==$1' 'ore_concentration.dat'" \
         using 1:2 title "Q_CONC level [t]", \
     "< awk 'NF==7 && $1+0==$1' 'ore_concentration.dat'" \
         using 1:6 title "cum. concentrate into WIP [t]" axes x1y2, \
     "< awk 'NF==7 && $1+0==$1' 'ore_concentration.dat'" \
         using 1:7 title "cum. consumed by digest stub [t]" axes x1y2, \
     150 title "Q_CONC capacity" with dots
pause 1

set title "Zone 1: queue lengths (blocking after buffer fills ~day 25)"
set ylabel "waiting batches"
unset y2label
unset y2tics
plot "< awk 'NF==7 && $1+0==$1' 'ore_concentration.dat'" \
         using 1:3 title "blocked at full Q_CONC", \
     "< awk 'NF==7 && $1+0==$1' 'ore_concentration.dat'" \
         using 1:5 title "flotation queue", \
     "< awk 'NF==7 && $1+0==$1' 'ore_concentration.dat'" \
         using 1:4 title "crusher queue"
pause 1

#end
