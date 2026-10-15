// Editor.h -- the circuit editor window: build components out of gates and other components,
// and simulate them with a controllable clock.

#pragma once

namespace Kestrel {

// Loads the library and runs the window until it's closed. With a screenshot path, renders a
// few frames, saves a PNG there and exits.
int RunEditor(const char* screenshotPath, bool simulateOnStart);

}  // namespace Kestrel
