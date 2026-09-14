# Agent Memory

## NativeScript React Native Module Porting

- When the active ask is source parity for a NativeScript TypeScript port, do not keep re-debugging an already-root-caused simulator symptom first. Clean the mechanical port, deviation comments, source tests, and docs before using simulator stress as the final verification gate.
- Simulator AX visibility is not UIKit transition/readiness proof. A visible button can still fail because the app scene is stale, the wrong bundle is foreground, or a detached UIKit/RN touch host is out of lifecycle. Verify with deterministic dev-client launch and focused stress after source cleanup.
- If simulator behavior gets flaky, shut down existing simulators and verify on a fresh iOS 26.5 iPhone 17 simulator. Do not stack source changes on stale SpringBoard/app lifecycle evidence.
- Keep generic runtime capability generic. Do not add library-specific TurboModule helpers for `react-native-screens` or React Navigation; expose reusable UIKit/ObjC interop primitives in NativeScript runtime when a TypeScript port needs native capability.
- SimDeck AX frame fallbacks must be conditional. Prefer a plausible AX element center; reject only impossible frames (for example giant full-screen frames). Do not replace plausible AX with screenshot-guessed coordinates, since the screenshot can be visually misleading while AX remains the actual tappable point.
