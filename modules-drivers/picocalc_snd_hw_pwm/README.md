Require hardware modification on the lyra board

    1.solder a wire from pin 4 to pin 31/32(31=SpeakerLeft,32=SpeakerRigh,)
    2.remove the resistor next to the pin 31/32 on luckfox lyra (Depending on which pin you chosed.)
    
**Critical:** If you connect the two pins without removing the resistor, you risk burning out your Lyra board.
(in my case I use pin 32)
<img width="946" height="476" alt="image" src="https://github.com/user-attachments/assets/3a77c7e9-e93c-4b28-bad1-980c1555dfad" />
<img width="1917" height="2145" alt="image" src="https://github.com/user-attachments/assets/e240b789-6ccf-4637-be1d-c96b19e9f48a" />

Now RMIO_12 can be used to output audio via hardware pwm.
