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

The world reports the law that cannot hold, with its values. 90 crates of 12.5 kg are 1125 kg, and 975 kg is the limit.

```
$ build/quad run examples/load-too-heavy.quad
quad: a law cannot hold with these values: plan.total: .load == ((.crates * .crate_mass) + .pallet)
  the law is at examples/load-too-heavy.quad:11
  plan.total.1 (an inner value of the law) = <= 975 kg
  plan.crates = 90
  plan.crate_mass = 12.5 kg
```

## thermal.quad and two-rooms.quad: change over time

Two rooms and a wall. A native kernel moves heat through the wall, and the energy of the two rooms together stays the same. The power through the wall is a law with an inner expression.

```
$ build/quad run examples/thermal.quad examples/two-rooms.quad --until 30min --every 10min \
      --show living.temperature,cellar.temperature,wall.power
time [s]    living.temperature [K]  cellar.temperature [K]  wall.power [W]
0                           303.15                  293.15             100
600                    ~299.646901             ~296.653098         ~29.938
1200                   ~298.598143             ~297.701856          ~8.962
1800                   ~298.284165             ~298.015834          ~2.683
```

## thermal.quad and house.quad: several phenomena

Three rooms in a row and the air outside, with three walls. The same types as the two rooms, in a second file.

```
$ build/quad run examples/thermal.quad examples/house.quad --until 6h --every 2h \
      --show kitchen.temperature,hall.temperature,bedroom.temperature,outer.power
```
