#!/bin/sh
# Runs ten hours at the toy power plant of plant.quad, and makes a page that
# shows the run: a diagram of the plant that moves with the time, and charts.
#
#   examples/plant-view.sh           writes build/plant-view.html
#   examples/plant-view.sh --open    writes the page and opens it in the browser
#
# The page is plant-view.html with three texts in it: the table that
# `quad run` prints, the inputs below, and the command.
set -eu
cd "$(dirname "$0")/.."

out=build/plant-view.html

# The inputs of the operator: the time, the cell and its value, and a text
# for the page. A value is in the coherent unit of its dimension.
inputs='0s|bunker.feed=0.5|The feeder starts and gives 0.5 kg/s of coal. The fire is small.
40min|turbine.valve=22500|The steam valve opens, and the turbine starts.
3h|draft2.conductance=15000|The fan of tower 2 fails.
4.5h|draft2.conductance=230000|The fan of tower 2 works again.
5.5h|bunker.feed=0.25|Half load. The feeder gives 0.25 kg/s of coal.'

show=bunker.coal,bunker.feed,bunker.fed,fire.bed,fire.firing,fire.thermal,fire.heating
show=$show,furnace.temperature,boiler.temperature,condenser.temperature
show=$show,tower1.temperature,tower2.temperature,air.temperature
show=$show,tubes.power,stack.power,loop1.power,loop2.power,draft1.power,draft2.power,draft2.conductance
show=$show,turbine.valve,turbine.steam,turbine.efficiency,turbine.electric,turbine.generated

sets=$(printf '%s\n' "$inputs" | awk -F'|' '{ printf " --set %s@%s", $2, $1 }')
command="build/quad run examples/thermal.quad examples/plant.quad --until 10h --every 3min$sets --show $show"

make -s build/quad
tmp=$(mktemp -d build/plant-view.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
$command > "$tmp/run"
printf '%s\n' "$inputs" > "$tmp/inputs"
printf '%s\n' "$command" | sed 's/ --/ \\\
    --/g' > "$tmp/command"

{
    echo '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"></head><body>'
    awk -v run="$tmp/run" -v inputs="$tmp/inputs" -v command="$tmp/command" '
        function copy(file,   line) { while ((getline line < file) > 0) print line; close(file) }
        $0 == "@RUN@"     { copy(run); next }
        $0 == "@EVENTS@"  { copy(inputs); next }
        $0 == "@COMMAND@" { copy(command); next }
        { print }' examples/plant-view.html
} > "$out"

echo "wrote $out"
if [ "${1:-}" = "--open" ]; then open "$out"; fi
