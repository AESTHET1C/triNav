![INAV](http://static.rcgroups.net/forums/attachments/6/1/0/3/7/6/a9088858-102-inav.png)

# triNav - iNav, but for tricopters!

triNav is a franken-fork of [iNav](https://github.com/iNavFlight/inav) and [Triflight 0.5](https://github.com/lkaino/Triflight/releases/tag/0.5), a long-discontinued tricopter-specific fork of Cleanflight. This allows for proper tricopter manoeuvring using bespoke control algorithms.

Many thanks to [lkaino](https://github.com/lkaino) for driving Triflight's original development and [jihlein](https://github.com/jihlein) for their efforts merging Triflight into iNav and Betaflight years after its abandonment. This fork wouldn't have happened without them.

triNav is almost entirely untested, without support, and without promise of future releases. I do not have the free time to maintain it, nor the interest. I hope what I put forth is helpful, but expect radio silence.

If you're looking for an actual hex file, you're going to have to wait or compile things yourself. If you're familiar with Linux, just install Docker and run the build script.

# INAV Community

* [INAV Discord Server](https://discord.gg/peg2hhbYwN)
* [INAV Official on Facebook](https://www.facebook.com/groups/INAVOfficial)
* [INAV Official on Telegram](https://t.me/INAVFlight)

## Features

* **Proper tricopter support!**
* Runs on the most popular F4, F7 and H7 flight controllers
* On Screen Display (OSD) - both character and pixel style
* DJI OSD integration: all elements, system messages and warnings
* Outstanding performance out of the box
* Position Hold, Altitude Hold, Return To Home and Missions
* Excellent support for fixed wing UAVs: airplanes, flying wings 
* Fully configurable mixer that allows to run any hardware you want: multirotor, fixed wing, rovers, boats and other experimental devices
* Multiple sensor support: GPS, Pitot tube, sonar, lidar, temperature, ESC with BlHeli_32 telemetry
* SmartAudio and IRC Tramp VTX support
* Blackbox flight recorder logging
* Telemetry: SmartPort, FPort, MAVlink, LTM
* Multi-color RGB LED Strip support
* Advanced gyro filtering
* Logic Conditions, Global Functions and Global Variables: you can program INAV with a GUI
* And many more!

For a list of features, changes and some discussion please review consult the releases [page](https://github.com/iNavFlight/inav/releases) and the documentation.

## Tools

### INAV Configurator

Official tool for INAV can be downloaded [here](https://github.com/iNavFlight/inav-configurator/releases). It can be run on Windows, MacOS and Linux machines and standalone application.  

### INAV Blackbox Explorer

Tool for Blackbox logs analysis is available [here](https://github.com/iNavFlight/blackbox-log-viewer/releases)

### Telemetry screen for OpenTX

Users of OpenTX radios (Taranis, Horus, Jumper, Radiomaster, Nirvana) can use INAV OpenTX Telemetry Widget screen. Software and installation instruction are available here: [https://github.com/iNavFlight/OpenTX-Telemetry-Widget](https://github.com/iNavFlight/OpenTX-Telemetry-Widget)

### INAV magnetometer alignment helper

[INAV Magnetometer Alignment helper](https://kernel-machine.github.io/INavMagAlignHelper/) allows to align INAV magnetometer despite position and orientation. This simplifies the process of INAV setup on multirotors with tilted GPS modules.

### OSD layout Copy, Move, or Replace helper tool

[Easy INAV OSD switcher tool](https://www.mrd-rc.com/tutorials-tools-and-testing/useful-tools/inav-osd-switcher-tool/) allows you to easily switch your OSD layouts around in INAV. Choose the from and to OSD layouts, and the method of transfering the layouts.

## Documentation, support and learning resources
* [INAV 5 on a flying wing full tutorial](https://www.youtube.com/playlist?list=PLOUQ8o2_nCLkZlulvqsX_vRMfXd5zM7Ha)
* [INAV on a multirotor drone tutorial](https://www.youtube.com/playlist?list=PLOUQ8o2_nCLkfcKsWobDLtBNIBzwlwRC8)
* [Fixed Wing Guide](docs/INAV_Fixed_Wing_Setup_Guide.pdf)
* [Autolaunch Guide](docs/INAV_Autolaunch.pdf)
* [Modes Guide](docs/INAV_Modes.pdf)
* [Wing Tuning Masterclass](docs/INAV_Wing_Tuning_Masterclass.pdf)
* [Official documentation](https://github.com/iNavFlight/inav/tree/master/docs)
* [Official Wiki](https://github.com/iNavFlight/inav/wiki)
* [Video series by Paweł Spychalski](https://www.youtube.com/playlist?list=PLOUQ8o2_nCLloACrA6f1_daCjhqY2x0fB)
* [Target documentation](https://github.com/iNavFlight/inav/tree/master/docs/boards)

## INAV Releases
https://github.com/iNavFlight/inav/releases
