#!/usr/bin/env bash
# CPU-saturation monitor for the SERVER box. Prints, once per second, the whole
# box's CPU utilization and the server process's CPU%, so you can CONFIRM the
# server is actually CPU-bound under the off-host load (idle% near 0) rather than
# I/O-wait-bound and ~idle as it is over loopback (the whole point of going
# off-host). On SIGINT it prints a summary over the sampled window.
#
# Usage: cpu_watch.sh <pid> [interval_sec]
# Linux: reads /proc/stat + /proc/<pid>/stat (no extra packages needed).
# macOS: falls back to `top` (less precise) for a macOS server box.
set -u
PID="${1:?usage: cpu_watch.sh <pid> [interval]}"
IV="${2:-1}"
NCPU=$( (nproc 2>/dev/null) || sysctl -n hw.ncpu 2>/dev/null || echo 1)

echo "cpu_watch: pid=$PID cores=$NCPU interval=${IV}s   (Ctrl-C for summary)"
echo "  time     busy%   idle%   server_cpu%(of ${NCPU}00%)"

sum_busy=0; sum_idle=0; n=0; min_idle=100; max_srv=0

if [ -r /proc/stat ]; then
  read_cpu() { awk '/^cpu /{idle=$5+$6; tot=0; for(i=2;i<=NF;i++)tot+=$i; print tot, idle}' /proc/stat; }
  read_proc() { awk '{print $14+$15}' "/proc/$PID/stat" 2>/dev/null; }   # utime+stime (ticks)
  HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
  read ptot pidle < <(read_cpu); pproc=$(read_proc)
  trap 'echo; echo "=== saturation summary (n=$n) ==="; \
        [ $n -gt 0 ] && awk -v b=$sum_busy -v i=$sum_idle -v mi=$min_idle -v ms=$max_srv -v n=$n \
          "BEGIN{printf \"  avg busy=%.1f%%  avg idle=%.1f%%  min idle=%.1f%%  peak server=%.1f%%\n\", b/n, i/n, mi, ms}"; \
        echo "  (CPU-saturated => idle% near 0 and server_cpu% near ${NCPU}00%)"; exit 0' INT
  while kill -0 "$PID" 2>/dev/null; do
    sleep "$IV"
    read ctot cidle < <(read_cpu); cproc=$(read_proc)
    dt=$((ctot-ptot)); di=$((cidle-pidle)); dp=$((cproc-pproc))
    [ "$dt" -le 0 ] && continue
    idle=$(awk -v di=$di -v dt=$dt 'BEGIN{print 100.0*di/dt}')
    busy=$(awk -v idle=$idle 'BEGIN{print 100.0-idle}')
    # server CPU% of one core *100 (so 800% = 8 cores); ticks over wall ticks
    srv=$(awk -v dp=$dp -v dt=$dt -v nc=$NCPU 'BEGIN{print 100.0*nc*dp/dt}')
    printf "  %s  %6.1f  %6.1f   %8.1f\n" "$(date +%H:%M:%S)" "$busy" "$idle" "$srv"
    sum_busy=$(awk -v s=$sum_busy -v v=$busy 'BEGIN{print s+v}')
    sum_idle=$(awk -v s=$sum_idle -v v=$idle 'BEGIN{print s+v}')
    min_idle=$(awk -v m=$min_idle -v v=$idle 'BEGIN{print (v<m)?v:m}')
    max_srv=$(awk -v m=$max_srv -v v=$srv 'BEGIN{print (v>m)?v:m}')
    n=$((n+1)); ptot=$ctot; pidle=$cidle; pproc=$cproc
  done
else
  # macOS / no /proc: use top sampling (server box on macOS = kqueue backend).
  echo "  (no /proc; using top -- macOS server box, kqueue backend)"
  trap 'echo; echo "stopped"; exit 0' INT
  while kill -0 "$PID" 2>/dev/null; do
    line=$(top -l 1 -pid "$PID" -stats cpu 2>/dev/null | tail -1)
    idle=$(top -l 1 -n 0 2>/dev/null | awk -F'[ %]+' '/CPU usage/{print $(NF-1)}')
    printf "  %s  server_cpu=%s%%  idle=%s%%\n" "$(date +%H:%M:%S)" "${line:-?}" "${idle:-?}"
    sleep "$IV"
  done
fi
