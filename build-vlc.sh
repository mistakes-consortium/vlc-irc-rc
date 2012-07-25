#!/bin/sh
VLC_URL=http://download.videolan.org/pub/videolan/vlc/2.0.3/vlc-2.0.3.tar.xz
CWD=$(pwd)

mkdir build
cd build
wget $VLC_URL
tar xfJ vlc-2.0.3.tar.xz
cd vlc-2.0.3
./configure
cp $CWD/src/ircrc.c modules/control/
echo -ne 'SOURCES_ircrc = ircrc.c\nlibvlc_LTLIBRARIES += libircrc_plugin.la\n' >> modules/control/Modules.am
./bootstrap
./configure
make

