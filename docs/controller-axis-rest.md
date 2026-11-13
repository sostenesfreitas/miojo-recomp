# Controller rest detection and camera inversion

SDL stick axes initially read as held remain suppressed until they actually rest inside the selected dead zone or
cross their resting position. This protects against a trigger or an unused axis mapped onto a stick, including a
second device whose held axis would otherwise dominate the player's controller.

The rest detector uses a stable axis orientation. Camera X/Y inversion applies to the admitted output, so changing
an inversion setting cannot impersonate physical movement and permanently admit a phantom axis (#2273). An axis
that already rested continues to respond immediately to inversion; real neutral and worn-stick crossing recovery,
the selected dead zone, and independent axes/devices retain their behavior.

`python -I -B tests/test_stick_inversion_rest.py` compiles the actual conversion/rest/output code over authored
device samples. It covers repeated settings changes with held axes, both right-stick directions, all six dead
zones, independent devices, genuine crossings, neutral recovery and the unchanged left-stick mapping.
It needs a C compiler, not a controller or game data; physical-controller validation is a separate test.
