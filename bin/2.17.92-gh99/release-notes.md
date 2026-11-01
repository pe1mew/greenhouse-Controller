# TEST RELEASE 2.17.92-gh99: do not install

**A deliberately crash-looping image for the gh#99 test, on the `soak` channel only. Never promote it.**

It is 2.17.3 with `abort()` 5 s into every boot, so it never reaches the 30 s that mark a boot healthy. A unit that takes it crashes through three boots, and the fourth rolls it back to the bank it came from.

It exists to show that a release which crashes does not loop: the seq mark was set at the bank switch, so every later check ends `dl` 3.
