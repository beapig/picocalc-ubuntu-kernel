Require hardware modification on the lyra board

    1.solder a wire from pin 4 to pin 31/32(31=SpeakerLeft,32=SpeakerRigh,)
    2.remove the resistor next to the pin 31/32 on luckfox lyra (Depending on which pin you chosed.)
    
**Critical:** If you connect the two pins without removing the resistor, you risk burning out your Lyra board.
(in my case I use pin 32)
<img width="946" height="476" alt="image" src="https://github.com/user-attachments/assets/3a77c7e9-e93c-4b28-bad1-980c1555dfad" />
<img width="1917" height="2145" alt="image" src="https://github.com/user-attachments/assets/e240b789-6ccf-4637-be1d-c96b19e9f48a" />

Now RMIO_12 can be used to output audio via hardware pwm.

## ALSA Configuration

The driver registers itself as an ALSA sound card with the following fixed parameters:
- **Format:** S16_LE (16-bit signed little-endian)
- **Sample rate:** 22050 Hz
- **Channels:** 1 (mono)

Most applications (ffplay, RetroArch, mpv, etc.) expect different audio parameters (e.g. 44100 Hz stereo). To make them work with this driver, configure ALSA's `plug` layer to automatically convert parameters.

### Quick Setup

Create `~/.asoundrc` with the following content:

```
pcm.!default {
    type plug
    slave {
        pcm {
            type hw
            card picocalcsndpwm
            device 0
        }
        format S16_LE
        rate 22050
        channels 1
    }
}

ctl.!default {
    type hw
    card picocalcsndpwm
}
```

This `plug` layer automatically handles:
- **Sample rate conversion** (e.g. 44100 Hz → 22050 Hz)
- **Channel downmixing** (stereo → mono, with L/R averaging)
- **Format conversion** (any format → S16_LE)

### RetroArch

Set the following in `~/.config/retroarch/retroarch.cfg`:

```
audio_device = "default"
audio_rate = "22050"
audio_out_rate = "22050"
audio_latency = "64"
```

The `plug` layer will handle the slight 22050 Hz conversion transparently.

### Verify

Test the configuration with:

```bash
speaker-test -D default -c 1 -r 22050 -f S16_LE -l 1
```


