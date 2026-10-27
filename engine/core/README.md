# omaface-core

The face recognition engine shared by [omaFace](https://github.com/L0adn9/omaface)
(face unlock for Omarchy on Linux) and Nod (face unlock for macOS).

It does the parts that must behave the same everywhere:

* **Detection and recognition**: YuNet finds faces, SFace turns one into a
  128-number template. A face is compared with the five closest samples of
  each enrolled appearance (the mean of the top five, not the single best).
* **Whole faces only**: a face cut by the side or top of the picture is not
  judged; only the chin may leave it, at the bottom.
* **One person at a time**: a second face in view, or a clear stranger,
  stops the run for a second; a face that jumps starts it again.
* **Attention**: eyelids open (MediaPipe iris landmarks) and eyes on the
  screen (a Gaze360 gaze model), relative to how the person looked at
  enrollment.
* **Webcam liveness**: two Silent-Face anti-spoofing models (MiniFASNet V2
  and V1SE) on every matching frame from a regular camera.
* **The verdict**: how many frames in a row count as a match, and what a scan
  that ran out of time amounts to (a strike, a sticky strike, or nothing).

It does not open cameras, talk to PAM, or decide where files live. Each app
feeds it frames and acts on its answers.

## Use

```cpp
#include "omaface/core.hpp"

omaface::FaceModels models;
models.load("/usr/share/omaface/models", err);
auto gallery = omaface::readGallery(path);
omaface::Verifier v(rules, gallery, models, &attention, &liveness);
for (each usable frame) {
  std::string status = v.verify(bgr, seconds);  // "match" ends the run
}
auto outcome = v.outcome();  // when the time ran out
```

The apps vendor this repository with `git subtree` (omaFace keeps it in
`engine/core`), so their source archives build without fetching anything.

## Tests

```
make test
```

The tests need OpenCV 4.5+ (or 5) but no models: they cover face geometry,
matching and every verification rule, with cases taken from measurements on
real cameras (box and landmark coordinates only, no face data).

## Models

Not included. The apps download them from their authors' releases and check
them by hash; file names are in `kDetectorFile` and friends.

## License

MIT, see LICENSE.
