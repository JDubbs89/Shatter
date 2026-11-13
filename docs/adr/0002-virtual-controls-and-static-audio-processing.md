# Virtual controls and static audio processing

Application commands use `mixer::IControl`, while native discovery and lifecycle
use the smaller `backend::IAudioBackend`. `mixer::Engine<Backend>` owns one concrete
backend that implements this interface; native callbacks enter
`mixer::AudioEngine<Backend>` and call its concrete buffer operations without
virtual backend dispatch. This separates portable mixer behavior from native
integration without making every adapter inherit mixer features or duplicating
backend objects, while keeping template definitions available for optimization.

The processor and channel controls share portable state; native adapters retain
endpoint lifetime, callback scheduling, and structural synchronization. Static
dispatch does not imply that native library calls or separately compiled DSP are
inlined, and unsupported prototype features remain unsupported.
