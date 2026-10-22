#!/bin/bash
# Full cold survey: -85 C (all tests), then stepping up with readout + dark at
# each temperature. The cooler is always switched off at the end, also on
# failure. Run detached:  setsid nohup bench/run_cold_survey.sh OUTDIR > log &
out=${1:?output directory}
cd "$(dirname "$0")/.."
mkdir -p "$out"
cooler_off() {
  python3 -c 'from andor_emccd import AndorEMCCDClient as C
c = C("localhost", name="cooler-off", acquire=True, force=True)
c.set_settings(cooler_on=False, fan_mode="full"); s = c.get_status()
print("cooler off at", s["temperature_c"], s["temperature_status"]); c.release_control()'
}
trap cooler_off EXIT
set -e
step() {  # step TARGET TASKS...
  local t=$1; shift
  python3 -u bench/cool_down.py --target "$t" --interval-s 15 --timeout-s 1500 \
    --json-out "$out/cool_$t.json"
  python3 -u bench/cold_survey.py "$out/cold_survey_$t.json" "$@"
}
[ -z "$SKIP_M85" ] && step -85 readout cic gain dark
step -70 readout dark
step -60 dark
step -40 dark
step -20 readout dark
echo "survey done"
