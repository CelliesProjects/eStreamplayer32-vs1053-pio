# eStreamPlayer32 VS1053 in PlatformIO

A web-based esp32 music player for webradio and mp3/ogg/aac/aac+ files from a local lamp or llmp server.
<br>Sound output comes from a separate VS1053 mp3/aac/ogg/wav decoder breakout board.

You control the music player with a browser on your phone, pc or tablet.

-  Supports http, https (insecure mode)/chunked streams.
-  Search for web radio stations on [radio-browser](https://www.radio-browser.info/) and save the results to your favorites.
-  No need for a display as the interface is completely network based.
-  Plays mp3, ogg, aac and aac+ streams.
-  Runs  on:<br>esp32<br>esp32-wrover<br>esp32-s2<br>esp32-s3

The interface is kept very simple, a playlist tab on the right and selectable source tabs on the left.<br>After 30 seconds of inactivity the file info overlay is shown, click on the overlay to hide it or use the info button on the bottom to toggle the overlay.<br>On this overlay you can also save the current playing search result to the favorites tab with this <img src="https://github.com/CelliesProjects/eStreamplayer32-vs1053-pio/assets/24290108/6e0d2706-45cd-4e5d-86ba-c194101afba7"> icon.


### Local file playback

eStreamPlayer is written for playback over http(s). What this means is that you will need a (lamp or llmp) webserver to play back your local files.<br>This is because the esp32 does not speak NFS or SMB which are common ways to share files over a network. Instead eStreamPlayer uses a php script on the server to navigate the music folders. Copy this script to the server to use your music library.<br>**This is totally insecure and should only be used on a trusted LAN!**

But if you don't have a local music server you can still use eStreamPlayer to tune in to web radio stations and add your own radio stations to presets and favorites.

### Webinterface screenshots

#### File info overlay

<img src="https://github.com/CelliesProjects/eStreamplayer32-vs1053-pio/assets/24290108/3d07fd0e-2cea-4ee7-acda-b85ffc17b79d" width="50%" alt="file overlay">

#### LIBRARY tab

<img src="https://github.com/CelliesProjects/eStreamplayer32-vs1053-pio/assets/24290108/9c1e57f1-41d5-445f-82dc-bde5ec84a0ff" width="50%" alt="file overlay">

#### WEBRADIO tab

<img src="https://github.com/CelliesProjects/eStreamplayer32-vs1053-pio/assets/24290108/d29e9863-afc2-43c0-a086-3a07e2b75729" width="50%" alt="file overlay">

#### FAVORITES tab

<img src="https://github.com/CelliesProjects/eStreamplayer32-vs1053-pio/assets/24290108/8993d7d0-d09b-469a-9a87-b0a22df34c9c" width="50%" alt="file overlay">

#### SEARCH tab showing results for 'techno' search
<img src="https://github.com/CelliesProjects/eStreamplayer32-vs1053-pio/assets/24290108/e68113c2-f02d-4fad-ad05-0724fae3ee1e" width="50%" alt="file overlay">

### First boot and setup

1.  Check and set your options in `platformio.ini`
2.  Add your WiFi credentials to `include/secrets.h` as shown below.<br>
```c++
#ifndef SECRETS
#define SECRETS

const char *SSID_NAME= "wifi_network";
const char *SSID_PASSWORD = "wifi_password";

#endif
```
3.  On first boot or after a flash erase the fatfs will be formatted. This will take from a couple of seconds up to a minute depending on the size of the filesystem.
<br>You can monitor the boot/formatting progress on the serial port.
<br>Flashing an update will not erase the fatfs data.
<br>**Note: Take care to select the same partition table when updating the app otherwise the fatfs partition will be formatted.**
4.  Browse to the ip address shown on the serial port.

### Setup for local file playback

-  A [lamp](https://en.wikipedia.org/wiki/LAMP_%28software_bundle%29) or llmp webstack is required to serve local files.
Apache2 and lighttpd were tested and should work.
-  The following file headers should be sent for supported filetypes:
<br>`MP3` `audio/mpeg`
<br>`OGG` `audio/ogg` or `application/ogg`
<br>`AAC` `audio/aac`
<br>`AAC+` `audio/aacp`
<br>With a vanilla LAMP setup this will be the default.
-  The php script `eSP32_vs1053.php` has to be copied to the root of the music library. The script is fairly version agnostic.

### Libraries used in the web interface

-   The free -as in beer- [radio-browser.info](https://www.radio-browser.info/) API is used for the search. The returned data is in the public domain.<br>See [de1.api.radio-browser.info](https://de1.api.radio-browser.info/) for API information.
-  The icons are from [material.io](https://material.io/tools/icons/?style=baseline) and are [available under Apache2.0 license](https://www.apache.org/licenses/LICENSE-2.0.html).
-  [Reconnecting WebSocket](https://github.com/joewalnes/reconnecting-websocket) which is [available under MIT licence](https://github.com/joewalnes/reconnecting-websocket/blob/master/LICENSE.txt).
-  [Google Roboto font](https://fonts.google.com/specimen/Roboto) which is [available under Apache2.0 license](https://www.apache.org/licenses/LICENSE-2.0.html).
-  [jQuery 3.4.1](https://code.jquery.com/jquery-3.4.1.js) which is [available under MIT license](https://jquery.org/license/).

The Google Roboto font and jQuery are fetched from the net. Your downloads will be tracked by the respective providers for this 'free' data. 

### License

MIT License

Copyright (c) 2023 Cellie

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
