# hexaOS apps (Flatpak)

Apps for hexaOS that are **not** built into the system image. Each folder is one
Flatpak app (C + GTK 3 on the GNOME runtime). You build and install it on hexaOS
itself, from VS Code or the terminal, and it updates without rebuilding the image.

```
hexa-app                     build / install / run / bundle / uninstall an app
hexa-launcher                adds or removes an app's Start menu entry
com.hexaos.Store/            the hexaOS Store: Flathub search, install, update, remove
                             (also built into the image as /usr/bin/hexa-store,
                             package/hexa-store, so it comes preinstalled)
```

Each app folder has the same layout:

```
com.hexaos.NAME/
  com.hexaos.NAME.yml        Flatpak manifest (runtime, permissions, build steps)
  src/                       the program and its Makefile
  data/                      .desktop file, metainfo, icon
```

## First time

```bash
apps/hexa-app setup          # flatpak-builder + GNOME runtime/SDK from Flathub (~1 GB)
```

## Build, install and run

In VS Code: Ctrl+Shift+P > "Tasks: Run Task" > **App: Install**, **App: Run**,
**App: Install and run**, **App: Bundle** or **App: Uninstall**, then pick the app.
Or in the terminal:

```bash
apps/hexa-app install com.hexaos.Store    # build, install for this user, add to the Start menu
apps/hexa-app run com.hexaos.Store        # open it (its messages show in the terminal)
apps/hexa-app bundle com.hexaos.Store     # apps/com.hexaos.Store.flatpak for another machine
apps/hexa-app uninstall com.hexaos.Store
```

Builds go to a local Flatpak repository (`apps/.build/repo`, ignored by git),
added as the `hexaos-local` remote, so `flatpak update` and the Store's Updates
page also pick up new builds.

The Start menu is read when the desktop starts: after installing, restart the
desktop (the Store has a button for it, or sign out).

## A new app

1. Create `apps/com.hexaos.MyApp/` with the layout above; the Store's folder
   is a working example of each file.
2. Use `com.hexaos.MyApp` everywhere: the folder, the manifest file name and
   its `id`, the `.desktop` and metainfo file names, and the GTK application
   id in the program (it must be the same as the Flatpak id).
3. Add `com.hexaos.MyApp` to `inputs > app > options` in `.vscode/tasks.json`.
4. Ask only for the permissions the app needs in `finish-args`
   (see the Store's manifest for host commands and files).

## How the Start menu entries work

Weston's Start menu lists every `[launcher]` section of its config. hexa-session
builds that config at login from `~/.config/hexaos/desktop.ini` (Settings),
`/etc/xdg/weston/weston.ini` (system apps) and `~/.config/hexaos/apps.ini`
(Flatpak apps). `apps.ini` holds one block per app, marked with `app-id=`;
`folder=Apps` puts them all in the **Apps** folder of the Start menu
(patches/weston/0003; older images ignore the key and list them directly):

```ini
# app: org.vinegarhq.Sober
[launcher]
displayname=Sober
icon=/home/hexa/.config/hexaos/icons/org.vinegarhq.Sober.png
path=/usr/bin/flatpak run org.vinegarhq.Sober
app-id=org.vinegarhq.Sober
folder=Apps
```

The Store and `hexa-launcher` write it; icons must be PNG (the Store makes a
64×64 copy in `~/.config/hexaos/icons/`). Without a window, the Store can do it
too: `flatpak run com.hexaos.Store --add-to-menu ID` / `--remove-from-menu ID`.

## Notes

- `hexa-app` runs flatpak-builder from Flathub (`org.flatpak.Builder`), so the
  system does not need it, and passes `--disable-rofiles-fuse` because the FUSE
  module is not loaded by default.
- Apps open links and files through the portal (`org.freedesktop.portal.Desktop`):
  hexaOS has a small one, `package/hexa-portal` (OpenURI and Settings only), so
  portals for file choosers, screenshots and such are not there.
