# Examples

Small contents that show what Quadrilateral does today. Build the tool, then run an example:

```
make
build/quad run examples/ohm.quad
```

The tool has three commands:

| Command | Result |
|---|---|
| `quad check FILE...` | The errors of a content, or a count of its parts |
| `quad ir FILE...` | The IR: kinds, cells, ports, propagators, kernels and dependencies |
| `quad run FILE...` | The value of each cell after propagation |
| `quad run FILE... --until 30min --every 10min --show a.x,b.y` | A table of values over time |

A value is in the coherent unit of its dimension. `~` before a value means that the value is between two steps of its scale.

## ohm.quad: a law works in every direction

A law is a relation, not an assignment. Give any two of voltage, current and resistance, and the world finds the third. A range gives a range.

```
$ build/quad run examples/ohm.quad
heater.voltage     50 V
lamp.current       0.12 A
sensor.voltage     0.95 .. 2.1 V
```

(The output has more lines. These are the values that the laws found.)

## load.quad: limits

An inequality is a law. The world finds how many crates fit on the truck, and how heavy one crate can be.

```
$ build/quad run examples/load.quad
order.crate_mass  <= 24.375 kg
van.crates        1 .. 78
```

## load-too-heavy.quad: a plan that cannot work

The compile runs the laws with the values of each instance. It reports the law that the values contradict, with its values. 90 crates of 12.5 kg and a pallet of 25 kg are 1150 kg, and 1000 kg is the limit.

```
$ build/quad check examples/load-too-heavy.quad
examples/load-too-heavy.quad:15:1: error: the values of plan at the start contradict the law plan.limit: .load <= .capacity (plan.load = 1150 kg, plan.capacity = 1000 kg)
```

## thermal.quad and two-rooms.quad: change over time

Two rooms and a wall. A native kernel moves heat through the wall, and the energy of the two rooms together stays the same. The power through the wall is a law with an inner expression.

The energy is the state of a room, and the laws compute its temperature. So the type marks the temperature with `@derived`. An instance gives the temperature at the start, and the compile finds the energy:

```
living: #Room & { temperature: 303.15K, capacity: 10000J/K }
```

```
$ build/quad ir examples/thermal.quad examples/two-rooms.quad | grep -E "stock|derived"
  stock cellar.energy: %energy = 2931500
  derived cellar.temperature: %temperature = any
  stock living.energy: %energy = 3031500
  derived living.temperature: %temperature = any
```

```
$ build/quad run examples/thermal.quad examples/two-rooms.quad --until 30min --every 10min \
      --show living.temperature,cellar.temperature,wall.power
time [s]    living.temperature [K]  cellar.temperature [K]  wall.power [W]
0                           303.15                  293.15             100
600                    ~299.646901             ~296.653098         ~29.938
1200                   ~298.598143             ~297.701856          ~8.962
1800                   ~298.284165             ~298.015834          ~2.683
```

The wall has a setting of its kernel, `tolerance`. The type gives it a default with `*`, so an instance does not state it:

```
tolerance: *1pct | %ratio
```

An instance that states `tolerance: 5pct` gets that value. A default never changes a value.

## thermal.quad and house.quad: several phenomena

Three rooms in a row and the air outside, with three walls. The same types as the two rooms, in a second file.

```
$ build/quad run examples/thermal.quad examples/house.quad --until 6h --every 2h \
      --show kitchen.temperature,hall.temperature,bedroom.temperature,outer.power
time [s]    kitchen.temperature [K]  hall.temperature [K]  bedroom.temperature [K]  outer.power [W]
0                            298.15                293.15                   289.65               66
7200                    ~282.119285           ~281.336113               280.195559           ~28.18
14400                   ~276.642967           ~276.338011              ~275.893888          ~10.973
21600                   ~274.510498           ~274.391747              ~274.218804           ~4.273
```
