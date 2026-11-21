# Brigadoon Triangle Language

**Date:** 2026-11-20
**Source:** chatgpt.com

---

### **You** · Fri, Nov 20, 7:42 AM

I've recently been working on a project called Brigadoon on my GitHub, which integrates a uh typed, structure grammar language called Triangle, which is intended to largely, imitate QLang in its syntax and to a large extent its semantics, but its pri- its

---

### **ChatGPT** · Fri, Nov 20, 7:42 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:42 AM

primarily uh, its intended as a sort of authoring language for uh, simulation

---

### **ChatGPT** · Fri, Nov 20, 7:42 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:42 AM

content in a large, well, the I- the- the first, the application is- is sort of a a roguelike or a sort of management game like in the style of of Dwarf Fortress, that kind of thing

---

### **ChatGPT** · Fri, Nov 20, 7:43 AM

Mm. Mm-hmm.

---

### **You** · Fri, Nov 20, 7:43 AM

Um.  And what I'm wondering is  Excuse me. What I'm wondering is What would this language look like if we were to take it and try to make it absolutely m-like max it out in terms of its flexibility expressively uh, fr-ex- a- and in terms of its ability to produce performant, deep sim- simulation uh, code. Um .Can you take a look at the project and let me know what you think

---

### **ChatGPT** · Fri, Nov 20, 7:43 AM

<small><strong>Reasoning / progress:</strong><br>
Right. I&#39;m checking out Brigadoon and Triangle. Hang on. I&#39;ll be back in just a second. I&#39;m gonna trace through the repo and see what&#39;s going on. Alright, I&#39;ve got it. Here&#39;s what I&#39;m thinking. I was not able to find your repo on GitHub, so this is gonna be based on your description. If you want Triangle to be both maximally expressive and capable of producing fast simulation code, then think about it as more than just a content description language. Think about it as a language for describing the laws, constraints, and processes of a world. I&#39;d organize it around five capabilities: typed feature structures, a relational layer for logical inference, rules for change over time, probabilistic and quantitative semantics, and compilation into specialized execution strategies. The key thing is that the authoring language can be uniform without requiring a uniform execution model. That way, you can keep it expressive, but optimize the actual simulation for locality and sparse updates. Could you send me the exact GitHub URL? I&#39;d like to look at Triangle&#39;s grammar, type system, and execution model before I get too far into specifics.</small>

I think there's a particularly interesting direction for Brigadoon: **make Triangle not merely a language for describing simulation content, but a language for describing the laws, constraints, and processes of a simulated world.**

The distinction matters. A conventional content language describes what a creature *is*. A more ambitious Triangle could describe what the creature can do, what it needs, how it interacts with its environment, and which consequences follow from those interactions.

I searched GitHub for Brigadoon, but couldn't confidently identify your repository among the similarly named projects. So I haven't inspected the implementation yet. What follows is an architectural proposal based on your description, rather than a code review.

## The direction I'd explore

I'd organize Triangle around five capabilities.

**1. Typed feature structures as the foundation.** Preserve the CUE-like ability to unify partial descriptions, refine types, express constraints, and compose definitions. This is particularly powerful for defining creatures, materials, occupations, equipment, and world-generation rules without conventional inheritance hierarchies.

**2. Relations and logical inference.** Add a Datalog-like relational layer. Instead of explicitly coding every interaction, let the simulation derive relationships such as whether a creature can reach a resource, whether a room is habitable, or whether an agent possesses the skills and tools required for a task.

**3. Rules that describe change over time.** Feature structures and logical inference describe what is true. Simulations also need to describe how the world changes. I'd introduce explicit transitions, preconditions, effects, and event generation.

**4. Probabilistic and quantitative semantics.** A Dwarf Fortress-like simulation needs continuous quantities, uncertainty, distributions, and stochastic processes. Temperature, pressure, hunger, fatigue, structural integrity, and creature behavior shouldn't all be forced into the same discrete rule model.

**5. Compilation into specialized execution strategies.** This is where I'd put substantial effort. Triangle should compile declarative descriptions into different execution engines depending on their computational characteristics: incremental relational evaluation, event-driven state machines, sparse spatial updates, or tightly optimized numerical loops.

The critical insight is that **the authoring language can be uniform without requiring a uniform execution model**.

## A possible architecture

<box border radius="xl" padding={3} gap={2}>
  <box background="surface-secondary" radius="lg" padding={3} align="center" gap={1}>
    **Triangle authoring language**
    <text color="secondary" size="xs">Types · constraints · relations · processes · probabilities</text>
  </box>
  <row justify="center">
    <icon name="arrow-down" color="secondary" size="lg"/>
  </row>
  <box border radius="lg" padding={3} align="center" gap={1}>
    **Unified semantic representation**
    <text color="secondary" size="xs">Typed intermediate representation, dependency graph, effect analysis</text>
  </box>
  <row justify="center">
    <icon name="arrow-down" color="secondary" size="lg"/>
  </row>
  <grid columns={2} gap={2}>
    {#each [{name:"Constraint solver",desc:"Unification and validation"},{name:"Incremental logic",desc:"Relations and derived facts"},{name:"Event engine",desc:"Actions and state transitions"},{name:"Numerical kernels",desc:"Spatial and physical simulation"}] as item}
      <grid-item>
        <box background="surface-secondary" radius="md" padding={3} gap={1} height="100%">
          <text weight="medium" size="sm">{item.name}</text>
          <text color="secondary" size="xs">{item.desc}</text>
        </box>
      </grid-item>
    {/each}
  </grid>
  <row justify="center">
    <icon name="arrow-down" color="secondary" size="lg"/>
  </row>
  <box background="surface-secondary" radius="lg" padding={3} align="center" gap={1}>
    **Persistent simulation world**
    <text color="secondary" size="xs">Entities · components · spatial state · event history</text>
  </box>
</box>

This would let you preserve a highly expressive declarative authoring experience while optimizing the actual simulation around data locality, incremental computation, and sparse updates.

## The most exciting extension: causal simulation

Consider a dwarf who becomes hungry, seeks food, discovers a blocked passage, and decides to excavate an alternative route.

In a conventional game, much of that behavior requires explicit procedural code.

In Triangle, I'd like the world to derive it from more fundamental rules:

- Hunger creates a need.
- Available food can satisfy that need.
- Accessibility determines which food is obtainable.
- Obstacles prevent accessibility.
- Digging capabilities permit certain obstacles to be removed.
- Agents can select actions that establish the preconditions of their goals.

That's approaching a combination of typed feature structures, Datalog, automated planning, and discrete-event simulation.

It could produce surprisingly rich emergent behavior without requiring you to hand-author every possible interaction.

There is a major technical challenge, though: unrestricted logical inference, recursive planning, and arbitrary state transitions can make performance unpredictable. I'd therefore give Triangle an explicit distinction between pure, monotonic deductions and state-changing processes. That distinction would enable aggressive caching, incremental evaluation, parallelism, and static dependency analysis.

## Where I'd start

I wouldn't implement all five capabilities simultaneously.

My first experiment would be to take a small simulation—perhaps a workshop containing workers, tools, materials, production recipes, and resource needs—and implement it entirely in Triangle.

I'd then measure three things: how naturally the language expresses the simulation, how much work can be performed incrementally instead of recomputed, and whether the compiler can turn the hot paths into efficient native code.

That would expose the most important language-design constraints before committing to a large runtime.

**One thing I particularly want to investigate is whether Triangle's type system can also serve as its simulation dependency system.** If the compiler knows which facts depend on which other facts, it can potentially construct much of the simulation's incremental execution graph automatically. That could be a defining advantage over conventional scripting languages.

Could you send me the exact GitHub URL for Brigadoon? I'd like to examine Triangle's actual grammar, unification semantics, representation of feature structures, and execution model before proposing specific language extensions. I suspect there are opportunities to push the design considerably further while keeping its conceptual core surprisingly small.

---

### **You** · Fri, Nov 20, 7:44 AM

Yes, it should be uh, Chazoo, C H A Z U, and the name of the repo on GitHub is B R U G G A D U N G. [cough

---

### **ChatGPT** · Fri, Nov 20, 7:45 AM

<small><strong>Reasoning / progress:</strong><br>
Perfect. I&#39;ll pull that up.<br>
<br>
I found it: &lt;Link url=&quot;https://github.com/chazu/bruggadung&quot; title=&quot;chazu/bruggadung on GitHub&quot;/&gt;.<br>
<br>
And this changes my assessment considerably. Triangle is already much further along than your initial description suggested.<br>
<br>
I&#39;ve confirmed that it has a C11 implementation with Tomabechi-style feature-structure unification, a completed type hierarchy, quick-check optimization, definite clauses, interval arithmetic with physical units, and three-valued pattern matching. It also has an Odin integration layer and generated component structures.<br>
<br>
Most importantly, you&#39;ve already made a very consequential architectural decision: **full constraint resolution at content-load time, but a restricted, conjunctive runtime.** That&#39;s exactly the kind of separation that could make a highly expressive simulation language practical.<br>
<br>
The most promising next step is now much more specific: **compile Triangle&#39;s declarative effects and pattern dependencies into specialized runtime plans**, rather than adding an entirely new rule language.<br>
<br>
For example, the current lab constructs descriptions from entity state and matches operations against them. A compiler could potentially identify which components and fields an operation actually reads, generate specialized matchers, and invalidate cached results only when those fields change.<br>
<br>
That would open the door to incremental action menus, faster agent reasoning, and substantially larger simulated populations.<br>
<br>
I also found that your design already includes declarative effects, agent-specific knowledge views, atomic change commitment, and variable simulation detail. So several things I initially proposed are already there.<br>
<br>
I&#39;d prioritize three investigations: compiled pattern matching, incremental dependency tracking, and optimizing declarative effects into direct component operations. The crucial constraint would be preserving Triangle&#39;s existing semantics, particularly unknown matches, shared feature-structure nodes, and atomic commitment.<br>
<br>
One question will shape the deeper design: **Do you want Triangle itself to become a general-purpose simulation programming language, or do you want to preserve the current philosophy that Triangle describes the world while Odin supplies a relatively small collection of general execution mechanisms?**<br>
<br>
Those lead to substantially different architectures, and I think the second may actually give you more leverage.</small>

Okay, thanks. That repo changes things a lot. You're much further along than I realized. Triangle already has a C11 implementation with Toma Bechi-style feature structure unification, a completed type hierarchy, quick check optimization, definite clauses, interval arithmetic with physical units, and three-valued pattern matching. That is a lot. The most promising next step seems much more specific now. Compile declarative effects and pattern dependencies into specialized runtime plans rather than introducing a new rule language. That might unlock incremental action menus, faster agent reasoning, and much larger populations. One question will shape the deeper design: do you want Triangle itself to become a general-purpose simulation programming language, or do you want to keep the current philosophy where Triangle describes the world and Odin supplies general execution mechanisms?

---

### **You** · Fri, Nov 20, 7:46 AM

Um .I - I think I'm, I would I would like to experiment with, well, I I kind of want to take the Triangle thing and and and branch off to take a more um, holistic approach to having Triangle be the front end for something that is a largely self-contained um, uh, model, for deep simulation. Um mm, I'm thinking maybe it compiles to the Beam VM or um, something like Golang where concurrency is, you know, really uh, first class. Um .Or maybe it stays C and we just put sugar on top of it to make it

---

### **ChatGPT** · Fri, Nov 20, 7:47 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:47 AM

um, you know, uh, amenable to use from the Beam. Uh- At any rate, think of this as a fork of the triangle project. Maybe we'll call it something like

---

### **ChatGPT** · Fri, Nov 20, 7:47 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:47 AM

quadrilateral

---

### **ChatGPT** · Fri, Nov 20, 7:47 AM

<small><strong>Reasoning / progress:</strong><br>
Interesting. Okay, lemme think about that. Quadrilateral is a cool name. What if quadrilateral were a declarative, concurrent simulation language where typed feature structures aren&#39;t just data, they&#39;re the foundation of all computation? I wouldn&#39;t immediately commit to BEAM or Go or C. There&#39;s a more fundamental architectural question to explore first. What is the natural unit of concurrency in a simulated world? Actors, like Erlang, so every machine or institution is an independent process? Or maybe processes and phenomena like combustion or metabolism. Or reactive constraints, where the runtime figures out what needs to run based on what changed. I think maybe the strongest architecture might combine all three. My hunch would be to keep the C core and add an intermediate representation for processes and dependencies, and then maybe prototype an Erlang-facing runtime without committing to compiling everything to BEAM just yet. The really big design question might be whether a quadrilateral process is itself a typed feature structure, something that you can unify and specialize and compose. If we can make that work, quadrilateral becomes something much more interesting than just a simulation scripting language. Wanna go deeper on that or should I sketch what this might look like in quadrilateral?</small>

**Quadrilateral.** I like that as the name for an experimental fork of Triangle.

I think the interesting ambition here is to make Quadrilateral a **declarative, concurrent simulation language**, where typed feature structures aren't merely a way to describe entities but the foundation of the entire computational model.

And I wouldn't immediately commit to BEAM, Go, or C. There's a more fundamental architectural question worth exploring first: **What is the natural unit of concurrency in a simulated world?**

Consider three possibilities.

First, **actors**, as in Erlang. Every agent, machine, or institution could be an independent process exchanging messages. This gives you isolation, supervision, and an intuitive model of autonomous behavior. BEAM would be particularly attractive here.

Second, **processes and phenomena**. Instead of making every dwarf or object an actor, you might have independently executing processes representing combustion, heat transfer, economic exchange, biological metabolism, or social interactions. These processes operate on shared conceptual world state through well-defined interfaces.

Third, **reactive constraints**. This is the most intriguing possibility to me. A simulation consists of typed facts, relationships, constraints, and transitions. When something changes, the runtime incrementally determines which computations need to run. Concurrency emerges from the dependency graph rather than being explicitly attached to every entity.

I suspect the strongest architecture combines all three.

## What Quadrilateral might look like

Imagine authoring something conceptually like this—not proposed Triangle syntax yet:



The compiler would understand the inputs, outputs, units, and dependencies. It could schedule combustion only when necessary, group compatible computations, and compile the numerical work into native code.

Meanwhile, an autonomous agent might run as an actor, with its own beliefs, goals, and mailbox. Its decisions would produce proposals against the simulated world rather than directly mutating shared state.

That preserves the separation between an agent's subjective understanding and the authoritative physical simulation that you've already established in Bruggadung.

## Choosing a runtime

| Runtime | Principal advantage | Main concern |
|---|---|---|
| **BEAM** | Lightweight processes, supervision, distribution | Numeric and memory-intensive simulation kernels |
| **Go** | Goroutines, native execution, straightforward integration | Garbage collection and shared-state coordination |
| **C + scheduler** | Control over memory, data layout, and performance | You must build concurrency and supervision |
| **Hybrid BEAM + C** | Actor orchestration with native simulation kernels | Crossing the runtime boundary and coordinating state |

My inclination would be a hybrid architecture, but with a twist: **don't make BEAM processes the authoritative owners of every physical object**.

Instead, give Quadrilateral a language-level concurrency model that can be implemented using different backends.

An actor-like agent could run on BEAM, while a physical domain could execute thousands of updates in a native batch. Both would interact through typed events, observations, and proposed transitions.

This also suggests an interesting compilation pipeline:

<box border radius="lg" padding={3} gap={2}>
  <box background="surface-secondary" radius="md" padding={3} align="center">
    **Quadrilateral source**
    <text color="secondary" size="xs">Types, constraints, processes, agents, transitions</text>
  </box>
  <row justify="center">
    <icon name="arrow-down" color="secondary"/>
  </row>
  <box border radius="md" padding={3} align="center">
    **Typed simulation IR**
    <text color="secondary" size="xs">Dependencies, effects, ownership, scheduling requirements</text>
  </box>
  <row justify="center">
    <icon name="arrow-down" color="secondary"/>
  </row>
  <grid columns={2} gap={2}>
    <grid-item>
      <box background="surface-secondary" radius="md" padding={3} align="center" gap={1}>
        **BEAM backend**
        <text color="secondary" size="xs" textAlign="center">Agents, coordination, messages, supervision</text>
      </box>
    </grid-item>
    <grid-item>
      <box background="surface-secondary" radius="md" padding={3} align="center" gap={1}>
        **Native backend**
        <text color="secondary" size="xs" textAlign="center">Physics, spatial queries, bulk updates, constraints</text>
      </box>
    </grid-item>
  </grid>
  <row justify="center">
    <icon name="arrow-down" color="secondary"/>
  </row>
  <box background="surface-secondary" radius="md" padding={3} align="center">
    **Simulation scheduler and transactional world state**
  </box>
</box>

The other important opportunity is to retain Triangle's unification machinery as a first-class runtime facility. Agents could exchange partial descriptions, negotiate constraints, and progressively refine their knowledge. That is considerably more expressive than conventional actor message passing.

There are precedents worth investigating: Oz for concurrent constraint programming, CHR for constraint-driven computation, differential dataflow for incremental updates, and Erlang for fault-isolated processes.

**My preferred first experiment for Quadrilateral would be a concurrent constraint-and-event runtime built around Triangle's existing feature structures.** I'd initially retain the C implementation, add an intermediate representation for processes and dependencies, and prototype an Erlang-facing runtime without committing to compiling the whole language into BEAM bytecode.

That would let us discover whether the natural execution model really is actors, reactive constraints, or something more novel.

The big design question I'd like to tackle next is whether **a Quadrilateral process should itself be a typed feature structure**—something that can be unified, specialized, composed, and reasoned about just like the objects it operates on.

If we can make that work coherently, Quadrilateral starts to become something substantially more interesting than a simulation scripting language.

---

### **You** · Fri, Nov 20, 7:48 AM

want to go deeper on that. I- I like your question of um, number one I like the concept of of the quadrilateral process itself being a uh typed feature structure, subject to unification I also like your question about the, you know, fundamental abstraction behind simulation. I think um, that's really worth digging into also, but um, I tend to think phenomena was the interesting answer there because um, levels of detail

---

### **ChatGPT** · Fri, Nov 20, 7:49 AM

Mm-hmm. Hmm.

---

### **You** · Fri, Nov 20, 7:49 AM

, you know, in- in simulation, being able to say, well, here is the locus of uh, you know, of- of the- the simulation's focus right now, this is being able to dynamically scale level of detail in

---

### **ChatGPT** · Fri, Nov 20, 7:49 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:49 AM

simulations um, in order to uh, you know, use resources efficiently is- is very uh, important I think. Anyway, having said all that, yeah, let's go deeper

---

### **ChatGPT** · Fri, Nov 20, 7:49 AM

<small><strong>Reasoning / progress:</strong><br>
Okay, let&#39;s drill down. I love the direction you&#39;re pointing at. What if the fundamental computational object in quadrilateral is a Phenomenon, a typed description of something happening plus the laws for how it evolves? And because phenomena are typed feature structures, we can unify, specialize, compose, and maybe even transform them between different computational representations. That last part matters for variable level of detail. Instead of four hard-coded fidelity levels, define a collection of models for the same phenomenon. Each one describes its assumptions, what information it preserves, and its cost. The runtime picks the cheapest valid one that satisfies current needs. Not just player proximity, but also observability, causal relevance, uncertainty, and computational budget. The hard part there is refinement, though. Turning a coarse state back into a more detailed one is not reversible. It&#39;s a constrained reconstruction problem. Unification can check for compatibility, but it can&#39;t magically restore lost history. So we&#39;d need explicit reconstruction policies and maybe conserved quantities. I&#39;d make that distinction fundamental to the language. The next question I&#39;d want us to tackle is, should a phenomenon be a stateful thing that evolves over time, or a relation over world states and time, with execution just being a way to evaluate that relation? That second view could make time itself something the language reasons about rather than just a counter you advance. I&#39;m leaning toward that. How does that sound?</small>

I think phenomena are the right abstraction to investigate, particularly because of your interest in dynamically varying simulation fidelity.

There's a fairly radical possibility here: **the fundamental computational object in Quadrilateral isn't an entity, actor, or process. It's a phenomenon—a typed description of something happening, together with the laws governing its evolution.**

And because phenomena are typed feature structures, we can potentially unify, specialize, compose, and even transform them into different computational representations.

That last part is especially important for level of detail.

## 1. A phenomenon is more than a process

Consider a fire burning in a wooden building.

At high fidelity, we might simulate heat transfer between materials, oxygen consumption, combustion chemistry, smoke movement, and structural degradation.

At medium fidelity, we might simulate the building as a handful of thermal regions, each with a temperature, fuel quantity, and combustion rate.

At low fidelity, we might represent the entire fire as a single process with an estimated rate of fuel consumption and a probability of spreading.

At the lowest fidelity, we might represent it as a scheduled event: this building will burn down in approximately twenty minutes unless something intervenes.

These are not necessarily four different phenomena.

They're four **representations of the same phenomenon**, with different computational costs and guarantees.

That suggests a powerful design principle:

**A phenomenon should have an identity and semantics independent of the algorithm currently simulating it.**

## 2. Make phenomena typed feature structures

Here's an illustrative syntax, not something current Triangle supports.

<CodeBlock language="cue" editable>
#Phenomenon: {
    participants: _
    state:        _
    laws:         _
    observables:  _
    regimes:      _
}

#Combustion: #Phenomenon & {
    participants: {
        fuel:     #Combustible
        oxidizer: #Oxidizer
        region:   #SpatialRegion
    }

    state: {
        fuelRemaining: mass
        temperature:   temperature
        burnRate:      mass / time
    }

    laws: [
        #ConservationOfMass,
        #ConservationOfEnergy,
        #CombustionKinetics,
    ]
}
</CodeBlock>

Now suppose we define a more specific phenomenon:



Unification specializes combustion into wood combustion.

The key is that this isn't merely an inheritance mechanism. The result can carry additional constraints, applicable laws, and potentially information about which simulation algorithms are valid.

For example, a particular combustion model might only be applicable when oxygen is plentiful and the material is approximately homogeneous.

Those applicability conditions could themselves be expressed as constraints.

## 3. Level of detail becomes a model-selection problem

Here's where I think the idea gets genuinely interesting.

Instead of hardcoding four fidelity levels, define a collection of *models* that can simulate a phenomenon.

Each model describes its assumptions, the information it preserves, its computational cost, and the circumstances in which it's valid.

<box border radius="xl" padding={3} gap={3}>
  <text weight="medium">One fire, four computational representations</text>
  <box gap={2}>
    {#each [
      {name:"Detailed",sub:"Spatial combustion and heat transport",cost:95,detail:"Local temperature, oxygen, material changes"},
      {name:"Regional",sub:"Coupled thermal compartments",cost:42,detail:"Room-level temperature and fuel stocks"},
      {name:"Aggregate",sub:"Lumped combustion dynamics",cost:12,detail:"Total fuel, heat output and spread potential"},
      {name:"Event-based",sub:"Predicted transitions and wake conditions",cost:2,detail:"Estimated outcome, uncertainty and next event"}
    ] as item}
      <box gap={1}>
        <row justify="between" align="center">
          <text weight="medium" size="sm">{item.name}</text>
          <text color="secondary" size="xs">{item.sub}</text>
        </row>
        <box background="surface-tertiary" height="8px" radius="full" clip>
          <box background="rgba(72,128,196,0.85)" width={`${item.cost}%`} height="100%" radius="full"/>
        </box>
        <caption>{item.detail}</caption>
      </box>
    {/each}
  </box>
  <caption>Illustrative relative computational costs, not measured benchmarks.</caption>
</box>

A model-selection system could choose the cheapest valid representation that satisfies the current requirements.

Those requirements wouldn't be determined solely by distance from the player.

They could depend on:

- **Observability:** What must currently be visible or measurable?
- **Causal relevance:** Which phenomena could affect something important?
- **Uncertainty:** How far has the approximate model diverged from a trustworthy prediction?
- **Interactions:** Is another phenomenon introducing conditions the simplified model cannot handle?
- **Computational budget:** How much work can the runtime afford?

That means an unattended fire might run at low fidelity, but automatically become detailed when a character enters the building or when its predicted behavior becomes unreliable.

## 4. The crucial abstraction is refinement

I would give every simulation model two important operations.

**Abstraction** converts a detailed state into a cheaper representation.

**Refinement** converts an abstract state into a more detailed representation consistent with everything currently known.

Abstraction is relatively straightforward. Refinement is much harder.

If an abstract fire has consumed twenty kilograms of wood, how do we reconstruct a plausible distribution of charred material, hot spots, smoke, and structural damage?

We cannot recover information that was never retained.

So refinement must be understood as **constrained reconstruction**, not reversal.

This is where Triangle's feature structures become particularly useful.

The abstract model supplies constraints. The detailed model supplies additional constraints. Unification determines whether they are compatible, while a reconstruction procedure fills in unconstrained details.

A deterministic seed could make those choices reproducible.

But there is an important limitation: unification can establish compatibility; it cannot magically recover lost history or guarantee physically plausible reconstruction. Quadrilateral would need explicit reconstruction policies, conservation checks, and perhaps retained latent state.

I would make that distinction fundamental to the language.

## 5. Phenomena can compose

Now consider a wooden building containing a fire.

The building participates in several phenomena simultaneously:

- Combustion consumes fuel.
- Heat transfer changes temperatures.
- Structural mechanics responds to material degradation.
- Smoke transport affects visibility and breathing.
- Human behavior responds to perceived danger.

These phenomena share participants and state, but operate at different characteristic timescales.

We could express their coupling through typed interfaces rather than having each phenomenon directly manipulate the others.

For example, combustion produces heat; heat transfer consumes that contribution; structural degradation responds to temperature histories.

The interesting possibility is that **composition itself can be analyzed**.

If two phenomena operate on disjoint state, they can execute independently.

If they share conserved quantities, they need a coordinated transfer mechanism.

If one depends on another's output, the runtime can derive scheduling dependencies.

And if the coupling is weak enough, perhaps the runtime can safely run them at different fidelity levels.

That leads to a potentially very powerful scheduler: one that schedules *phenomena and their interactions*, not just entities or ticks.

## 6. Concurrency falls out of ownership and coupling

This makes me less inclined to start with BEAM as the central execution model.

BEAM is excellent at independently evolving processes with message passing. But physical phenomena often involve tightly coupled numerical state, shared conservation laws, and high-frequency interactions.

I would instead give Quadrilateral a concurrency model based on three concepts:

**Ownership:** Every authoritative state variable has one writer.

**Coupling:** Phenomena communicate through explicitly declared inputs, outputs, and transfers.

**Synchronization:** The runtime establishes boundaries when interactions require mutually consistent state.

Independent phenomena can run concurrently. Strongly coupled phenomena may need to execute together, or use an iterative numerical solver.

That model could be implemented using native threads, Go goroutines, or BEAM processes. The language needn't expose those choices directly.

## 7. The really radical extension: models are themselves unifiable

Suppose a model is also a typed feature structure.

A model could declare:



The runtime could match a phenomenon against available models, reject incompatible models, and select among valid alternatives.

More ambitiously, it could **compose models**.

A detailed combustion model might be paired with an aggregate smoke model, provided their coupling contracts are compatible.

This creates a sort of constraint-based simulation compiler.

The author describes what is happening and what must remain true. The compiler chooses how to execute it.

That is the direction I'd pursue for Quadrilateral.

## 8. The first experiment I'd build

I'd start with a deliberately small world containing a furnace, a room, and a thermal reservoir.

The experiment would have three representations: detailed spatial heat transfer, aggregate thermal dynamics, and event-based prediction.

Then I'd test whether the runtime can switch between them while preserving energy and producing acceptable results.

The success criteria would be concrete: bounded prediction error, conservation across representation changes, deterministic reconstruction given the same state and seed, and a measurable reduction in computation when using coarser models.

If that works, we've demonstrated something much more important than a new language feature: **a simulation can change its computational representation dynamically while preserving a coherent underlying world model.**

And there's one deeper question I'd like us to explore next.

Should a phenomenon be fundamentally a **stateful object that evolves over time**, or should it be a **relation over world states and time**, with processes and execution schedules merely being ways to evaluate that relation?

I'm leaning toward the second.

It brings Quadrilateral closer to constraint programming, relational semantics, and declarative physics. It could also make time itself something the language reasons about, rather than simply a counter advanced by the runtime.

That might be the foundation for the kind of maximally expressive simulation language you're imagining.

---

### **You** · Fri, Nov 20, 7:51 AM

Run that part by me again in more detail, that last part

---

### **ChatGPT** · Fri, Nov 20, 7:52 AM

Sure. So instead of thinking of a phenomenon as a little program with its own mutable state that you step forward every tick, think of it as a relation over world states and time. That means it says, given this world state at time T, these future states are valid. Then execution is just one way of exploring or sampling that relation, not the definition of the phenomenon itself. So time becomes something explicit that the language can reason about, not just a counter maintained by the engine. That opens doors for things like asking the compiler: "What has to be true for this coarse model to refine into this detailed one without contradictions?" It also means you can compare different phenomena just by comparing their constraints, not by running them. It's more like declarative physics or constraint programming than a traditional game loop. Does that help clarify what I meant?

---

### **You** · Fri, Nov 20, 7:52 AM

Yeah, y-you, yeah, that does help clarify. And you mentioned constraint programming. Um

---

### **ChatGPT** · Fri, Nov 20, 7:52 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:52 AM

. Uh .Which I know very little about uh, but intuitively, it seems to me that a type feature structures insofar as they are able to express constraints um, and, you know, uh to- to- make- make uh, branching decisions, uh, seems like they are a pretty uh, good way to express constraint uh, based programs. Um, is- is am I kind of barking up the right tree or, uh, is there prior art for this or like w- am I just mm- just like making shit up

---

### **ChatGPT** · Fri, Nov 20, 7:53 AM

<small><strong>Reasoning / progress:</strong><br>
Mm-hmm. Hmm. I&#39;m checking that out. Hang on, you&#39;re not making it up. Typed feature structures have been used as a form of constraint programming for decades. You&#39;re in good company. Triangle already does a lot of representation and propagation, and quadrilateral could lean into that with more explicit relations and solvers. There are a few strong precedents to look at. The LIFE language, which combined typed feature structures and logic programming; Oz, which combined constraints with concurrency; CHR, which lets you write rules that simplify or propagate constraints; and concurrent constraint programming, where processes communicate by adding and asking about constraints. The twist for you is that physical simulation isn&#39;t monotonic. Things change. Fuel burns, temperature moves. So we&#39;d need to distinguish constraints describing one instant from relations linking instants across time. That&#39;s why I like the &#39;relation over states&#39; view. For quadrilateral, I&#39;d explore a hybrid where unification coordinates specialized solvers, arithmetic here, logic there, simulation kernels elsewhere. And treat ordinary procedural code as just an optimization, not the semantic core. The research bet would be, can propagation be the default computation model? Does that resonate with what you&#39;re imagining, or did I drift too far into PL theory land?</small>

You're absolutely barking up the right tree. In fact, **typed feature structures have a substantial history as a form of constraint programming**. You're rediscovering a connection that people working on computational linguistics, logic programming, and programming-language theory have explored for decades.

And I think there's an interesting opportunity to take those ideas somewhere they haven't traditionally been applied: deep, concurrent, variable-fidelity simulation.

The important distinction is between **constraint representation, constraint propagation, and constraint solving**.

Triangle already does a significant amount of the first two.

Consider a feature structure describing a furnace:



Unification can combine this with another description saying the temperature is between 500 and 700 degrees. The result is a narrower constraint: 500 to 700.

That's constraint propagation. Information becomes more specific as constraints are combined.

But suppose we introduce a relationship:



Now we want information to propagate in multiple directions. If we know fuel consumption and energy density, we can calculate heat. If we know heat and energy density, we might constrain fuel consumption.

And if we only know ranges, we want to narrow all three variables until no further narrowing is possible.

That's where **constraint programming** becomes particularly interesting.

There are several important precedents.

**LIFE**, developed by Hassan Aït-Kaci and colleagues, combined typed feature structures with logic programming. It treated types and feature constraints as fundamental computational concepts. This is probably one of the closest intellectual ancestors of what you're proposing.

**Oz** is especially relevant to Quadrilateral. Oz combines logic variables, constraint programming, concurrency, and explicit state. Its dataflow concurrency model is particularly elegant: computations can wait until the information they require becomes available. Rather than explicitly orchestrating everything, information availability helps determine execution.

**Constraint Handling Rules**, or CHR, is another fascinating example. CHR lets you write rules that simplify constraints or derive additional constraints. It's effectively a programmable constraint solver. You could imagine using CHR-like rules to define how different physical phenomena interact.

And **Concurrent Constraint Programming**, associated with Vijay Saraswat and others, may be the most conceptually relevant of all.

Its basic idea is that concurrent processes communicate through a shared store of constraints.

A process can *tell* the store something new, adding information. Another process can *ask* whether something is known, and suspend until the necessary information becomes available.

That is remarkably close to what you might want from a simulation in which phenomena are continually contributing information about the world.

But there's a crucial complication.

Traditional concurrent constraint programming is largely **monotonic**. Information accumulates. Once you've established that a variable equals five, you don't subsequently establish that it equals six in the same constraint store.

Physical simulations aren't like that. Temperature changes. Objects move. Fuel disappears.

So Quadrilateral would need to distinguish between constraints describing a world at a particular time and transitions relating different world states.

For example:



That's a relation between two states, not a contradiction within one state.

And this is why I was suggesting that phenomena might fundamentally be relations over states and time.

You could think of Quadrilateral as having three interconnected layers.

The first is a **structural constraint layer**, essentially Triangle's existing typed feature structures and unification machinery.

The second is a **relational constraint layer**, supporting equations, logical relationships, and propagation across multiple variables.

The third is a **temporal layer**, where phenomena constrain how state evolves, including what must remain invariant across changes.

The runtime would then be responsible for selecting appropriate solving and execution strategies.

For instance, a small system of linear constraints might use a specialized arithmetic solver. A recursive relationship might use a logic-programming engine. A large heat-transfer problem might compile into an optimized numerical kernel.

You wouldn't want Tomabechi unification itself to solve every mathematical problem. Instead, I'd investigate a system where feature-structure unification coordinates specialized constraint domains.

That idea has a name in the literature: **constraint logic programming**, particularly CLP(X), where X denotes a particular constraint domain.

There's also a subtle connection to your interest in variable levels of detail.

A coarse simulation model could produce constraints on a future state rather than one exact predicted state. A detailed model could refine those constraints when greater fidelity becomes necessary.

For example, an abstract furnace model might predict that its temperature after ten minutes lies between 600 and 650 degrees. A detailed model could then refine that range using additional information about airflow and fuel distribution.

That gives you a principled way to represent uncertainty introduced by abstraction.

The challenge is ensuring that the coarse model's predictions genuinely encompass the detailed model's behavior. That's a numerical-analysis and model-validation problem, not something unification alone guarantees.

**The research direction I'd investigate first is the intersection of LIFE, Oz, concurrent constraint programming, and hybrid systems modeling.** Hybrid systems are systems combining discrete events with continuous dynamics, exactly the combination a Dwarf Fortress-like simulation needs.

And I have a particularly interesting architectural hypothesis for Quadrilateral:

What if each phenomenon were a typed feature structure containing not only its participants and state constraints, but also a set of *propagators*?

A propagator is a computation that watches certain constraints and narrows other constraints whenever it learns something useful.

Then unification would provide the structural substrate, propagators would provide computational behavior, and a scheduler would run propagators until the system reached a stable state—or until it needed to advance simulation time.

That could make the language feel remarkably declarative while still allowing highly optimized execution.

The key design question becomes: **Can we make constraint propagation the fundamental computational mechanism of Quadrilateral, with ordinary procedural code treated as an optimization or specialized solver?**

I think that's a much more promising question than simply deciding whether to compile Triangle to BEAM or Go.

---

### **You** · Fri, Nov 20, 7:54 AM

No, I mean, I- I- I want- I want to get theoretical. It's uh, so I'm not sure what is meant by propagation, so I'd like you to define that, but um, also just a note, I think when you say simulation kernels, it makes sense, I think, in quadrilateral for those to be implemented

---

### **ChatGPT** · Fri, Nov 20, 7:54 AM

Mm-hmm.

---

### **You** · Fri, Nov 20, 7:54 AM

outside of quadrilateral, not in quadrilateral, at least for the first pass, although my, you know, programming language nerd soul wants to say, well, this should all be expressible in quadrilateral. What's your take on- on- on , first of all, what's propagation, and then what's your take on that

---

### **ChatGPT** · Fri, Nov 20, 7:55 AM

<small><strong>Reasoning / progress:</strong><br>
Right, I&#39;m on it. First off, here&#39;s propagation. Constraint propagation means whenever you learn something new about one variable, you use the relationships you&#39;ve declared to narrow down what else could be true. You keep doing that until nothing else gets narrower. That&#39;s a fixed point. It doesn&#39;t pick a single solution, just squeezes out what it can. Now kernels. I agree with you for version one. I&#39;d keep heavy numerical kernels external, but make their meaning declarative inside quadrilateral. Think of a kernel as an implementation of a declared relation, not the thing that defines it. In practice, a kernel declares what it reads, what it writes, what it conserves, and its valid ranges, and the runtime trusts but verifies where possible. Then later, a quadrilateral-written kernel can satisfy that same contract. So semantically self-contained, computationally pluggable. Does that align with your gut?</small>

I think your instinct about external simulation kernels is right, especially for the first version of Quadrilateral. And there's a way to design that boundary without giving up the possibility of eventually expressing everything in Quadrilateral.

But first, propagation, because it's a really important concept.

## What constraint propagation means

**Constraint propagation is the process of using what you know about some variables to narrow what is possible for other variables.**

Imagine three variables: distance, speed, and time. We know that distance equals speed multiplied by time.

Suppose initially we know that distance is between 100 and 200 meters, speed is between 10 and 20 meters per second, and time is completely unknown.

A constraint propagator can infer that time must be between 5 and 20 seconds.

Now suppose we learn that the distance is exactly 150 meters. We can narrow the time range further, to between 7.5 and 15 seconds.

And suppose we subsequently learn that time is exactly 10 seconds. We can infer that speed must be exactly 15 meters per second.

Notice something interesting: **we never specified which direction to evaluate the equation.**

In ordinary procedural programming, you'd write a function that calculates distance from speed and time, or another function that calculates speed from distance and time.

In constraint programming, you describe the relationship, and the propagator narrows whichever variables it can.

That's the fundamental idea.

### Propagation as a fixed-point computation

Now imagine hundreds of constraints connected together.

A propagator narrows one variable. That change activates another propagator, which narrows another variable, and so forth.

Eventually, no propagator can narrow anything further.

We've reached a **fixed point**.

That doesn't necessarily mean we've found a unique solution. It means we've extracted all the information that our current propagation algorithms can derive without making additional choices.

For example, if we know that X plus Y equals 10, and both are between zero and ten, propagation alone may leave many possibilities.

To select a particular solution, we might need search, branching, or a stronger solver.

This is the distinction between propagation and solving.

## Why this fits Triangle

Triangle already has a particularly elegant version of this idea through unification.

When you unify two feature structures, you're combining their information into a more specific structure.

For example, if one structure says temperature is between 100 and 500 degrees, and another says it's between 300 and 700, unification produces the intersection: 300 to 500.

That's a form of propagation.

But Triangle's current arithmetic, as documented, primarily propagates constraints forward. A richer Quadrilateral could have bidirectional or multidirectional propagators.

And importantly, the propagation machinery needn't understand the implementation of every constraint.

It could expose a general protocol:

A propagator declares which variables it depends on, examines their current domains, and returns narrower domains.

The runtime schedules propagators whenever relevant information changes.

This is quite close to the propagation engines used in constraint programming systems.

## Now, about external simulation kernels

I would distinguish between two things: **the semantics of a phenomenon and the algorithm that evaluates it**.

Quadrilateral should own the former. External code can implement the latter.

Consider heat transfer.

Quadrilateral might describe a phenomenon in terms of temperature, heat capacity, conductivity, geometry, and conservation of energy.

A native kernel could implement a finite-volume numerical method to evolve that system efficiently.

But the kernel shouldn't get to invent the meaning of heat, temperature, or conservation. Those belong to the language's model.

In other words, I'd treat native kernels as *implementations of declared relations*.

This is similar to how a programming language can provide a mathematical abstraction without implementing every algorithm directly in its core language.

### A possible kernel contract

Conceptually, a Quadrilateral kernel might declare:



That's illustrative pseudocode, not existing Triangle syntax.

The crucial part is that the kernel has a declarative contract.

The runtime can reason about its dependencies, scheduling, validity, and interactions with other phenomena without understanding its internal algorithm.

But we need to be precise about guarantees: simply declaring conservation doesn't prove a native kernel conserves energy. We'd need runtime checks, static verification where feasible, or explicit trust in the implementation.

## The architectural principle I'd adopt

**Quadrilateral should be semantically self-contained, but not necessarily computationally self-hosting.**

That's a distinction I think is worth preserving.

It means the language defines what a simulation means, while implementations are free to use C, Odin, Rust, GPU kernels, BEAM processes, or eventually Quadrilateral itself.

I'd actually design the kernel interface with eventual self-hosting in mind.

A kernel written in Quadrilateral and a kernel written in C should satisfy the same contract and be interchangeable from the runtime's perspective.

That gives you an evolutionary path:

First, implement native kernels and a small propagator runtime.

Then, allow Quadrilateral to define simple propagators using its own constraint language.

Eventually, add compilation of those propagators into efficient native code.

You don't need to make the language Turing-complete on day one to preserve that possibility.

## One theoretical wrinkle I find fascinating

Constraint propagation is typically monotonic in an information-theoretic sense: a variable's possible values get narrower, not wider.

But simulation involves change, which is not monotonic in that sense.

The clean solution is to separate **information refinement from temporal evolution**.

Within a particular simulation state, propagators narrow possibilities.

Across time, phenomena relate one state to another, potentially with entirely different values.

So a temperature changing from 300 to 400 degrees is not a violation of monotonic propagation. They're two different variables: temperature at time one and temperature at time two.

That separation could give Quadrilateral a remarkably clean theoretical foundation.

And it leads to a further idea: instead of representing time exclusively as discrete ticks, Quadrilateral could support temporal constraints such as “this transition occurs sometime between five and ten seconds from now.”

Then propagators could narrow not only physical quantities but also the timing of future events.

That would be particularly useful for variable-fidelity simulation, because an abstract phenomenon could predict an interval for its next significant event rather than requiring constant updates.

**My recommendation is to make propagators the first-class computational abstraction, phenomena the first-class simulation abstraction, and native kernels interchangeable implementations of propagators or temporal evolution operators.**

The next theoretical question I'd investigate is whether we can express all three using a single typed relational formalism—or whether trying to unify them would sacrifice useful distinctions between logical constraints, numerical evolution, and discrete state transitions.

---

### **You** · Fri, Nov 20, 7:55 AM

Yes

---

### **ChatGPT** · Fri, Nov 20, 7:56 AM

Great, then I'd structure it so phenomena declare their participants, invariants, and temporal relations in quadrilateral, while allowing multiple interchangeable evaluators. Some could be native C or Odin for speed, others written in quadrilateral later. The runtime chooses evaluators based on declared assumptions and budget, while propagation keeps the world model coherent. The next theoretical question I would push on is whether time should be discrete steps or constraints over intervals, since that choice affects everything from refinement to scheduling. Do you wanna tackle that one first?

---

### **You** · Fri, Nov 20, 7:56 AM

Yeah, what's your take on that

---

### **ChatGPT** · Fri, Nov 20, 7:56 AM

<small><strong>Reasoning / progress:</strong><br>
Checking. I&#39;d make time continuous in the semantics but discrete and event-driven in execution. In other words, let phenomena declare constraints over intervals and use an event scheduler to advance only when something meaningful must happen. That matches variable fidelity. A coarse model can predict an event window, a refined model can narrow it, and nothing forces you to tick every millisecond. I&#39;d also separate physical time from evaluation time so you can revise predictions without pretending the past changed. Under the hood, I&#39;d use adaptive numerical integration for continuous parts and temporal constraint solving for scheduling, but unify their interfaces so phenomena don&#39;t care which one is used. That gives you flexibility now and keeps the theory clean. Next I would test how refinement behaves when two asynchronous phenomena suddenly interact, like a person opening a door that changes airflow to a fire that&#39;s modeled at a different resolution. Would you want the language to expose those boundaries explicitly or hide them behind the scheduler?</small>

I'd make **time continuous in Quadrilateral's semantics, but discrete and event-driven in its execution**.

More precisely, I'd treat time as a first-class quantity over which phenomena express constraints, rather than making the simulation advance through a universal sequence of ticks.

That gives us a fairly elegant combination of ideas from discrete-event simulation, hybrid dynamical systems, and temporal constraint programming.

Here's why I think that's the right direction.

First, consider three phenomena: a fire burning, a person walking, and a political institution negotiating a treaty.

Those phenomena operate on radically different timescales. The fire might require millisecond-scale integration under some conditions. Walking might need updates every fraction of a second. A negotiation might progress over days.

A global tick forces them into an artificial common resolution.

Instead, each phenomenon should declare something like: given my state at time T, I can predict my evolution until time T plus some horizon, subject to certain assumptions.

For a fire, that might be a numerical trajectory. For walking, it might be a predicted arrival time. For a negotiation, it might be a distribution over possible future events.

The scheduler then advances directly to the next time at which something meaningful needs to happen.

And this connects beautifully to your level-of-detail idea.

Imagine a furnace running at low fidelity. It predicts that its temperature will cross a critical threshold sometime between 12 and 15 minutes from now.

That interval is a constraint on an event's time.

If another phenomenon changes the furnace's airflow, the prediction becomes invalid. The runtime wakes the furnace model, propagates the new constraints, and calculates a new event window.

But if nothing disturbs it, there's no reason to keep executing the furnace every millisecond.

Now here's the subtle theoretical distinction I'd make.

**Physical time and computational time are different things.**

Physical time is part of the simulated world's semantics. Computational time is the sequence of operations the runtime performs to establish what happens.

A phenomenon might cover an hour of physical time with one computational step. Another might require a million steps to cover the same hour.

Neither should affect what time means in the world.

I'd actually distinguish three temporal concepts in Quadrilateral.

The first is **valid time**: when a fact holds in the simulated world.

The second is **event time**: when a transition occurs, potentially expressed as an interval rather than an exact instant.

The third is **evaluation time**: when the runtime computes or revises its knowledge of those facts.

This last distinction is useful for incremental computation and debugging. It also means we can revise a prediction without pretending that the simulated past changed.

There's a particularly interesting connection to constraint propagation here.

Suppose event A must happen before event B, and B must happen within five minutes of A. Meanwhile, another constraint says B must occur before noon.

Those constraints can propagate backward and forward, narrowing the possible times of both events.

This is closely related to **Simple Temporal Networks**, which represent temporal relationships as difference constraints.

Quadrilateral could use that machinery for scheduling, planning, and causal reasoning without invoking an expensive general-purpose solver.

For physical evolution, however, we'd need a different mechanism: numerical integration, analytic solutions, or bounded approximations.

I wouldn't force temporal constraint solving and numerical integration into one algorithm. I'd unify their interfaces and semantics, not their implementations.

There is one complication I think we should take seriously: **causality across asynchronously advancing phenomena**.

Suppose the fire has advanced to minute 20, but a person at minute 12 opens a door that changes its oxygen supply.

We can't simply let each phenomenon advance independently without coordinating their interactions.

So I'd retain something analogous to Bruggadung's existing scheduler: phenomena declare safe advancement horizons, and the runtime coordinates them at interaction boundaries. Parallel execution is allowed where dependencies permit it.

For a first implementation, I'd use conservative synchronization rather than speculative execution and rollback. It's easier to reason about and preserves your existing commitment model.

One other thing: I wouldn't literally represent every instant of time as a feature-structure node. That would be enormously expensive.

Instead, a phenomenon's feature structure would describe a **temporal relation or trajectory**, and its evaluator would provide operations such as predicting the next event, advancing to a requested time, and producing observations.

The runtime stores compact representations of evolution, not an infinite collection of world states.

So the architecture I'm imagining is:

- **Quadrilateral** defines temporal relationships, invariants, phenomena, and evaluator contracts.
- **Propagators** refine constraints on values and event times.
- **External kernels** evaluate continuous or computationally intensive dynamics.
- **A scheduler** coordinates advancement, dependencies, and fidelity changes.
- **A commitment system** makes discrete world changes authoritative.

And here's the part I find most exciting.

If a phenomenon is fundamentally a relation over states and time, **you can potentially reason about it backward as well as forward**.

For example, rather than merely asking, "Given this fire, what happens next?" you could ask, "What conditions would have been sufficient for this building to collapse before noon?"

That opens up planning, diagnosis, counterfactual reasoning, and agent inference using the same underlying language.

Not every evaluator would support backward reasoning, of course. A numerical kernel might only support forward evolution. But the language could distinguish the mathematical relation from the directions in which an implementation can efficiently evaluate it.

That's a powerful separation.

My preference, then, is a **hybrid temporal constraint model with event-driven execution and adaptive numerical integration**. I think it offers much more flexibility than choosing either fixed ticks or purely interval-based time.

The next question I'd investigate is whether a phenomenon's evolution should be represented as a *relation between two states*, or as a *constraint over an entire trajectory*. The latter is more expressive—it can describe things like "temperature never exceeds this threshold during the interval"—but it's also substantially harder to implement efficiently.

I suspect Quadrilateral should have trajectory semantics, with state-to-state transitions as its primary executable approximation.

---

### **You** · Fri, Nov 20, 7:57 AM

I mean, exposing those boundaries explicitly seems complicated, but I mean, gah, eh, it could be one of our ambitions I suppose

---

### **ChatGPT** · Fri, Nov 20, 7:57 AM

<small><strong>Reasoning / progress:</strong><br>
I think I&#39;d default to hiding those boundaries but expose them as an advanced control. At the basic level, authors just say &quot;opening a door affects airflow,&quot; not &quot;synchronize these two kernels at 12.4 seconds.&quot; The runtime should infer that from the interaction graph. But we can make validity contracts first-class. Phenomena say when their approximations are safe, so the scheduler can wake them up when assumptions break. That keeps it simple but gives power users the option to steer scheduling without changing meaning. I&#39;d also want something like an &quot;explain&quot; mode that shows why a phenomenon is running at high fidelity or why the scheduler can&#39;t advance. That makes the magic inspectable instead of opaque. If that sounds right, the next design decision is how shared state works across phenomena, whether constraints converge on shared variables or communicate through typed interactions. Which way are you leaning?</small>

I'd actually make **hiding those boundaries the default, but exposing them an advanced capability**. And I think we can do that without compromising the theoretical elegance of Quadrilateral.

The distinction I'd draw is between *declaring interactions* and *managing synchronization*.

An author should absolutely declare that opening a door changes airflow, and that airflow influences combustion. Those are facts about the simulated world.

But the author shouldn't ordinarily have to specify that the combustion solver must synchronize with the airflow solver at precisely 12.4 seconds.

That's an implementation concern the runtime should derive from the interaction graph.

Here's a more interesting possibility, though: **make synchronization requirements themselves constraints**.

A phenomenon might declare that its approximation remains valid as long as external heat input stays within a certain range, or that its predictions are accurate to within a specified tolerance.

When those assumptions cease to hold, the runtime knows it must reconsider the phenomenon's execution.

So rather than explicitly exposing synchronization barriers, we'd expose *validity contracts*.

For example, a coarse combustion model might say, conceptually, "I can predict this fire's evolution for the next ten minutes, provided oxygen availability doesn't change by more than five percent."

If someone opens a door, the airflow phenomenon changes the oxygen conditions. The scheduler detects that the combustion model's assumptions may no longer hold and invalidates its prediction.

One subtlety: the runtime must detect the interaction before advancing past it, rather than discovering afterward that it already committed an invalid future. That's why conservative synchronization and explicit safe horizons are important.

I think this suggests three levels of control for Quadrilateral.

At the simplest level, the author describes phenomena and their interactions. The runtime handles everything else.

At an intermediate level, the author specifies tolerances, fidelity requirements, and invariants.

At the most advanced level, the author can explicitly influence scheduling, synchronization, and evaluator selection.

But those advanced controls should be optional, and ideally shouldn't change the meaning of the simulation—only its execution strategy and approximation guarantees.

There's an interesting analogy to database systems here. You write a declarative query, and the database chooses an execution plan. You can inspect that plan and sometimes provide hints, but you don't normally orchestrate every join yourself.

**I'd like Quadrilateral to have something analogous to `EXPLAIN` for simulations.**

You could ask why a particular phenomenon is running at high fidelity, what dependencies are forcing synchronization, or why the runtime can't advance further without evaluating something.

That would make a sophisticated adaptive scheduler understandable rather than magical.

And I'd add one more ambition: let the runtime explain why a particular approximation is valid, including which assumptions and invariants justify it.

That seems especially compatible with typed feature structures, because those explanations could themselves be structured descriptions.

So I think we've arrived at a useful design principle: **authors describe causality; the runtime manages coordination; advanced users can inspect and constrain the coordination.**

The next architectural problem I'd tackle is how phenomena share state. If two phenomena both influence the temperature of the same object, do they each contribute constraints to a shared variable, or does one own that variable while the other communicates through a typed interaction?

That choice will determine whether Quadrilateral behaves more like a concurrent constraint language, an actor system, or a hybrid of the two.

---

### **You** · Fri, Nov 20, 7:58 AM

I mean, communicate through typed interactions, I think makes the most sense for scalability of the freaking uh, mental model

---

### **ChatGPT** · Fri, Nov 20, 7:59 AM

Yeah. Yeah, agreed. Typed interactions scale the mental model better. Each phenomenon owns its state and exposes typed ports like heat flux, force, or treaty terms, and then the runtime links them with explicit contracts. It keeps reasoning local and the scheduler can still build a dependency graph from those contracts. Shared variables sound simpler but they turn into global coupling fast, and you can still offer a shared-variable *view* for convenience, but compile it down to interactions so the engine stays sane. If we go that way, I'd sketch an interaction contract next, something like "this port provides energy flow within these bounds over this interval," and see how the scheduler would use it. Want to start with a super simple case like heat flow through a wall?

---

### **You** · Fri, Nov 20, 7:59 AM

Yes, please do that

---

### **ChatGPT** · Fri, Nov 20, 7:59 AM

<small><strong>Reasoning / progress:</strong><br>
Sure thing, okay, imagine two rooms with a wall. Left room at 30 degrees, right room at 20. The wall phenomenon says, &quot;I expose a heat flow port, and I need temperatures on each side plus material properties.&quot; It promises a bounded heat transfer over the next interval. The rooms don&#39;t reach inside the wall. They just connect to the port. The contract might say, &quot;Over the next minute, here&#39;s the range of heat energy I will transfer, given these temperatures and material constraints.&quot; The scheduler</small>

Let's use two rooms separated by a wall. One room is at 30 degrees Celsius, the other at 20. Heat flows through the wall from hot to cold.

The interesting thing is that **neither room needs to know how the other room works**. They only need to agree on the meaning of the interaction between them.

## 1. Three phenomena, two interactions

<box border radius="xl" padding={3} gap={2}>
  <svg viewBox="0 0 340 174" width="100%" xmlns="http://www.w3.org/2000/svg">
    <rect x="5" y="16" width="112" height="112" rx="10" fill="#D97706" fillOpacity="0.13" stroke="#D97706" strokeWidth="1.5"/>
    <rect x="223" y="16" width="112" height="112" rx="10" fill="#2563EB" fillOpacity="0.12" stroke="#2563EB" strokeWidth="1.5"/>
    <rect x="133" y="16" width="74" height="112" rx="8" fill="#888888" fillOpacity="0.12" stroke="currentColor" strokeOpacity="0.5"/>
    <g fontSize="12" textAnchor="middle" fill="currentColor">
      <text x="61" y="39" fontWeight="600">Hot room</text>
      <text x="170" y="39" fontWeight="600">Wall</text>
      <text x="279" y="39" fontWeight="600">Cold room</text>
      <text x="61" y="69" fontSize="21" fill="#D97706">30°C</text>
      <text x="279" y="69" fontSize="21" fill="#2563EB">20°C</text>
      <text x="170" y="72" fontSize="10">Conduction</text>
    </g>
    <g stroke="#D97706" strokeWidth="2.5" fill="none" strokeLinecap="round" strokeLinejoin="round">
      <path d="M81 94 H155"/>
      <path d="M147 87 L155 94 L147 101"/>
      <path d="M185 94 H259"/>
      <path d="M251 87 L259 94 L251 101"/>
    </g>
    <g fontSize="10" fill="currentColor" textAnchor="middle">
      <text x="116" y="147">Heat flux</text>
      <text x="224" y="147">Heat flux</text>
      <text x="170" y="166" opacity="0.65">Typed energy-transfer interactions</text>
    </g>
  </svg>
  <caption>Conceptual model: each room owns its thermal state; the wall mediates energy transfer.</caption>
</box>

I'd model each room as a thermal-storage phenomenon, and the wall as a heat-transfer phenomenon.

The hot room owns its internal energy. The cold room owns its internal energy. The wall describes the relationship between their boundary temperatures and the resulting heat flow.

This immediately gives us a useful ownership rule: **a phenomenon can propose an energy transfer, but it cannot directly mutate another phenomenon's energy.**

## 2. The interaction contract

For a simple steady-state wall model, Fourier's law gives us:

\[
P=\frac{kA}{L}(T_h-T_c)
\]

Here, P is heat-transfer power in watts, k is thermal conductivity, A is wall area, L is wall thickness, and the two temperatures are the boundary temperatures.

Suppose the wall has an effective thermal conductance of 10 watts per kelvin. With a 10-kelvin temperature difference, it transfers 100 watts.

<box border radius="lg" padding={3} gap={2}>
  <text weight="medium" size="sm">Illustrative heat-transfer contract</text>
  <grid columns={2} gap={3}>
    <grid-item>
      <text color="secondary" size="xs">Temperature difference</text>
      <title size="xl" color="default" tabularNums>10 K</title>
    </grid-item>
    <grid-item>
      <text color="secondary" size="xs">Transfer power</text>
      <title size="xl" color="default" tabularNums>100 W</title>
    </grid-item>
    <grid-item>
      <text color="secondary" size="xs">Duration</text>
      <title size="lg" color="default">60 s</title>
    </grid-item>
    <grid-item>
      <text color="secondary" size="xs">Energy transferred</text>
      <title size="lg" color="default">6,000 J</title>
    </grid-item>
  </grid>
  <caption>Assumes constant boundary temperatures and conductance during the interval. This is an approximation, not an exact prediction for freely cooling rooms.</caption>
</box>

What I want Quadrilateral to understand is that the wall proposes a transfer of 6,000 joules over that interval, and that the two endpoints must agree on it.

The interaction is therefore not simply a message saying "add 6,000 joules."

It's a typed, conservative transaction:

- The hot room loses 6,000 joules.
- The cold room gains 6,000 joules.
- The transfer is applied atomically.
- Both sides agree on the same time interval.

## 3. How it might look in Quadrilateral

Here's speculative syntax, borrowing Triangle's feature-structure style.

<CodeBlock language="text" editable>
#EnergyTransfer: #Interaction & {
    source: #ThermalPort
    sink:   #ThermalPort

    interval: #TimeInterval
    quantity: energy

    conserved: true
}

#ConductiveWall: #Phenomenon & {
    ports: {
        hot:  #ThermalPort
        cold: #ThermalPort
    }

    conductance: thermal_conductance

    interaction: #EnergyTransfer & {
        source: .ports.hot
        sink:   .ports.cold
    }

    evaluator: "native.conduction"
}
</CodeBlock>

The native conduction evaluator would calculate the energy transfer, but Quadrilateral would understand the types, participants, ownership, and conservation contract.

One important detail: the port types should distinguish temperature observations from energy transfers. Temperature is an intensive quantity, whereas energy is extensive. You can conserve transferred energy, but you don't conserve temperature.

## 4. Where propagation enters

Suppose the hot room initially reports that its temperature is somewhere between 28 and 32 degrees, while the cold room reports between 19 and 21.

The wall's propagator can derive a range of possible heat-transfer rates.

If its conductance is exactly 10 watts per kelvin, that gives us a range from 70 to 130 watts.

If a more precise observation subsequently narrows the hot room's temperature, the wall's predicted transfer can narrow too.

Notice that **the wall doesn't need to run its numerical simulation just because a constraint was refined**. It may only need to update the possible range of its outputs.

This is where I'd separate two operations:

**Propagation** refines knowledge about what transfers are possible.

**Evolution** actually computes or commits the transfer over simulated time.

That separation prevents us from confusing new information with physical change.

## 5. How this enables variable fidelity

Imagine the hot room is far from the player.

Its evaluator might use a single aggregate temperature and heat capacity. The nearby room might use a detailed three-dimensional temperature field.

Both can expose the same thermal interaction contract.

The detailed room provides a boundary temperature, perhaps varying across the wall. The aggregate room provides an approximate boundary temperature with an associated error bound.

The wall evaluator chooses a compatible coupling strategy.

If the aggregate approximation is no longer sufficiently accurate, the scheduler can request refinement.

Crucially, neither room has to understand the other's internal representation.

This is the payoff of typed interactions: **fidelity becomes an implementation choice behind a stable semantic interface.**

There's a qualification, though. An aggregate room temperature is not necessarily equal to its boundary temperature. The coarse model must either justify that approximation or expose a bounded boundary-temperature estimate.

## 6. What I would make fundamental

I think this example suggests that Quadrilateral needs four first-class concepts: phenomena, ports, interactions, and evaluators.

A phenomenon owns state and declares laws. A port exposes a typed boundary. An interaction relates ports and expresses what can pass between them. An evaluator implements a way of computing the phenomenon's behavior.

And I would make interactions themselves typed feature structures, so they can be unified, specialized, and inspected.

That gives us an elegant symmetry: phenomena are descriptions of processes, while interactions are descriptions of their coupling.

The runtime's job is to assemble those descriptions into an executable network.

There's one theoretical wrinkle worth exploring next: **does an interaction have its own identity and state, or is it purely a relation between ports?**

For instantaneous energy transfer, a relation may suffice. But a wall has thermal mass, meaning it stores energy and introduces delay. In that case, the wall must itself be a stateful phenomenon.

I think that's an important distinction to preserve: interactions describe exchange, while phenomena describe things that can evolve and retain state.

That would give Quadrilateral a relatively small conceptual core with considerable expressive power.

---

### **You** · Fri, Nov 20, 8:02 AM

Okay one more question. In terms of implementation, what libraries or techniques or technologies are shoe-ins to get a good start here?

---

### **ChatGPT** · Fri, Nov 20, 8:02 AM

For Quadrilateral, I think there are several technologies that are almost tailor-made for what we're discussing. And the encouraging thing is that **you already have one of the hardest pieces: Triangle's typed feature-structure engine.**

I would resist building a new VM, garbage collector, constraint solver, and scheduler simultaneously. Instead, I'd assemble a small runtime from established techniques, preserving the option to replace pieces later.

My preferred starting architecture would be:

**Triangle's C core + a propagator engine + typed interaction ports + a conservative discrete-event scheduler + external numerical kernels.**

I'd keep BEAM as an experimental concurrency backend rather than making it foundational immediately.

## The technologies I'd investigate first

| Technology | What it gives Quadrilateral | Priority |
|---|---|---|
| **MiniZinc / Gecode** | Constraint modeling and solving | High |
| **CHR** | Rules for propagating constraints | Very high |
| **Differential Dataflow** | Incremental computation over changing relations | High |
| **Sundials** | Numerical integration for continuous phenomena | High |
| **FMI / FMU** | Standard interfaces to external simulation models | Very high |
| **Erlang/OTP** | Concurrent processes and supervision | Experimental |
| **Z3** | SMT solving for validation and difficult constraints | Selective |

There's an important distinction in that table: some of these are libraries to embed, while others are conceptual models or external tools worth studying.

I wouldn't embed all seven. In fact, I'd start with only two or three new dependencies.

## My top recommendations, in more detail

<box gap={3}>
  <row align="start" gap={3}>
    <AsyncImage query="Functional Mock-up Interface FMI standard logo Modelica Association" aspectRatio="1:1" maxWidth="88px"/>
    <box flex="1" gap={1}>
      **1. <Entity category="software" value="FMI 3.0" disambig="Functional Mock-up Interface standard"/> — the most obvious architectural fit**

      FMI already defines how independently implemented simulation models exchange state and coordinate advancement. It supports model exchange, co-simulation, and scheduled execution. Its standardized interface is C-based. <Cite refs={["turn229484search2","turn229484search15"]}/>

      I would borrow its execution lifecycle, capability negotiation, and communication-point concepts. Quadrilateral could implement a smaller, more strongly typed version initially, then add FMU interoperability.
    </box>
  </row>
  <divider color="subtle"/>
  <row align="start" gap={3}>
    <AsyncImage query="SUNDIALS numerical solvers Lawrence Livermore logo" aspectRatio="1:1" maxWidth="88px"/>
    <box flex="1" gap={1}>
      **2. <Entity category="software" value="SUNDIALS" disambig="Scientific numerical solver library"/> — external numerical kernels**

      This is a very practical fit for C-based simulation. CVODE handles ordinary differential equations, IDA handles differential-algebraic equations, and ARKODE offers adaptive and multirate integration. <Cite refs={["turn229484search0","turn229484search12"]}/>

      I'd use it for the furnace/heat-transfer prototype, but keep it behind a generic evaluator interface.
    </box>
  </row>
  <divider color="subtle"/>
  <row align="start" gap={3}>
    <AsyncImage query="Gecode constraint programming toolkit logo" aspectRatio="1:1" maxWidth="88px"/>
    <box flex="1" gap={1}>
      **3. <Entity category="software" value="Gecode" disambig="Constraint programming solver toolkit"/> — a solver worth borrowing from**

      Gecode provides efficient finite-domain constraint solving, custom propagators, branching, and search. It's written in C++. <Cite refs={["turn229484search13"]}/>

      I wouldn't make it Quadrilateral's universal solver. I'd study its propagator scheduling and domain representation, then optionally integrate it for difficult combinatorial problems.
    </box>
  </row>
  <divider color="subtle"/>
  <row align="start" gap={3}>
    <AsyncImage query="Timely Dataflow differential dataflow project diagram" aspectRatio="1:1" maxWidth="88px"/>
    <box flex="1" gap={1}>
      **4. <Entity category="software" value="Differential Dataflow" disambig="Rust incremental dataflow computation framework"/> — the scalability research target**

      This is particularly relevant for maintaining derived relationships as world state changes. It incrementally updates query results when records are inserted or removed, rather than recomputing everything. <Cite refs={["turn229484search4","turn229484search10"]}/>

      Imagine updating which villagers can reach food, which machines are connected to power, or which institutions recognize a claim. It's a strong candidate for that workload, though its Rust implementation would introduce another runtime boundary.
    </box>
  </row>
</box>

## The technique I'd implement myself: a propagator network

This is small enough that I wouldn't start by embedding an entire constraint programming framework.

Give every constraint variable a domain, every propagator a list of dependencies, and maintain a work queue.



When a variable's domain narrows, schedule the propagators depending on it. Run them until the queue empties or a contradiction appears.

The critical properties are monotonic narrowing, soundness, termination conditions, and avoiding redundant work.

I'd start with integer intervals, type constraints, equality, addition, and multiplication. Triangle already implements much of this mathematically; the new work is persistent dependency tracking and scheduling.

One subtle but important distinction: when simulated state changes, old constraints must be invalidated or replaced. You cannot simply keep narrowing the previous instant's domains. I'd give each propagation context an explicit validity epoch or immutable state version.

## What about BEAM?

I would prototype BEAM integration, but I wouldn't make it the first milestone.

The attraction is real: Erlang processes provide isolation, mailboxes, supervision, and distributed coordination. That maps naturally to autonomous agents and loosely coupled phenomena.

But I would keep high-frequency simulation work inside native kernels, with BEAM managing coarse-grained orchestration.

For interoperability, there are two interesting approaches: an Erlang NIF for low-latency native calls, or Erlang ports for process isolation. NIFs must avoid long blocking computations, so I'd favor ports or dirty schedulers for expensive work.

And I wouldn't try to compile arbitrary Triangle feature structures into BEAM terms on every interaction. That could destroy the performance advantages you've already achieved. Use compact typed handles and exchange deltas instead.

## A concrete first implementation

I'd fork Triangle into Quadrilateral and build the smallest possible experiment around two rooms and a wall.

<box border radius="lg" padding={3} gap={2}>
  <list marker="number" gap={2}>
    <list-item>**Preserve Triangle's existing C engine.** Keep the type hierarchy, unifier, arithmetic, and matcher unchanged initially.</list-item>
    <list-item>**Introduce phenomenon and interaction schemas.** Compile typed ports, ownership, and dependencies into a compact execution graph.</list-item>
    <list-item>**Implement a minimal propagator scheduler.** Use a work queue and domain-version tracking. No threads yet.</list-item>
    <list-item>**Add an event-time scheduler.** Each evaluator reports its next event, safe advancement horizon, and invalidation dependencies.</list-item>
    <list-item>**Integrate one external kernel.** Start with a simple analytic heat-transfer model; compare against SUNDIALS as the more sophisticated evaluator.</list-item>
    <list-item>**Demonstrate fidelity switching.** Move between an aggregate room model and a more detailed thermal model, checking energy conservation and approximation error.</list-item>
  </list>
</box>

I'd keep this first experiment single-threaded. That may sound counterintuitive given the concurrency ambitions, but it lets you establish the semantics of interactions and time before introducing scheduling races.

Once it works, running independent phenomena concurrently becomes a much more manageable engineering problem.

## Two additional research directions

I'd put **Oz and concurrent constraint programming** near the top of your reading list, even though they aren't necessarily libraries to embed. Their treatment of logical variables, suspension, and information flow could directly inform Quadrilateral's computational semantics.

And I'd study **Modelica**, particularly its equation-based modeling and acausal connectors. Modelica is remarkably close to your idea of describing phenomena relationally rather than procedurally. It can teach us how to express physical relationships without prematurely choosing an evaluation direction.

In fact, if I were choosing the three most useful intellectual influences, they'd be **LIFE for typed feature structures, Oz for constraint concurrency, and Modelica for declarative physical modeling**.

The most important architectural decision is to make the external evaluator interface small and independent of its implementation language. If you get that right, Quadrilateral can begin as a C-based constraint and simulation runtime, experiment with BEAM, and eventually compile its own numerical kernels without redesigning the language.

I think that's the most promising route to the ambitious system we've been describing.

---

### **You** · Fri, Nov 20, 8:09 PM

can you export this entire conversation to a markdown file? and i think i had you create a document earlier in the conversation, right?

---

### **ChatGPT** · Fri, Nov 20, 8:09 PM

Yes, I can put our Quadrilateral discussion into a Markdown file.

And regarding the earlier document: **we developed several substantial design sketches, but I don't see a separate downloadable document being created in this conversation.** We discussed the architecture, phenomenon-based simulation, constraint propagation, temporal semantics, typed interactions, and implementation technologies, all directly in the chat.

There is one limitation with the export: I can reconstruct the discussion from the conversation available to me, but I don't have a raw transcript-export function that guarantees a verbatim copy of every message. I'll make a Markdown document preserving the technical substance and progression of our discussion, clearly labeled as reconstructed notes rather than an exact transcript.

[Download Quadrilateral Conversation Notes (Markdown)](sandbox:/mnt/data/quadrilateral_conversation_notes.md)

The file includes the technical discussion, architectural decisions, implementation recommendations, and open research questions.

For a **complete, verbatim transcript**, you can also use ChatGPT's data export under **Settings → Data Controls → Export Data**. That provides the original conversation text, rather than reconstructed notes.

---
