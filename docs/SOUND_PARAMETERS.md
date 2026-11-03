# Sound parameter automation

SQ8L exposes 256 sound parameters with stable numeric identities and symbols.
Display names and groups use readable synth terminology: `OSC 1 Octave`,
`DCA 1 Level`, `ENV 1 Time 1 (Free)`, `LFO 1 Rate (Sync)` and `Filter Cutoff`.
Modulation slots retain their numbers, for example `OSC 1 Mod 2 Amount`.

## Editing and state

Host edits and editor gestures share a parameter service. Accepted values are
available to host getters immediately, and pending changes are applied at the
render boundary. The existing engine mutex protects access, so parameter
callbacks can block; this implementation makes no lock-free callback guarantee.

Each LFO rate and envelope time has a separate Free and Sync identity. The
envelope or LFO `Sync` switch chooses the active value. The service retains
explicit edits to inactive values so switching modes can recover those edits.
Project state includes an extension for these retained values; legacy state
without the extension remains supported. Program replacement, compare and
project restore also update the service and host-facing values.

Names and groups are presentation metadata. Symbols, numeric parameter IDs,
ordering, native value domains and defaults remain independent of display naming.

## Implementation

- `src/engine/SoundParameters.*` declares identities, descriptors and pure codecs.
- `src/engine/SoundParameterLabels.*` shares value formatting with the editor.
- `src/plugin/SoundParameterService.*` manages accepted and pending values.
- `src/plugin/SoundParameterState.*` validates state and composes lifecycle changes.
- `patches/dpf-sq8l-parameters-*.patch` implements opt-in VST2, VST3, CLAP and AU
  delivery, gesture handling and host refresh behavior.

CMake applies the vendor patch stack during configuration. Serialize complete
configure/build sequences that share one source tree, or use independent checkouts
with their own submodules. Configuring one build can otherwise change vendor
sources while another build compiles them.

## Build and check

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/sq8l_render_check --regression tests/regression
```

The VST2 ABI checker requires a Python interpreter with NumPy. Select it with
`-DPython3_EXECUTABLE=/path/to/python` when configuring. Native CTest coverage
includes the registry, service, lifecycle, stopped editor delivery, controller
refresh, format ABIs and direct-edit/audio parity. AU checks are available through
`sq8l_au_check` on macOS. CI also contains Windows and Linux build/check jobs.

`tests/run_automation_matrix.py` and its qualification manifest provide optimized
timing tooling. Instrumentation is disabled by default; enable it only in a
separate diagnostic build with `SQ8L_SOUND_PARAMETER_INSTRUMENTATION=ON`.

Passing functional checks does not establish full performance qualification.
The full duration/repetition matrix and native Intel performance reference remain
outstanding at delivery preparation. Detailed DAW/version/workflow coverage and
Windows/Linux runtime results must be reported separately from macOS checks.
