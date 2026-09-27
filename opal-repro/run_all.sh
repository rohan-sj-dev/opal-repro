#!/usr/bin/env bash
# Reproduces the shape of the paper's Figures 3, 6 and 7(c) on one machine.
# Each configuration is run 3 times; results/*.csv hold every run.
set -u
B=./bench.exe
R=2000000
S=2
mkdir -p results

out=results/threads.csv; ./bench.exe --header > $out
for rep in 1 2 3; do
  for W in lookup-only lookup-heavy balanced update-heavy update-only; do
    for T in 1 2 4 8 12 16; do
      for L in stdrw mcsrw optiql opal opal-nor; do
        $B --lock $L --workload $W --threads $T --records $R --secs $S >> $out
      done
    done
  done
done

out=results/oversub.csv; ./bench.exe --header > $out
for rep in 1 2 3; do
  for W in balanced update-heavy; do
    for T in 16 32 48 64; do
      for L in mcsrw optiql opal; do
        $B --lock $L --workload $W --threads $T --records $R --secs $S --no-pin >> $out
      done
    done
  done
done

out=results/skew.csv; ./bench.exe --header > $out
for rep in 1 2 3; do
  for K in 0.05 0.1 0.2 0.3 0.4 0.5; do
    for L in mcsrw optiql opal; do
      $B --lock $L --workload update-only --threads 16 --records $R --skew $K --secs $S >> $out
    done
  done
done
echo DONE
