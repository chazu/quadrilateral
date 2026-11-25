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
| `quad transfers FILE...` | Each transfer of each kernel, and the interaction in source that states it |
| `quad run FILE...` | The value of each cell after propagation |
| `quad run FILE... --until 30min --every 10min --show a.x,b.y` | A table of values over time |
| `quad run FILE... --until 20min --set room.visible=true@5min` | The same, with an input of the application at a time |
| `quad run FILE... --until 20min --switch room=fine@5min` | The same, with a switch of a model at a time |

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

## thermal.quad and detail-room.quad: a room with two models

A room can have more than one model. The coarse model has one temperature. The fine model has four parts in a row, and a kernel moves heat between neighbors. One model is active, and the other has no state and costs no computation.

A model is a field with a type below `#Model`. The phenomenon names the model that is active at the start. A map moves the state from one model to another. A map can have a condition: the room is fine while somebody looks at it, or while it is warmer than a limit.

```
#DetailRoom: #Phenomenon & {
    west:   #ThermalPort              // the port of the active model
    spread: %temperature
    model:  *"coarse" | text          // the model that is active at the start
    visible: *false | bool            // an input of the application
    limit:  %temperature              // above this temperature the room is fine also when nobody looks

    coarse: #CoarseRoom
    fine:   #FineRoom

    refine:   #Map & { from: "coarse", to: "fine", evaluator: "native.room.refine",
                       when: .visible || .west.temperature > .limit }
    abstract: #Map & { from: "fine", to: "coarse", evaluator: "native.room.abstract",
                       when: !.visible && .west.temperature <= .limit }
}
```

The wall connects to `study.west`. It does not know which model is behind the port, and its law for the power follows a switch.

```
$ build/quad run examples/thermal.quad examples/detail-room.quad --until 20min --every 5min \
      --set study.visible=true@5min --set study.visible=false@15min \
      --show hall.temperature,study.coarse.temperature,study.fine.c0.temperature,study.fine.c3.temperature,door.power
time [s]    hall.temperature [K]  study.coarse.temperature [K]  study.fine.c0.temperature [K]  study.fine.c3.temperature [K]  door.power [W]
0                         303.15                        293.15                        unknown                        unknown             100
300                  ~300.885782                       unknown                    ~295.414216                     295.414218         ~54.715
600                  ~299.852832                       unknown                    ~297.502956                    ~295.724492         ~23.498
900                  ~299.294988                   ~297.005011                        unknown                        unknown         ~22.899
1200                 ~298.776488                   ~297.523511                        unknown                        unknown         ~12.529
```

At 300 s somebody looks at the room, and it becomes fine. Each part starts at the temperature of the whole room, and the energy of the room does not change. Then the part at the wall warms faster than the far part, so less power comes through the wall. At 900 s nobody looks, and the room becomes coarse again, with the energy of the four parts.

The limit of the study is 299 K, and the study does not get that warm. With a lower limit, the room becomes fine when it is warm, with no input:

```
$ build/quad run examples/thermal.quad examples/detail-room.quad --until 20min --every 5min \
      --set study.limit=296@0s --show study.refine.when,study.coarse.temperature,study.fine.c0.temperature
time [s]    study.refine.when  study.coarse.temperature [K]  study.fine.c0.temperature [K]
0                       false                        293.15                        unknown
300                     false                   ~295.414217                        unknown
600                      true                       unknown                    ~297.373659
900                      true                       unknown                    ~297.825027
1200                     true                       unknown                    ~297.953413
```

A condition has these parts (ADR 0022):

- a path to a field with the type `bool`: `.visible`
- a comparison of a path with an atom: `.door != "open"`
- a comparison of two quantities: `.west.temperature > .limit + 2K`
- the operators `!`, `&&` and `||`, and parentheses

A part can be unknown, for example a comparison with a field that has no value. An unknown condition changes nothing. `true || unknown` is true, and `false && unknown` is false.

A condition with an operator is a cell with the path of its field `when`, such as `study.refine.when`. The laws compute it, and `--show` can print it. A law can also give a name to a truth value:

```
busy: bool
laws: [ .busy == (.visible || .people > 0) ]
```

A program in C sets the input with `qw_input_set`. It can give a room or a type of room another condition, with `qw_override_condition` and `qw_override_type_condition`. That condition is a statement about one field, such as `qw_when_true("busy")`. The program can also switch a room with `qi_switch(ir, world, "study", "fine")`.

## thermal.quad and fire.quad: a reaction

A fire burns wood and heats a room. One amount decides both changes: the mass of wood that burns. A reaction has the rate one time, and each flow has a ratio. For each kilogram that burns, the stove loses one kilogram of wood, and the room gets the heat of one kilogram.

```
#Burn: #Reaction & {
    fuel: #Interaction & { ratio: 1, conserved: false }
    heat: #Interaction & { conserved: false }
}

#Stove: #Phenomenon & {
    ...
    speed: .burning == .wood / .life
    burn: #Burn & {
        rate: .burning
        fuel: { source: .wood }
        heat: { sink: .room.energy, ratio: .heating }
    }
}
```

```
$ build/quad run examples/thermal.quad examples/fire.quad --until 2h --every 20min \
      --show stove.wood,stove.burning,cabin.temperature
```

The fire burns fast at the start and slower as the wood goes. The room becomes warm, and then the walls lose more heat than the fire gives. At each time, the heat that the fire gave is the wood that burned times 15 MJ/kg.


## thermal.quad and plant.quad: a toy power plant, with a page that shows it

A coal power plant in one file. A feeder puts coal on the grate, the fire burns it in the furnace, tubes heat the boiler, the steam goes through the turbine to the condenser, and two cooling towers give the rest of the heat to the air. The rooms and the walls are the types of `thermal.quad`.

The fire starts small and grows. In a cold furnace only the part `pilot` of the coal on the grate burns. A warmer furnace lights more of the bed, and the coal that does not burn yet stays on the grate. So the heat of the fire starts near zero, goes above the feed for some minutes while the bed burns down, and then follows the feed.

```
warmth:   .warm == (.furnace.temperature - .cold) / .kindle
ignition: .alight == (.pilot + .warm) / (1 + .warm)
speed:    .firing == .alight * .bed / .life
```

The turbine is a reaction with two flows from the same stock. The part `efficiency` of the heat of the steam becomes electricity, and the rest goes to the condenser. A law gives the efficiency from the two temperatures, so a warmer condenser makes less electricity.

```
carnot: .efficiency == .quality * (1 - .cold.temperature / .hot.temperature)
rest:   .rejected == 1 - .efficiency

expand: #Expand & {
    rate:    .steam
    work:    { source: .hot.energy, sink: .generated,   ratio: .efficiency }
    exhaust: { source: .hot.energy, sink: .cold.energy, ratio: .rejected }
}
```

The operator sets the feed of coal and the steam valve with `--set`. A script runs ten hours with five inputs, and it makes a page with a diagram that moves with the time, and with charts:

```
$ examples/plant-view.sh --open
wrote build/plant-view.html
```

The feeder starts, the fire grows, the valve opens, the fan of a tower fails and works again, the plant goes to half load, and then the bunker is empty. The page is `plant-view.html` with the table of `quad run` in it. It computes nothing about the plant.
