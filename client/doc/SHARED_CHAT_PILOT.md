# Optional local shared chat panel

This experimental client adapter puts a shared CPU-rendered chat panel in one
existing widget slot. It is disabled at build time and at runtime. Existing
Classic chat is unchanged. The panel labels itself **Local echo**: all text,
including slash-prefixed text, is accepted into its private in-memory history.
It never sends a command or packet. This is a candidate embedding experiment,
not chat replacement or a qualified distribution feature.

## Explicit source-build inputs

Configure the client with these additional CMake options in an owned
Classic-derived wrapper profile/build plan:

```
-DATRINIK_SHARED_CHAT_PILOT=ON
-DATRINIK_SHARED_CHAT_INCLUDE_DIR=/absolute/sdk/include
-DATRINIK_SHARED_CHAT_LIBRARY=/absolute/sdk/lib/libtilewright_ui_ffi.a
-DATRINIK_SHARED_CHAT_LINK_LIBRARIES=dl;pthread;m
-DATRINIK_SHARED_CHAT_FONT_MANIFEST=/absolute/font-pack/manifest.json
```

The library must provide the embedded ChatPanel ABI 2 and CPU prototype exports.
The linker closure shown is for the Linux static candidate only; other targets
need their own verified closure. No Rust compiler, downloads or new fonts are
required by the default build. CMake does not fetch or build the optional SDK.
Default packages do not install any SDK or font-pack assets. Supplying this
option does not establish redistribution or platform qualification.

The explicit schema-1 font manifest supplies an ordered `fonts` array. Each
entry names `family`, relative `path`, `bytes` and `sha256`. Configure verifies
font sizes, hashes and containment in the manifest directory and emits a private
build header. Runtime reads only those named paths and the shared library
reverifies their hashes. The pack is bounded to 32 fonts, 32 MiB each and 64 MiB
total. Missing, changed or incompatible inputs leave a visible generic error;
text is never logged in that error.

Run only an isolated wrapper-managed client with
`ATRINIK_SHARED_CHAT_PILOT=1` to create the panel. The fixed 480×240 widget does
not persist its layout, transcript, draft, selection or history. It is admitted
only when fully inside the window at 1:1 scale. Click its composer to focus;
Escape releases focus. An outside click releases focus and is consumed. Standard
selection keys and explicit Ctrl+A/C/X/V gestures use shared text state.
Existing popups, menus and focused Classic chat/command inputs retain precedence.
Release the existing input (Escape) before focusing the pilot. After
they close, click the pilot to focus again.

## Boundaries and validation

The host retains its single SDL window, event poller, GPU renderer and presenter.
The adapter copies at most 256 native events per drain, bounds copied text to
4096 bytes and fences both physical Enter keys on any text/composition activity
in that drain. Ordinary editing and clipboard keys remain usable alongside
committed text; non-Enter keys are quarantined during actual native composition.
Control-epoch changes cancel held Classic movement and pointer
gestures, even when the final shared state allows world input. Host key
quarantine survives widget/context destruction and reconstruction until matching
key-up events arrive; a copied native drain also finishes after destruction. Native text
sessions are restarted on ownership barriers; timestamps retire late text.
Clipboard and visual movement additionally require shared physical-key effect
permission. There is no implicit world hit from the embedded panel.

The panel renders a bounded 480×218 premultiplied RGBA surface through
`gpu_renderer_draw_surface`, inside normal widget order. The host acknowledges
its exact prepared stamp only after successful whole-frame presentation. Move,
resize, focus, modal and renderer-recovery barriers invalidate displayed
geometry before input. IME rectangles come from the acknowledged shared layout.

History is limited to 32 messages and 32 KiB; the composer and messages to
4096 bytes; pending actions to eight and prepared frames to three. Acceptance
copies an action once into local history and acknowledges its exact batch ID.
Retained pilot content denies screenshot/video/diagnostic admission through
`capture_privacy`. Hiding retains privacy. Destruction drops private memory but
capture remains denied until a successfully presented clean frame.

The enabled `client-shared-chat-pilot` CTest exercises the production adapter
with the actual shared ABI/CPU renderer and a small fake Classic host. It checks
ownership barriers, physical Enter identities, composition fences, output
backpressure, exact presentation, stale attachment geometry, alpha mode and
private-content lifetime. It uses the SDL dummy video driver and is **not** a
real GPU or game embedding qualification. Required wrapper builds, native
coverage and isolated real-runtime acceptance remain separate evidence.
