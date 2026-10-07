#pragma once

// Runs the self-test, logging PASS or FAIL for every check.
//
// windows - also run the tests that create real windows and move them between
//           displays. They interrupt whatever is on screen, so --no-windows turns
//           them off; everything else (rotation arithmetic, icons, Start with
//           Windows, remap round trips) opens nothing and always runs.
//
// Returns 0 if every check ran and passed, 1 if any failed, and 3 if everything
// that ran passed but the window-move tests were skipped - asked for, or for want
// of a second display. (2 is left for usage errors, which main() reports before the
// self-test ever runs.)
int SelfTest(bool windows);
