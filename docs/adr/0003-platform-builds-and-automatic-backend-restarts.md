# Platform builds and automatic backend restarts

Ship one executable per platform, compiling all of its supported adapters and
mixer template specializations. Select the running server at daemon startup,
preferring native PipeWire over PulseAudio on Linux. Templates provide static
processing dispatch; a runtime factory chooses the already compiled engine.

Audio-server replacement triggers a debounced daemon restart instead of swapping
live callback ownership. The control thread uses the existing show autosave and
shutdown path, destroys the engine and transport, and relaunches automatically.
Linux replaces its process image to preserve the systemd main PID; Windows starts
a replacement process. No-server mode keeps the command service and detection
alive, and explicit stop never restarts. Device/default changes within a server
remain the adapter's responsibility. Linux releases require both client runtime
libraries; automatic selection does not imply equal adapter capabilities.
