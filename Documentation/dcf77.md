# DCF-77 receiver module

## Technical Specifications

```
Product number: WVB-0860N-03A
RCCM category: WWVB/60KHz single frequency module
PCB size: 25.0x11.5x1.0mm/FR4
Antenna size: AR8x60mm/High Q
Nominal frequency: 60KHz±300Hz
Sensitivity: 28±2dB
Receiving IC: CME6005
 
   • Operating voltage: 1.1...3.3 V
   • Current consumption: 85 µA max
   • Receiving frequency: 60KHz
   • Board size (without antenna) LxWxH: 22x13.5x1 mm
   • Antenna quality: LxØ: 60x10mm
   • Scope of delivery: 1 piece
      ⇒ DCF receiver module with antenna

   • connect:
      ⇒ Ground = Pin G
      ⇒ Operating Voltage = Pin V
      ⇒ data = pin T
      ⇒ Power On/Off = Pin P1
   • Important note:
      ⇒ PIN P1 is an on/off switch and must be set to logic low.
```

See: https://www.xtals.co.uk/product-page/wwvb-60khz-single-frequency-modules-radio-time-signal-receiver


![Outlook-0oebjw2s.png](Outlook-0oebjw2s.png)

 - DCF77_P: PDN (power down) control pin (0 = ON; 1 = OFF)
 - DCF77_T: Time pulse output
 
# Arduino Code

https://github.com/udoklein/dcf77
