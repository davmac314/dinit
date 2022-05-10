# Getting Started with Dinit

In this guide we will go through the steps required to set up a tiny user-mode
Dinit instance. When run as a regular user, Dinit can be used to supervise,
start, and stop other processes (services) also running under the same user ID.

We assume that Dinit has already been installed; i.e. we will only cover
configuration here. See [../BUILD](../BUILD) for information on how to
build Dinit from source and install it.

We don't cover how to set up Dinit as a system "init". See [linux/DINIT-AS-INIT.md](linux/DINIT-AS-INIT.md)
if you're interested in using Dinit as your system "init" or service manager.
However, you may still wish to read through this guide first, as it will give
an overview of Dinit's configuration and operation.

## Starting Dinit

The main component of Dinit is the `dinit` daemon. Let's start by running dinit
with the bare minimum configuration. First we need to choose a place for our
service descriptions to live. For this guide we are going to use the default
location (for user-mode), which is `~/.config/dinit.d`.

```
$ mkdir ~/.config/dinit.d
$ cd ~/.config/dinit.d
```

Next we need a service description for the `boot` service. This is the service
that dinit will bring up upon starting.

Create (using your preferred text editor) a file called `boot` (under the
directory we just made) and put as its contents:
```
type = internal
waits-for.d = boot.d
```

The first line (`type = internal`) tells dinit that this is not a service which
will run any external process directly.

The second line (`waits-for.d = boot.d`) specifies a directory which will be
used to keep track of which services should be started when dinit starts (or
more specifically, which services should be started before the `boot` service).
Here we've made it a `boot.d` sub-directory under our main `dinit.d` directory,
but it can be anywhere you like so long as you adjust the `waits-for.d` setting
appropriately.

Now, let's create the aforementioned directory and start dinit for the first time.
Assuming you are still in `~/.config/dinit.d`:
```
$ mkdir boot.d
$ dinit
[  OK  ] boot
```

Note: you may need to specify the full path to `dinit`, such as `/sbin/dinit`,
depending on details of your system and how Dinit was installed.

Dinit lives!

The `[  OK  ] boot` message tells us that the `boot` service we created has
been started. Note that dinit always starts the boot service by default,
unless you specify a different service or services on the command line.

In the long run, you'll want to find a way to invoke dinit at boot time, but
that's an exercise for the reader. For the moment, leave dinit running and
switch to another terminal. Or, if you would prefer to stick to one terminal,
press Ctrl+C (or the interrupt combination for your terminal) and dinit should
gracefully exit, then start dinit again, this time in the background:
```
$ dinit -q & 
```

Because `-q` suppresses service status messages, you won't see `[  OK  ] boot` as
you did when running dinit the first time.

## Adding Service Descriptions

So we have dinit running, but it currently has no services to supervise. Let's
give it something to do.

Suppose we want to run mpd, the music player daemon, under dinit. Put the
following in ` ~/.config/dinit.d/mpd`:
```
type = process
command = /usr/local/sbin/mpd --no-daemon
restart = true
```

This assumes, of course, that you have a suitable `mpd` at the specified path.
If you don't have mpd installed, you can use "sleep" for testing purposes;
change the `command` line above to:
```
command = /usr/bin/sleep 600
```
(you should double check the location of 'sleep' first; it may be in '/bin'!)

Now run `dinitctl list` (or `/sbin/dinitctl list`):
```
$ dinitctl list
[[+]     ] boot
```

The mpd service isn't visible yet because dinit lazily loads services. If we
start the service, we will see it in the list:
```
$ dinitctl start mpd
Service 'mpd' started.
$ dinitctl list
[[+]     ] boot
[[+]     ] mpd (pid: 14823)
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
[[+]     ] boot
[[+]     ] mpd (pid: 1667)
```

Notice that a new instance of mpd is running; it has a different pid.

You can stop a service using `dinitctl stop`:
```
$ dinitctl stop mpd
Service 'mpd' stopped.
$ dinitctl list
[[+]     ] boot
[     {-}] mpd
```

Here the "slider" for the mpd service has been moved to the right to signify
that it has been switched off.

## Starting Services at Startup

So far we've configured a service which can be brought up and down in an ad-hoc
fashion. This would be ideal for SSH tunnels, for example, but mpd is the kind
of daemon you want to *always* run; we want it to start when dinit itself
starts.

To that end, we can use `dinitctl enable mpd`. This will start the service
immediately *and* make sure it starts by default:
```
$ dinitctl list
[[+]     ] boot
[     {-}] mpd
$ dinitctl enable mpd
Service 'mpd' has been enabled.
$ dinitctl list
[[+]     ] boot
[{+}     ] mpd (pid: 49921)
```

Notice that the mpd status is shown as `{+}` rather than `[+]` as it was
earlier. This is because it is now started only as a dependency of boot -
we haven't explicitly marked it active (as is done via `dinitctl start`).
This means that if boot stops, mpd will also stop (and dinit will also
stop, seeing as it has no services left running).

We now want to restart dinit, to see that the mpd service does indeed start
automatically. First, stop dinit:
```
$ dinitctl shutdown
Shutting down dinit...
Connection closed.
```
(you could also send it the TERM signal using the `kill` command, or press Ctrl+C
in the terminal where it is running in the foreground; or, as alluded earlier, you
could stop the boot service via `dinitctl stop boot`).

Start dinit again (`dinit` in the other terminal, or `dinit -q &`, as before).
Then list services to make sure mpd started:
```
$ dinitctl list
[[+]     ] boot
[{+}     ] mpd (pid: 17601)
```

Success! - mpd was started when dinit started.

If you look in the `waits-for.d` directory we configured earlier, you will
find a symlink to the mpd service description file. This is how dinit keeps
track of what should be started by default.

## Further Reading

In this guide, we've really only scratched the surface of what Dinit can do.
For example, we've hardly touched on dependencies (where one service depends
upon another to function). For full details about service configuration, read
the `dinit-service(5)` manual page. The `dinit`, `dinitctl` and `dinitcheck`
commands also have manual pages.
