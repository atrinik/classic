# Client session and semantic actions

`Atrinik::Session` is the authoritative renderer-independent model for local
gameplay state. Its public API is `src/include/session.h`; the implementation
uses only the C17 standard library and links no SDL, widget, texture, audio, or
platform-input code. `src/client/session_client.c` is the graphical client's
protocol sink and transition adapter.

The graphical main thread is the sole mutable session owner. Packet handlers
finish parsing and updating presentation caches before invoking a typed
reducer. GUI input dispatches a typed semantic action, while a future API or
automation producer can use the bounded action queue. The main loop drains
that queue immediately after SDL input. A worker must transfer requests through
an already synchronized client queue; the session object itself is deliberately
not internally synchronized.

## Revisions, snapshots, and events

Every accepted state mutation emits an event with a strictly increasing
64-bit revision. `session_snapshot_copy()` returns an owned immutable copy of
player state, visible map cells and targetable generations, inventory/skill/
spell/effect items, the stable open-container handle, dialog, quest, messages,
party/server metadata, and local intent. Pass it a zero-initialized destination
and free a successful copy with `session_snapshot_free()` before reusing it.
Carried weight is derived from root inventory items by the session reducer, so
it does not depend on whether a graphical inventory widget has rendered.

Events are a bounded ring. A consumer calls `session_events_read()` with its
last applied revision. `SESSION_EVENTS_GAP` means the consumer fell behind and
must request a fresh snapshot. `session_snapshot_apply_event()` validates
contiguous ordering and provides the canonical reconstruction behavior.
Messages use their own bounded oldest-first ring.

Default limits are 256 events, 64 pending actions, 32,768 map-layer cells,
1,024 targetable entities, 1,024 items, and 128 messages. Callers may choose
smaller limits for constrained instances and tests, within the hard limits
validated by `session_create()`.

## Generations and replay boundaries

External object references are `session_handle_t`, never raw pointers or bare
tags. A handle binds an object ID to the session generation, map/inventory
generation, and the object's insertion generation. Removal followed by tag
reuse cannot revive an old handle.

Disconnect, reconnect, logout/character reset, new maps, map scrolling, and
inventory replay advance the relevant generation. Map scrolling preserves and
shifts still-visible snapshot cells, but invalidates old entity handles. A soft
map clear preserves faces while clearing targetable entities and names,
invalidating known light, and marking the cell fogged; a hard clear removes its
layers. Metadata-only map updates, such as a player sub-layer change, do not
invalidate otherwise current handles. Inventory replay invalidates old handles
and clears the replayed container tree before accepting replacement items. If
a nested container is open while its parent refreshes, its subtree is preserved
and its handle is renewed into the new inventory generation. Lifecycle reset
clears movement, run, fire, queued actions, target, and combat intent.

## Semantic actions

Movement, path movement, stop, targeting, combat, casting, applying, getting,
dropping, talking, dialog replies, character selection, local controls, and
player commands use `session_action_t`. Dispatch validates lifecycle,
capabilities, syntax, dialog generation, and item/entity handle freshness
before calling the abstract command sink. An accepted action means only that
the local client emitted it; server-side success remains authoritative.

The SDL keybinding and widget layers are adapters. Stable action IDs and typed
payloads are the contract; persisted keybinding strings are not. The local
session contains gameplay state and intent, while render caches retain faces,
animation frames, layout, selection, scrolling, and other presentation-only
details.

The versioned IPC, web gateway, headless executable, and bot-policy migration
remain outside this library and belong to the separate client API work. Those
surfaces should consume snapshots/events and enqueue the same semantic actions
rather than adding another model or protocol implementation.
