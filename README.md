# GunconDuino
PS1 Guncon controller as absolute Mouse coordinates (or Joystick) via Arduino Pro Micro or Leonardo.

This is a fork based entirely on the excellent ground work done by developer Matheus Fraguas (sonik-br).
The purpose of this fork is to make it work with the new RetroArch Shader (hold) function, making it possible to trigger a screen flash that happens before the game itself asks for coordinates, ensuring it always gets them. No more missed shots due to aiming at dark areas where tracking is lost due to no light.

Due to 1-3 frame emulation lag (setup dependent) the built-in flash wouldn't match with the original GunconDuino, some games didn't flash in the first place due to being IR instead.
This makes it possible to play all types of gun games if set up correctly.

![Sync](docs/Enclosure.jpg)
![Sync](docs/GunconSync.jpg)

Software setup:
Within the RetroArch Hotkeys, "Shader (hold)" should be mapped to keyboard "L" for this to work, use a shader of your choice such as shaders/misc/color-mangler.slang and adjust brightess/gamma there.
The shader should be saved for the game and then set as disabled in the game override config (video_shader_enable = "false" within the gamename.cfg).
Use the "rawmouse" driver, this allows you to also use two GunconDuino for 2-player lightgun games.

Adjust your setup for minimal latency, this only works with native output setups such as CRTEmudriver. 
Within RetroArch settings, adjust the swapchain number (a different number can make a 3 frames latency difference in some cases), turn off frame delay (to be on the safe side, as it can cause some latency variablity) and use d3d11 or vulkan as the video driver.

I've included the various configs for this all to work in the Preset-configs folder of this project. They can be copied over and adjusted to your setup, or simply compare them with your configs to see how you map the lightguns.

GunconDuino mappings and usage:
Calibrate by moving your aim across a fully lit screen, left-right-top-bottom to get the min-max screen values. The calibration process is pretty fast, you can set your flash shader to enabled to do this.
It keeps updating minmax until the point when the trigger has been pressed 5 times, locking in the calibration. (Reconnecting the Arduino will require a new calibration)

Trigger = Left-Click (and keyboard "L" pulse for the shader flash)
A = Right-Click
B = Middle-Click
Trigger-press right after the Arduino is plugged in = Absolute Mouse XY mode (what most games will use)
A-press right after the Arduino is plugged in = Joystick mode (for games with positional analog guns, not real lightguns), still uses mouse clicks for buttons.
Disable Combo: Press A+B+Trigger, disables the Guncon and unsticks the mouse from your aim, so you can use a regular mouse again. Trigger press re-enables the Guncon.
Holding A+B for 2 seconds = Toggles infinite hold-XY, this is for games that required continous shooting. It freezes the last seen XY-coordinates (last time light was sensed). 
  XY gets updated with every trigger pull (single screen flash)
  This is for games that weren't actual lightgun games (eg. IR). Allowing for continous shooting at the same target without other tricks like terrible black levels (making everything bright) or making a strobe     mode.
  Some games like those that used the SNES Superscope opted to use bright graphics on screen at all times instead of flashing, those allowed for continous shooting without strobing.


Use the RA MAME-core config examples I've provided here for mappings to work, lightgun games in MAME that used actual lightguns (not positional analog), such as Point Blank use GunX and GunY.
Within the mame.ini enable lightguns (not mouse, the difference is that lightguns have absolute position).
Un the RetroArch MAME core, in general input, map GunX and GunY (should NOT be MouseX MouseY).
You'll most likely need to manually edit the RetroArch and MAME-core configs to add the lightgun mappings, rather than try map them with the built in button mapping configurators.

How this works:
Instantly at any trigger hardware press it sends a keyboard "L" key pulse. (triggering the RA shader hold flash) followed by a buffered click, ensuring the game gets both valid XY-coordinates and a click.
Trigger presses for Mouse-Left-Click are continously buffered, nothing gets lost.
The buffer makes it possible to spam the trigger as fast as possible and never have it miss a shot. 
This trigger buffer is set to 34ms and there's a liniency with the XY-coordinate hold, but you may want to fine tune it for some laggier setups. (Always tackle your setup lag first however)
Hold-XY: Last XY values are held for 34ms (lag leniency failsafe)

Misc-improvements over the original GunconDuino:
Screen XY polling is made to be faster without breaking bottom of CRT screen light sensing.
Buttons are polled separately and as fast as possible, since they're not limited by CRT scan rate.
Joystick mode now still sends mouse clicks for A/B/Trigger, all lightgun A, B, Trigger presses are seen as mouse clicks. (So these are still mapped the same in the MAME-core)
A+B+Trigger disabling now makes sure clicks don't get stuck as pressed.



Build instructions:

PlayStation accessories works in 3.3v. You will need a 5v to 3.3v voltage regulator, a level shifter (photo below, the level shifter is a MUST or you will likely damage the Guncon over time),
an Arduino Pro Micro (my recommendation) or a Leonardo, and a PS1/PS2 female connector (available on aliexpress).
I personally use an off the shelf level shifter and a voltage regulator in my build. 
The shield Sonic-br recommends incorporates these into a single PCB, but it's otherwise the same thing.


####[shield](https://github.com/SukkoPera/PsxControllerShield)

#### If not using the shield then connect it this way:

![controller pinout](docs/psx.png)
![wiring](docs/Guncon_Voltage.png)
![leonardo icsp header](docs/icsp_header.png)

Before using it you will need the to install the libraries [PsxNewLib](https://github.com/SukkoPera/PsxNewLib) and [ArduinoJoystickLibrary](https://github.com/MHeironimus/ArduinoJoystickLibrary).
(I've included the libraries in this release)

This only works on a CRT at standard resolutions, native output setups such as CRTEmudriver.

The official guncon works perfectly.
With a 3rd party gun the readings are not correct. (might be fixed by customizing minmax values in the script)
 
### Credits

Original GunconDuino code [GunconDuino](https://github.com/sonik-br/GunconDuino) by sonic-br.

This piece of software would not be possible without the amazing [PsxNewLib](https://github.com/SukkoPera/PsxNewLib) by SukkoPera.

It also uses a modified version of [absmouse](https://github.com/jonathanedgecombe/absmouse) by jonathanedgecombe.

[ArduinoJoystickLibrary](https://github.com/MHeironimus/ArduinoJoystickLibrary) by MHeironimus.

PS controller pinout by [curiousinventor](https://store.curiousinventor.com/guides/PS2).
