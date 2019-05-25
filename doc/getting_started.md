# Getting Started with Dinit

In this guide we will go through the steps required to set up a tiny user-mode
dinit instance.

We assume that dinit has already been installed; i.e. we only cover
configuration here.

## Starting Dinit

Let's start by running dinit with the bare minimum configuration. First we need
to choose a place for our service descriptions to live. For this guide we are
going to use the default location (for user-mode), which is `~/dinit.d`.

```
$ mkdir ~/dinit.d
$ cd ~/dinit.d
```

Next we need a service description for the `boot` service. This is the service
that dinit will bring up upon starting.

In a file called `boot` (under the directory we just made) put:
```
type = internal
waits-for.d = boot.d
```

The first line tells dinit that this is not a service which will run anything
directly.

The second line specifies a directory which will be used to keep track of which
services should be enabled at dinit start time. Here we've made it a `boot.d`
sub-directory under our main dinit directory, but it can be anywhere you like.

Now let's make the aforementioned directory and start dinit for the first time.
Assuming you are still in `~/dinit.d`:

```
$ mkdir boot.d
$ dinit -d .
[  OK  ] boot
```

Dinit lives!

In the long run, you'll want to find a way to invoke dinit at boot time, but
that's an exercise for the reader.

## Adding Service Descriptions

So we have dinit running, but it currently has no services to supervise. Let's
give it something to do.

Suppose we want to run mpd under dinit. Put the following in ` ~/dinit.d/mpd`:
```
type = process
command = /usr/local/sbin/mpd --no-daemon
restart = true
```

Now run `dinitctl list`:
```
$ dinitctl list
[{+}     ] boot
```

The mpd service isn't visible yet because dinit lazily loads services. If we
start the service, we will see it in the list:
```
$ dinitctl start mpd
Service started.
$ dinitctl list
[{+}     ] boot
[{+}     ] mpd (pid: 14823)
```

Now let's simulate mpd crashing and check dinit brings it back up:
```
$ kill 14823
```

On the dinit log, we see:
```
[STOPPD] mpd
[  OK  ] mpd
```

And if we query dinit for its status, we see:
```
$ dinitctl list
[{+}     ] boot
[{+}     ] mpd (pid: 1667)
```

Notice that a new instance of mpd is running; it has a different pid.

You can stop a service using `dinitctl stop`:
```
$ dinitctl list
[{+}     ] boot
[     {-}] mpd
```

Here the "slider" for the mpd service has been moved to the right to signify
that it has been switched off.

## Starting Services at Startup

So far we've configured a service which can be brought up and down in an ad-hoc
fashion. This would be ideal for (for example) SSH tunnels, but mpd is the kind
of daemon you want to *always* run.

To that end, to start a service at the time dinit starts, we can use `dinitctl
enable`. This will start the service immediately *and* make sure it starts by
default:
```
$ dinitctl list
[{+}     ] boot
$ dinitctl enable mpd
$ dinitctl list
[{+}     ] boot
[{+}     ] mpd (pid: 49921)
```

If we now restart dinit:
```
$ dinitctl list
[{+}     ] boot
[{+}     ] mpd (pid: 17601)
```

mpd was started when dinit started.

And if you look in the `waits-for.d` directory we configured earlier you will
find a symlink to the mpd service description file. This is how dinit keeps
track of what should be started by default.

## Further Reading

In this guide, we've really only scratched the surface of what dinit can do.
For example, we've not even touched on dependencies (where one service depends
upon another to function). Next it'd be good to read the `dinit-service(5)` and
`dinitctl(8)` manual pages.
