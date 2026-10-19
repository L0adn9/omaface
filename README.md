# Face ID for Omarchy

Windows Hello-style face unlock for [Omarchy](https://omarchy.org) 4, using the
infrared camera many laptops already have. Glance at the camera to unlock the
lock screen, sign in at the login screen, answer `sudo`, or approve system
prompts. Your password keeps working everywhere, always.

It looks and moves like the rest of Omarchy: the lock screen and polkit dialog
are Omarchy's own, with a Face ID mark that scans, draws a check mark when it
knows you, and shakes when it does not.

```
git clone https://github.com/L0adn9/omarchy-faceid ~/.local/src/omarchy-faceid
cd ~/.local/src/omarchy-faceid && ./install.sh
```

Then open **Face ID** from the app launcher (or run `faceid settings`) and
choose **Set up Face ID**.

## What you need

* **Omarchy 4** or newer (the Quickshell lock screen).
* **An infrared (Windows Hello) camera.** A normal webcam is not used: a photo
  can fool it. `faceid cameras` shows what your laptop has.
* About 120 MB for OpenCV if it is not installed yet. The installer lets
  pacman list what it adds and asks first.

Developed on an ASUS laptop. The camera code handles the formats IR cameras
use (greyscale `GREY`/`Y16`, and `YUYV` when the camera calls itself IR) and
picks a frame size near 640 wide, so most laptops with a Windows Hello camera
should work. If yours does not, open an issue with the output of
`faceid cameras` and `faceid emitter`.

## Commands

```
faceid                     status
faceid settings            the Face ID window
faceid enroll              add your face (up to 3 appearances, e.g. with glasses)
faceid test                look at the camera and see the scores
faceid rename <N> <name>   rename an appearance
faceid remove <N|all>      remove one, or all
faceid enable|disable lock login sudo polkit
faceid set attention on|off
faceid cameras             cameras on this machine and which one is used
faceid emitter             check that the IR light comes on
faceid doctor              check every part
faceid uninstall           put everything back the way Omarchy had it
```

## Where it is used

| Place  | When it scans | Notes |
|--------|---------------|-------|
| lock   | on activity after locking (key, pointer, a click on the mark), and right after waking from sleep | a lock you just asked for is not undone while you still sit there |
| login  | when the login screen appears; Enter on the empty field scans again | only with SDDM as the login manager, and only once a face is enrolled |
| sudo   | when sudo asks | Enter skips to the password; typed keys are not echoed while it looks |
| polkit | when the dialog opens | the dialog shows the Face ID mark, then the password field |

Each one can be switched off in the settings window or with `faceid disable`.

## Require attention

On by default. Face ID only unlocks when **both eyes are open and looking at
the camera**, so it does not open for someone asleep, looking at their phone,
or turned away.

It uses the IR light itself. The light sits beside the lens, so an eye looking
into the camera reflects it straight back off the retina and the whole pupil
glows (the same effect as red-eye in a flash photo). Closed, lowered or
averted eyes show no glow. The head's angle, compared with how it sat when you
enrolled, catches looking up or away.

What that means in practice:

* Glance at the camera; the lock screen says so while it scans.
* The glow only shows within a few degrees of the camera, so looking at the
  middle or bottom of the screen usually does not count.
* Eyes closed, looking at the keyboard, squinting, or looking to the side do
  not unlock. A miss never counts toward the lockout below.
* Looking just past the camera with your eyes only (head still) cannot be told
  apart from looking at it.
* Some glasses throw glare over the eyes. If Face ID recognizes you
  (`faceid test` says so) but never unlocks, turn attention off in the
  settings window.

## How it works

```
 lock screen ─┐   PAM: omarchy-lock-face
 polkit ──────┤   PAM: polkit-1       one line:  auth sufficient pam_exec.so
 sudo ────────┤   PAM: sudo                      /usr/local/lib/faceid/faceid-pam
 login ───────┘   PAM: sddm
                        │  exit 0 only on "RESULT ok"
                        ▼
              /run/faceid.sock   (systemd socket, Accept=yes)
                        │  one short-lived process per scan, sandboxed
                        ▼
              faceid-engine serve
                IR camera (V4L2, IR-lit frames only)
                YuNet face detection → SFace 128-d embedding
                cosine match against /var/lib/faceid/<user>/faces.yml
                attention: pupil glow on both eyes + head pose
                2 frames in a row → ok
```

* **Nothing stays in memory.** The engine starts when something asks, looks
  for a few seconds and exits (about 100 MB while scanning, nothing at rest).
  No daemon. OpenCL is off, so a discrete GPU is never woken.
* **The camera turns off as soon as the answer is known**, or within about a
  fifth of a second when a prompt is cancelled or the settings window closes.
* **The password always works.** Each Face ID PAM line is `sufficient` and
  sits directly above the password line: success skips the password, and
  anything else (no face, wrong face, timeout, lid closed, camera busy, engine
  broken, service stopped) falls straight through to it. Omarchy's fingerprint
  lines, if you use them, keep their place above it.
* **Everything stays on the machine.** No network access, no images saved.
* **The shell plugin `l0adn9.faceid`** replaces `omarchy.lock` and
  `omarchy.polkit`. It is built on your machine from Omarchy's installed files
  plus small anchored patches (`tools/patches.py`), not shipped as a frozen
  copy, and the build refuses to produce a half-patched lock screen.

## Security, honestly

* The IR camera ignores phone and monitor screens (they give off no infrared),
  and attention needs live eyes reflecting the camera's own light. There is
  still no depth sensor: a good enough infrared-visible replica, or a close
  relative, might pass. Treat it as convenient, not as strong as your
  password. `faceid set threshold 0.6` makes matching stricter (default 0.50;
  `faceid test` shows your scores).
* Five scans that see a face that is not yours pause Face ID for five
  minutes; the password still works.
* Face templates are root-owned (`/var/lib/faceid/<user>`, mode 0640, group =
  your user), so nothing running as you can swap in another face. Only root
  can enroll, rename or remove.
* sudo and polkit only accept a face while you have an active local session.
  sudo from SSH or another remote session never uses the camera. If you run
  sshd, note that a polkit prompt raised remotely could still be answered by
  whoever sits at the laptop; `faceid disable polkit` avoids that.
* The scan service runs with no capabilities, no network, a read-only
  filesystem apart from `/run/faceid`, and access to video devices only.

## Troubleshooting

* **No IR camera found**: `faceid cameras`. If your IR camera is listed but
  not picked, set `device=/dev/videoN` in `/etc/faceid/config`.
* **"No face found" all the time**: `faceid emitter`. Many Lenovo, Dell and HP
  laptops keep the IR light off until it is switched on once:
  `omarchy pkg aur add linux-enable-ir-emitter`, then
  `sudo linux-enable-ir-emitter configure`.
* **Recognized but never unlocks**: look into the camera; if it still fails,
  `faceid test` says whether it saw your eyes. Try `faceid set attention off`.
* **Lock screen misbehaving**: `omarchy plugin disable l0adn9.faceid` (brings
  back `omarchy.lock`), `omarchy plugin enable omarchy.polkit`, then
  `omarchy restart shell`. The same commands work from a TTY (Ctrl+Alt+F3).
* **Login screen misbehaving**: `sudo rm /etc/sddm.conf.d/20-faceid.conf`
  returns to Omarchy's theme. SDDM also falls back to its built-in theme on a
  broken one.
* `faceid doctor` checks every part and names the fix.

## Updates

* **OpenCV upgrade**: a pacman hook rebuilds the engine and swaps it in only
  after it passes a self-test.
* **Omarchy update**: a post-update hook runs `faceid doctor --after-update`.
  If Omarchy changed its lock screen or polkit dialog, the plugin is rebuilt
  from the new files; if that cannot be done cleanly, the working copy stays.
  If the plugin ever fails to load, Omarchy's own lock screen and polkit
  dialog are switched back on, so you are never left without a lock.
* **Face ID itself**: `git pull && ./install.sh`. Enrolled faces and settings
  are kept.

## Files

| Path | What |
|------|------|
| `/usr/local/lib/faceid/` | `faceid-engine`, `faceid-pam`, `rebuild`, `uninstall` |
| `/usr/local/bin/faceid` | the command |
| `/usr/local/share/faceid/` | models (checksummed), plugin sources and build tool |
| `/usr/local/src/faceid/` | engine sources for the OpenCV rebuild hook |
| `/etc/faceid/config` | places, attention, threshold, timeouts |
| `/var/lib/faceid/` | enrolled faces, PAM backups |
| `/etc/systemd/system/faceid.socket`, `faceid@.service` | the scanner |
| `/etc/pam.d/{sudo,polkit-1,sddm}` | one marked line each; `omarchy-lock-face` |
| `/usr/share/sddm/themes/omarchy-faceid/`, `/etc/sddm.conf.d/20-faceid.conf` | login |
| `/etc/pacman.d/hooks/faceid.hook` | rebuilds the engine when OpenCV upgrades |
| `~/.config/omarchy/plugins/l0adn9.faceid/` | lock screen, polkit dialog, settings |
| `~/.config/omarchy/hooks/post-update.d/faceid` | runs the doctor after updates |

`faceid uninstall` removes all of it (`--keep-data` keeps enrolled faces) and
puts PAM, the lock screen, polkit and the login screen back as they were.

## Credits

Built on [Omarchy](https://github.com/basecamp/omarchy) (MIT), the
[YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet)
face detector (MIT) and
[SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface)
recognizer (Apache 2.0) from OpenCV Zoo, and [OpenCV](https://opencv.org).
See `NOTICE`. Face ID for Omarchy is MIT licensed.
