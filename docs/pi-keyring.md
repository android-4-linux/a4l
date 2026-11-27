<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l documentation; gnome-keyring-daemon(1) and the freedesktop Secret
     Service API. No GPL-3.0 sources. -->

# The keyring on a Raspberry Pi 4 or 5

An app's `AndroidKeyStore` keys are kept in the user's keyring through the freedesktop Secret
Service (`src/svc/keystore`). On a desktop the login creates and unlocks that keyring and
nothing has to be set up. A Pi that boots to a console, or is reached over ssh, has no such
login: the keyring does not exist, a4l keeps keys in memory, and an app that registers a device
key (a messenger, for one) has lost it at the next start.

a4l says which it is using when an app starts:

```
keystore backend: secret-service (login keyring)     keys persist
keystore backend: memory (non-persistent)            keys are lost at exit
```

Checked on a Raspberry Pi 4 with Debian 13 (trixie) and the Raspberry Pi kernel,
gnome-keyring 48.0, from an ssh session. The steps were not run on a Pi 5; nothing in them is
specific to the board.

## Set up

Do this as the user that runs a4l, in that user's session: `XDG_RUNTIME_DIR` must be set and
the session bus must be the one a4l will use. Over ssh the bus is not always in the
environment:

```sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
```

1. Install the keyring daemon and the client library:

   ```sh
   sudo apt install gnome-keyring libsecret-1-0
   ```

2. Put the keyring's password in a file only that user can read. No newline at the end: the
   daemon takes every byte it is given as the password.

   ```sh
   mkdir -p ~/.config/a4l
   (umask 077; printf %s 'the password' > ~/.config/a4l/keyring-pass)
   ```

3. Create and unlock the keyring:

   ```sh
   gnome-keyring-daemon --replace --unlock --components=secrets < ~/.config/a4l/keyring-pass
   ```

   The first run creates `~/.local/share/keyrings/login.keyring` with that password. Later
   runs unlock it with the same one. An empty password creates nothing.

4. Run step 3 again in every session, before a4l starts. The keyring is locked after a reboot
   or a logout. The line belongs in whatever starts the session on the board, ahead of the
   compositor and a4l.

## Check

```sh
busctl --user get-property org.freedesktop.secrets \
    /org/freedesktop/secrets/collection/login org.freedesktop.Secret.Collection Locked
```

prints `b false` for a keyring that is there and unlocked. `acl-keystore-probe` (built with
a4l, not installed) stores a key, reads it back and erases it, and prints the backend it
used.

## What a4l does when it is not set up

| State | a4l | Log |
|---|---|---|
| No Secret Service on the bus | memory | `Secret Service unreachable (...)` |
| The daemon runs, no default keyring | memory | `the Secret Service has no default keyring (...)` |
| The keyring is locked and nothing can prompt | memory | `the default keyring is locked and was not unlocked` |
| The keyring is unlocked | Secret Service | `keystore backend: secret-service (login keyring)` |

The second and third lines include the command from step 3. Before a4l checked for these two
states it chose the Secret Service and every store failed.

a4l has to be built with libsecret (`libsecret-1-dev`; meson prints
`svc/keystore Secret Service backend enabled`). Without it the backend is always memory.

## Starting over

To start with a new password, stop the daemon, delete
`~/.local/share/keyrings/login.keyring` and repeat steps 2 and 3. Every key stored so far is
gone with the file, so each app has to register again. This was not tried.

## What the password protects

The keyring file is encrypted with the password, and the password sits in a file next to it
that the same user can read. That keeps the keys from another user of the board and from
someone who has only the keyring file. It does not keep them from anyone who can read that
user's home directory.
