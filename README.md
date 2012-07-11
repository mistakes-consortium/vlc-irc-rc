vlc-irc-rc
==========

Control your VLC with your IRC, bro.

building
--------

I removed the Makefile because out-of-tree compilation was too finnicky and
I couldn't come up with build instructions that worked consistently across
distros.

* Download the latest VLC sources (http://www.videolan.org/vlc/download-sources.html) and install its build dependenices

* Extract and build vlc (./configure && ./compile)

* Copy src/ircrc.c to modules/control/ in the vlc source directory

* Add the following lines to modules/control/Modules.am:

Add

<pre>
SOURCES_ircrc = ircrc.c
</pre>

With the rest of the SOURCES_* defines, and add

<pre>
libvlc_LTLIBRARIES += \
        libircrc_plugin.la
</pre>

To the end.

* Run `make clean' in modules/control

* Now run `make' in the root of the vlc source folder (:

usage
-----

<pre>
vlc -I irc --server your.irc.server --channel "#yourchannel" --nick
some_nickname yourfile.avi
</pre>

commands
--------

* >play
* >pause
