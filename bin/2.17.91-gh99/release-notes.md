# TEST RELEASE 2.17.91-gh99: do not install

**A deliberately broken release for the gh#99 test, on the `soak` channel only. Never promote it.**

The web assets zip holds DEFLATE entries. The SHA-256 passes and T13 refuses the zip (`ota_manager.cpp`, `extract_zip_store()`), so nothing is installed. The firmware in it is the 2.17.3 image under another name, and it does not run.

It exists to show what a unit does after a failed install:

- A fixed image (2.17.1 or later) leaves the seq mark unmoved, and retries at each check.
- An older image marks the seq as applied, and ends every later check `dl` 3.
