Option 1: PWM + RC Low-pass Filter (Simplest)
The ESP32-S3's LEDC peripheral has 8 channels — use 3 of them with PWM + RC filter per channel.

Hardware: 3× resistor + capacitor per channel (~1kΩ + 10µF typical)
Pros: Zero extra ICs, easy software, GPIO-flexible
Cons: Some ripple on the output; filter adds settling time; voltage limited to 3.3V
Good fit: Panel meters are slow/mechanical — ripple won't matter much if filter cutoff is low enough (e.g., ~10Hz)