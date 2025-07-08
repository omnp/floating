# floating
Basic digital paint program using C, xcb and libtiff (to save the images) supporting a graphics tablet and stylus in addition to a mouse.

The goal of this project thus far has been to implement a simple paint program in C using xcb (and in the process *learn* the basics of xcb).
It is *very* basic in the current state, for example there is no zooming of the "canvas" or anything like that (as this is "pure" xcb with no additional gui component library).
Worth mentioning is that AVX instruction support in the CPU is required in order to run the program. Some of the core image drawing related code uses those instructions (and they are written in gcc extended asm syntax).
As mentioned above the libtiff shared library is linked to in order to save the images.

A makefile is provided in order to build the program.
Dependencies are gcc (clang may work as well), libxcb, and libtiff, and of course make.
For example on a Fedora system, do a "dnf install gcc libxcb-devel libtiff-devel make".

Feel free to fork if you find anything interesting in here.

# Running the program
The binary is (logically) called draw.
Make sure that your graphics tablet is plugged in before you execute the program if you want to use it in the current painting session.
The stylus device part should be identified as containing 'stylus' or 'Stylus' in device name information otherwise it will not be used.
This is because many devices report many axes even if they are not a graphics tablet stylus.
The keybindings may be described in a later update to this document, currently your best bet is to read the source (the event loop at the end part of draw.c).
But to get you off the ground:
  * Hit 'b' to start brushing, and again to stop.
  * The numbers 1-5 select the colors red, green, blue, white, and black respectively.
  * The 's' key enables smudge mode and the 'p' key enables pick mode, hit again to disable them.

Also, when starting the program the canvas is completely transparent.
Accepted command line parameters are (in order): width height outputfilename.tif

Close the program using your window manager, but before you do you might want to save your artwork (in 'outputfilename.tif') by hitting the key combination 'shift-s'.

Have fun painting! :)
