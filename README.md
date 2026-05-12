# Markov FSM

A JUCE/C++ desktop app for performing with a finite-state machine whose states host live SuperCollider lanes.

## Build

```sh
cmake -S . -B build -DJUCE_PATH=../Granny/JUCE
cmake --build build --target MarkovFSM -j 6
```

If your JUCE checkout is somewhere else, pass that folder as `JUCE_PATH`.

## Run

The built macOS app is:

```text
build/MarkovFSM_artefacts/Markov FSM.app
```

## SuperCollider

Each state can contain one or more lanes. A lane is a live `.scd` script buffer. Use `Play` to run the selected lane, or `Run FSM` / `Step` to play all lanes in the active state while the FSM advances through weighted transition rules.

For tighter standalone audio-app behavior, the app keeps one persistent `sclang` bridge alive, boots the SuperCollider server once, and preloads lane programs. This avoids spawning a new language process for every lane, while the JUCE transport keeps parent and nested FSM state changes deterministic.

Use `Boot audio` before performing to start the bridge and preload every lane program without playing anything. When `Run FSM` starts, the app also preloads all lanes before the first transition.

JUCE sends setup, play/stop, and emergency commands over OSC to the persistent language process on `127.0.0.1:57141`, with an ordered file-backed command path as startup insurance. Hidden code-level latency profiles control SC scheduling and buffer size without adding UI.

Lane objects are kept warm where possible: synth-like lane objects are gated/paused instead of recreated on every state change. A hidden crossfade value smooths state changes without adding a visible control.

States can contain nested finite-state machines. Select a state and use `+ FSM` to create a child machine, `Enter` to edit it, and `Back` to return to the parent. When the parent enters that state, the child machine runs inside it; the parent holds until the child returns to its entry state, then the parent is allowed to advance. Leaving the state stops the child and gates its lanes.

The header shows a compact audio status. `Log` opens a hidden drawer with captured `sclang` output for debugging scripts. `Panic` stops all active lane objects and sends `s.freeAll` to SuperCollider without cluttering the normal performance surface.

The app auto-detects `/Applications/SuperCollider.app/Contents/MacOS/sclang` on macOS, then falls back to `sclang` on the shell path. If SuperCollider is installed somewhere else, paste the full `sclang` executable path into the `sclang path` field.

For best stop behavior, write each lane so the final expression returns something stoppable, such as `Routine(...).play`, `Task(...).play`, or a `Synth`. The default lane scripts follow this pattern.
