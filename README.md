metar
=====
This is a tool to fetch meterological information from the National Oceanic and Atmospheric Administration's (NOAA) [Aviation Weather Center](http://aviationweather.gov/adds/metars/ "ADDS - METARs").

Prerequisites:

* libcurl-dev
* libxml2-dev

How to build:

    make

Yep, it's that simple.  And to install:

    sudo make install

For help, type:

    metar -?

Notes:

* This should work for most UNIX-like platforms.  If anyone's willing to get it working on Windows, that'd be great too.
* I've only tested the code on gcc and clang so far.
* Yes, the code is horribly messy.  Sorry about that.
* I wrote this because I felt like it, and because it's a convenient tool for me.  But if you want to help improve it, you are awesome and very welcome to do so.

License
=====
Copyright (c) 2013, Nijumi "Ninja" Ardetus-Libera.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

* Neither the name of the software nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

