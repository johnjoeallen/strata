# Compiled modules and application linking

A **compiled Strata module** (working extension `.smod`; a bundle of them is an archive, `.sar`) is the compiled form of one source *package* (a folder of files that all start with the same `package name` line, see [D14](open-decisions.md#d14-packages-and-imports)); the module takes the package's name. It is what a library ships and what an application consumes. It carries enough to type-check, lifetime-check, effect-check and optimise a program against it **without the original source**. Names and extensions are placeholders.

## 1. Three tiers of information

The module separates what is needed for correctness from what only enables optimisation. A consumer can drop a higher tier and still get a correct program.

| Tier | Purpose | Contents | If absent |
|---|---|---|---|
| **A: Contract** | Correctness | Exported types and layouts, signatures, borrow origins, effect summaries, interface/dispatch contracts, generic constraints, ownership metadata, versions and hashes | The module cannot be used. |
| **B: Instantiation IR** | Correctness for generics and exposed bodies | IR of generic functions and types, and of functions the author exposes for inlining | Generic code cannot be instantiated downstream. (Unless generics use a shared-code representation; see D6.) |
| **C: Optimisation IR** | Speed and size | IR of every non-generic function, call-graph edges with targets, park-point and liveness tables, frame layouts | Still correct and linkable through Level-0 entries; no cross-module inlining, devirtualisation or fusion. |
| (cache) | Build speed | Per-target C and native objects, keyed by hashes | Rebuilt on demand. |

A module can be shipped as A+B only (a "closed" library: interface and generics, plus precompiled objects) or A+B+C (an "open" one). That is the exact meaning of "without requiring the original source": the compiler needs IR, not text.

## 2. Contents of a module

```
header
  magic, format version, ABI version, compiler id
  module name, module version, target triple + layout hash
  content hash, interface hash
imports
  for each imported module: name, version range, and for each symbol used:
    the contract hash that this module was compiled against
exports                                            [Tier A]
  types: layout (or "opaque"), fields visible, cleanup method, copy/move rules,
         whether sync-able (has a lock word)
  functions: signature, per-parameter ownership mode (borrow, consume, Ref),
             borrow origins of the return value, effect summary, throws list
  interfaces: method list, per-method effect bound, vtable layout, versioning rule
  generic definitions: constraints, effect conditions (Cond(P))
  constants and static data a caller may fold
instantiation IR                                   [Tier B]
  IR for generic items and for explicitly inlinable functions
implementation                                     [Tier C]
  typed IR per function, with a per-function content hash
  call-graph edges: direct (symbol), indirect (interface method / function type + its declared bound),
                    address-taken set
  per-function park-point table: state id -> source location, live set, logical function
  frame layout (private) and the Level-0 entry names
  initialisation order and dependencies
debug (optional)
  names, line tables, logical stack maps for fused code
native cache (optional, repeatable)
  per target/compiler/flags: generated C and object, keyed by the inputs below
```

### Effect summaries

The summary is part of every exported function's contract, not an attribute of its body:

```
Summary(f) = {
  park:    Never | May | Cond(P)
  throws:  set of exception types that can escape
  foreign: none | nonblocking | blocking          // reaches a foreign call (see dispatch page)
  yields:  bool                                    // park=May only because of compiler-inserted budget checks ([yield]); cannot wait on I/O or a lock
}
advisory (not hashed into the contract) = {
  loops:   none | bounded | long | unbounded       // loop and recursion class of f including its callees; feeds the loop warnings
}
invariants: cleanup methods are Never; foreign=blocking implies park=May
```

**The `loops` class is advisory.** It records, for each exported function, the worst loop class among its own loops and those of its direct callees (nested loops compose; see [suspension 1.3](suspension.md#13-loop-warnings)). It exists so a caller's lint can warn about `tight -> parse -> scanForever` when `scanForever` is in another module and its source is not available. It is stored in Tier A but kept out of the contract hash on purpose: loop structure changes often, and a change from `bounded` to `long` must not force every dependant to recompile. Consequences: a warning can lag until the caller's lint next reads the callee's current `.smod`, and `loops` is never used for code generation. Calls through interfaces, function values and foreign code are recorded as `unknown` contributions and not as a class.

`Cond(P)` is stored symbolically, so a downstream module can resolve it with its own arguments. A conditional effect through a type parameter (`T.compare` that might park) is stored as `Cond({T})` and resolved at instantiation.

Summaries are **inferred**, then **exported as part of the contract**. The consequence, already noted for borrow origins in spec 5.1: changing an implementation can change the contract with no source signature change. The compiler reports it ([versioning](#4-versioning-and-compatibility)).

### Access and escape summaries

Also part of each exported function's contract, and of each exported type (see [sharing](../sharing.md)):

```
per borrowed or Ref parameter:  access = none | read | write      (through the receiver and by-value fields)
                                escape = none | retained | published (stored beyond the call / given to spawn or a message)
result:                         fresh  = yes | no                 (a new allocation nothing else refers to)
per type field:                 mutability = write-once | mutable
```

These are what the compiler needs to report a missing or too-weak `sync`/`mutate` across modules without the callee's source. Changing a field from write-once to mutable, a parameter's access from read to write, or a result from fresh to not fresh is a breaking change.

### Call-graph information

Retained in Tier C, and also, in reduced form, in the contract:

- **Direct edges:** `f -> g` with the symbol `g` (module-qualified). Enough for whole-program effect checking and for dead-code elimination.
- **Indirect edges:** an interface method or function-typed value, with its declared effect bound. Resolved against known implementations at link time; never assumed resolvable.
- **Address-taken set:** functions whose address escapes as a value. Needed to know which functions can be the target of a function-value call.
- **Reachability roots:** exported functions, interface implementations, and initialisers.

## 3. ABI and IR boundaries

This is the central design cut: **what must stay stable for separately compiled code to link** versus **what is free to change**.

### Level 0: the stable ABI between modules

Everything a caller compiled against `db` may rely on, and what `db` must keep working across releases that claim compatibility:

| Item | Stable? |
|---|---|
| Symbol name, argument and result passing (C calling convention rules per target) | Yes |
| `status` values `DONE`/`THROWN`/`PARKED`, and the `wake` reasons including `CANCELLED` | Yes |
| The frame **header** (resume pointer, state number, parent link, task pointer, result binding) | Yes |
| The way a function exposes its two entries (`f_call`, `f_resume`) | Yes |
| Layout of exported types that are not opaque | Yes; changing it is a breaking change |
| Vtable layout of an interface, slot order, per-slot effect bound | Yes |
| Exception type identities | Yes |
| **The frame body** (live slots, state numbering, number of frames used) | **No.** Private to the function. |
| Whether the function uses lazy, eager or embedded frames | **No.** |
| The IR | **No.** It is versioned independently; a consumer reads the IR format version. |

Keeping the frame body private is what makes incremental compilation work: a caller of `db.query` never learns how many bytes `query`'s frame is. A callee that changes its locals, adds a park point or switches to eager frames changes nothing a caller's object code depends on. **Direction**, resting on the lazy convention; if the callee's frame had to be embedded in the caller's, this would not hold (and that is why embedding happens only at link time, from IR, never at module compile).

### The IR boundary

The IR is what a consumer uses to *regenerate* code, so it must carry what the generated C needs, and no more:

- Typed, structured control flow with ownership operations explicit: move, borrow, `Ref` copy and release, `new`, cleanup scopes, `sync`/`mutate` regions, throw edges.
- Effects on every call site, and park points marked, so that frame lowering is a function of the IR alone.
- **Not** raw source text, comments, formatting or macro structure.
- Stable under a format version number; the consumer refuses a newer major version.

The minimum IR needed for cross-module optimisation is [open (D3)](open-decisions.md#d3-the-minimum-ir-for-cross-module-optimisation).

## 4. Versioning and compatibility

- **Format version** (major.minor): the layout of the file. A consumer reads any same-major file.
- **ABI version**: the Level-0 rules in the table above. A different ABI means a rebuild, never a silent link.
- **Contract hash**, per exported symbol: a hash of exactly its Tier A entry (and for generics and exposed bodies, the Tier B IR). It is the unit of compatibility.
- **Module interface hash**: a hash of all contract hashes, for a fast check.
- **Advisory data** (`loops`) is excluded from every hash above.
- **Compatibility rule (Direction):** an implementation change that alters no contract hash is compatible. A change that narrows an effect (`May` to `Never`), reduces `throws`, or adds an export is *backward compatible*. A change that widens an effect (`Never` to `May`), adds to `throws`, changes layout or changes a borrow origin is **breaking**, whether or not the signature text changed.
- **Semantic version policy:** breaking contract changes require a major version; the compiler can compare two versions of a module and print the contract diff (which symbols changed and which dimension). This answers spec 10.10 (API stability) at the tooling level; the policy itself is [Open](open-decisions.md#d4-representing-effects-in-public-interfaces).

### The native cache key

A cached C or object file for a function is valid only for the inputs it was generated from:

```
key = hash( function IR hash,
            contract hashes of every symbol it references,
            Tier B/C IR hashes of anything inlined into it,
            module ABI + runtime ABI version,
            compiler version, C compiler id + version,
            optimisation level + codegen flags, target triple )
```

Any input changing gives a different key; a stale entry is never reused, only left to age out.

## 5. Application compile and link

### Incremental compilation (default for development)

1. For each module, in dependency order: read the interfaces of imports (Tier A only), type-check, infer effects and borrows, produce IR, lower to Level 0, emit C, cache the object.
2. Link the objects and the runtime with the system linker. No IR from other modules is read.
3. **Early cut-off:** if a module's interface hash is unchanged after recompilation, its dependants are not touched, even though its objects changed (they relink, not recompile).

Correctness across separately compiled modules rests on Tier A plus Level 0. Calls to `May` functions are compiled to check `status` because the *contract* says `May`, whatever the body does today.

### Optimised application compilation (release builds)

1. Resolve all modules (names, versions, contract hashes). A mismatch between an importer's recorded hash and the provider's is an error naming the symbol.
2. Build the closed-world call graph from Tier C edges and the build configuration (entry points, open interfaces).
3. Check effects across modules with the summaries: a `Cond(P)` is resolved against real arguments; indirect calls are checked against implementations; a violated bound is an error.
4. Re-run lifetime/borrow checks that cross modules (only the contracts are needed).
5. Decide optimisations: devirtualise calls with a single reachable implementation, inline small functions, fuse or embed frame chains, specialise generics.
6. Re-lower and re-emit only the functions changed by step 5. Everything else keeps its cached Level-0 object.
7. Run the C compiler and linker; ideally with LTO as an independent layer.

Step 5 is where the retained IR is consumed. If a module lacks Tier C, calls into it stay at Level 0 and nothing else breaks.

### What each step costs

| Cost | Why | Mitigation |
|---|---|---|
| Whole-program effect resolution | One pass over summaries: proportional to call-graph edges and indirect bound checks | Summaries are small and cached per module; independent of code size |
| Devirtualisation and inlining need IR | Read and re-lower IR for the functions touched | Limit to reachable code from roots; parallel by SCC; cache the results keyed as in section 4 |
| Re-optimisation on change | A change to one function can alter fusion decisions upstream | Content-hashed products: only chains whose inlined inputs changed are regenerated |
| IR size in the module | Tier C is bigger than the contract | Tier C is optional; shipped as separate section or file |
| Cache invalidation | A wrong key gives wrong code | Keys include everything in section 4; unknown dimensions invalidate |
| Cold full build | Whole-program mode over all modules | Not the default; development uses incremental |

These costs are estimated from experience of comparable systems, not measured for Strata. Evaluating the build-time cost is part of the [plan](benchmarks.md#build-and-tooling-measurements).

## 6. When a module needs recompiling

Recompile module M when any of these changes:

1. **M's own source** (or its IR, if only IR is provided).
2. **A contract hash of any symbol M references.** Granularity is the symbol, not the whole imported module: M is not rebuilt because an unused export of a dependency changed.
3. **Compiler, runtime ABI, module ABI, C compiler or flag set** (the cache key dimensions).
4. **An inlined body, in optimised mode only:** if M's cached link-time product inlined or fused a function from N, and that function's Tier B/C IR hash changed.
5. **Build configuration that affects closed-world decisions:** the set of open interfaces or the entry points. (Budget checks are chosen by `[yield]`/`[noyield]` in the source, so they are already in the source hash and in the effect summaries; there is no separate build setting to track. The lint level is not a dimension: warnings do not affect generated code.)

Not a reason to recompile M: a change in an imported function's body that leaves its contract hash alone (in incremental mode); a change in an unreferenced export.

**Ripple of effect changes (the main risk).** If `net.Conn.send` becomes `May` where it was `Never`, every caller's contract and generated code change, and so does every caller's callers when their own summary widens. The ripple is bounded by the call graph, and stops wherever a `nopark` tag or an already-`May` function absorbs it. The module compile prints the contract diff so the cause is visible; this is the same visibility the ownership metadata already needs.

## 7. Cross-module propagation, example

```
package net      export Conn.recv       Park=May   foreign=nonblocking    hash n1
                export Conn.close      Park=Never                         hash n2

package db       import net.Conn.recv@n1
                export query           Park=May (via net.Conn.recv)       hash d1
                export escape          Park=Never                         hash d2

package app      import db.query@d1, db.escape@d2
                handle()   -> Park=May      (via db.query)   inferred; nothing written
                render()   -> Park=Never
```

If `db` is rewritten internally to cache rows and `query` now sometimes avoids `recv`, its body changes but not its contract: `d1` is unchanged, `app` is not recompiled. If `db.escape` is changed so that it logs through a `May` function, its summary becomes `May`, `d2` changes, and `app` is recompiled (and `render`, if it called `escape`, becomes `May` too).

```
 app.handle ---May---> db.query ---May---> net.Conn.recv   (extern parks)
      |                                     ^ summary read from net.smod
      +--Never--> app.render                ^ no net source needed
 db.smod carries only: query: May; its IR (Tier C) is read in optimised link only
```
