# Open questions

## Design

- Are all `Ref<T>` counts atomic by default, or can compiler-proven confinement use plain counts, and how is migration to shared ownership kept safe?
- Is `sync`/`mutate` required for every `Ref<T>`, or only for those that may cross tasks, and how is shareability declared?
- When does a growable array get a dedicated virtual region instead of a relocatable buffer?
- Exact string representation, slicing and UTF-8 indexing.
- How does a throw interact with a scope that still has child vthreads?
- Stack model: fixed reserve with guard pages, or segmented stacks.
- How are exported lifetime and effect metadata versioned?

## Not yet built or measured

- Any compiler. Everything here is hand-written C beside `.stra` source.
- The multi-object lock retry, and a sharded store to exercise it.
- Incremental table resize and key-expiry accuracy in mini-redis.
- A Rust or Go mini-redis for a baseline.
- Streaming and multipart bodies, offloaded file I/O and exception unwinding in the C.

## Waiting on a decision

- Is `Disconnect` the right default for a slow subscriber?
- Should constants become camelCase too?
