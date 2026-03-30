# BoWWServer_x86
AMD64/x86 version to keep build and optimisations simple  

Dropped silero as the wakeword model can act as VAd and with further updates will also be secondary authoritive wakeword.  
Check https://github.com/rolyantrauts/BoWWServer for https://github.com/rolyantrauts/BoWWServer/blob/main/README.md  

🛡️ Authoritative Wakeword Verification (Server-Side)
BoWW utilizes a two-stage "Second Opinion" architecture. The edge clients run a fast, highly-efficient INT8 quantized TFLite model to monitor for the wake word with minimal CPU overhead.

When a client triggers, it sends a 2-second pre-roll audio buffer to the server. To demonstrate how we can leverage higher compute power centrally, the BoWW Server runs a heavier, unquantized F32 TFLite model. Before the server opens your ALSA speakers or begins writing to a file, it acts as a "bouncer," running the pre-roll audio through the F32 model to verify the wake word wasn't a false positive (like TV background noise).

If the server's authoritative check fails, it instantly drops the stream and frees up the audio hardware.

Global Parameters
Depending on the type of verification you choose, you will use a combination of these parameters under the authoritative_wakeword block in your clients.yaml:

enabled (bool): Turn the authoritative check on or off.

type (string): The verification algorithm to use ("leading", "average", or "ratio").

threshold (float): The raw probability (0.0 to 1.0) the F32 model must hit to register a positive frame.

attack (int): [ADSR Modes] The consecutive number of frames the raw probability must exceed the threshold to open the gate.

hold (int): [ADSR Modes] The size of the sliding window used to smooth the neural network output.

decay (float): [ADSR Modes] The drop in probability required to close the gate and finalize the word.

ratio (float): [Ratio Mode] The percentage of frames (0.0 to 1.0) that must hit the threshold relative to the client's reported duration.

Mode 0: VAD-Only (Disabled)
If you are in a quiet environment and completely trust the INT8 edge nodes, you can disable the authoritative check entirely. The server will blindly accept the client's trigger and immediately begin streaming, using the F32 model purely as a Voice Activity Detector (VAD) to know when the user has stopped speaking.  

```
authoritative_wakeword:
      enabled: false
```

Mode 1: Leading Edge (Relative Decay)
Best for: Quiet to moderate environments where you want tight, responsive gating.
Logic: It uses the raw F32 output to trigger the "Attack" phase. Once open, it tracks the highest point the smoothed average reaches. The gate successfully closes (validating the word) when the average drops by your decay amount relative to that peak.

```
authoritative_wakeword:
      enabled: true
      type: "leading"    # Relative decay envelope
      threshold: 0.90    # Raw F32 probability must hit 90%
      attack: 4          # Must hold 90% for 4 frames to open the gate
      hold: 20           # 400ms smoothing window
      decay: 0.20        # Closes the gate when the average drops 20% from its peak
```

Mode 2: Average (Absolute Decay)
Best for: Noisier environments where background chatter causes transient neural network spikes.
Logic: It requires the fast raw "Attack" to trigger, but it refuses to validate the word until the smoothed average has also climbed above the threshold. Once validated, it waits for the average to drop below a fixed, absolute hard line (threshold - decay).

```
authoritative_wakeword:
      enabled: true
      type: "average"    # Absolute decay envelope
      threshold: 0.85    # Smoothed average must eventually cross 85%
      attack: 5          # Fast raw trigger (100ms)
      hold: 30           # Larger 600ms smoothing window to ignore noise gaps
      decay: 0.15        # Closes the gate when the average drops strictly below 0.70 (0.85 - 0.15)
```

Mode 3: Ratio (Density/Hit Ratio)
Best for: Highly unpredictable environments (e.g., TVs playing YouTube videos that cause partial false positives).
Logic: This mode drops the ADSR envelope entirely and relies purely on density math. The client tells the server exactly how many frames the wake word lasted. The server scans the pre-roll and counts how many frames spiked above the threshold. It divides the server's hits by the client's duration. If the resulting ratio meets your requirement, it passes.

```
authoritative_wakeword:
      enabled: true
      type: "ratio"      # Pure density mathematics
      threshold: 0.90    # The F32 model must hit 90% to register a "hit"
      ratio: 0.50        # The server hits must equal at least 50% of the client's reported frame duration
```

