# Requirements
  * rom not required to build
  * libsdl2-dev
  * python
  * [PSPSDK](https://pspdev.github.io/pspsdk/)
  * [PSP Media Engine Custom Core](https://github.com/mcidclan/psp-media-engine-custom-core) and [PSP Media Engine MIST](https://github.com/mcidclan/psp-media-engine-mist) installed to PSPSDK
  * sm-psp repo `git clone --recursive https://github.com/hulilikii/sm-psp`


# PSP

CD to your SM root folder and open the terminal and type:
```sh
make
```

For more advanced usage:
```sh
make -j$(nproc) # run on all core
make clean  # clear gen+obj and rebuild
CC=clang make   # specify compiler
```
