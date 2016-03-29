# twitchnotify

### Dependencies
* libcurl
* libnotify
* GdkPixbuf2

### Install

Straightforward. 

```
make
sudo make install
```

### Run

Pass in up to 100 Twitch usernames. Use the --no-daemon option
to prevent forking.

```
twitchnotify --no-daemon kueller917 xangold test_channel
```