# Wayland dispatch seed corpus

Raw client→server byte streams for `fuzz_wayland_dispatch`, in wire format: each message is
a 4-byte object id, then a word packing `(size << 16) | opcode`, then arguments.

Seeds matter more than usual here. The wire format rejects almost every random byte string
at the header, so an unseeded run never reaches argument unmarshalling or Lumen's own
request handlers — coverage sits in the low tens and stays there. Starting from valid
message sequences moved it to ~108 and got the fuzzer into `bind()` argument decoding,
string termination checks and new-object-id validation.

| Seed | Sequence |
|------|----------|
| `sync` | `wl_display@1.sync` |
| `get_registry` | `wl_display@1.get_registry` — the first thing every real client sends |
| `bind_compositor` | `get_registry`, then `wl_registry@2.bind("wl_compositor")` |
| `create_surface` | the above, then `wl_compositor@3.create_surface` — reaches Lumen's handler |

Add a seed whenever a new global becomes bindable, so the fuzzer can get far enough to
exercise its requests rather than rediscovering the handshake each run.

Regenerate or extend with the script in the commit that added this directory, or by hand:
the format is small enough that `struct.pack("<II", obj, (size << 16) | opcode)` plus
arguments is the whole of it.
