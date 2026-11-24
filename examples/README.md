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

## thermal.quad and heater.quad: a flow with no kernel in C

A heater in a room, and a wall to the air outside. The heater is an interaction with a rate: it moves energy into the room at the rate `power`. No kernel in C is necessary.

```
#Heater: #Phenomenon & {
    room:  #ThermalPort
    power: %energy / %time
    heat:  #Interaction & { sink: .room.energy, rate: .power, conserved: false }
}
```

The energy comes from outside the world, so the interaction says `conserved: false`. Without it, the compile gives an error: a forgotten `source` must not make energy from nothing.

```
$ build/quad run examples/thermal.quad examples/heater.quad --until 6h --every 1h \
      --show living.temperature,wall.power
time [s]    living.temperature [K]  wall.power [W]
0                           283.15             500
3600                   ~299.875038       ~1336.065
7200                   ~302.619355       ~1473.025
10800                  ~303.073338       ~1495.456
14400                  ~303.152208        ~1499.13
18000                  ~303.169641       ~1499.732
21600                  ~303.177011        ~1499.83
```

The room comes to the temperature where the wall loses the 1500 W of the heater: 30 K above the air outside.

## tank.quad: a rate that a law gives

A tank with a tap and a leak. The tap has a constant rate. A law gives the rate of the leak from the volume, so a fuller tank leaks faster.

```
speed: .outflow == .volume / .empties
tap:   #Interaction & { sink: .volume, rate: .inflow, conserved: false }
leak:  #Interaction & { source: .volume, rate: .outflow, conserved: false }
```

```
$ build/quad run examples/tank.quad --until 20min --every 2min --show tank.volume,tank.outflow
time [s]    tank.volume [m^3]  tank.outflow [m^3 s^-1]
0                           0                        0
120               0.140092738            0.00140092738
240               0.182503146            0.00182503146
360               0.195277914            0.00195277914
480               0.198991953            0.00198991953
600                0.19993646             0.0019993646
720               0.199989867            0.00199989867
840               0.200002027            0.00200002027
960               0.199999594            0.00199999594
1080              0.200000081            0.00200000081
1200              0.199999984            0.00199999984
```

The level comes to the inflow times the time constant: 0.002 m^3/s × 100 s = 0.2 m^3. Each interaction has a `tolerance` with the default 1 %. A smaller tolerance gives shorter steps and a smaller error.
