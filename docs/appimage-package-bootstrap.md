# AppImage package bootstrap recovery

The Ubuntu 22.04 CI container first refreshes its base indexes and installs the tools needed to enroll the
toolchain PPA. This phase retries independently, at most three times.

The PPA phase then enrolls or repairs the repository before explicitly refreshing its signed indexes and installing
the toolchain/dependencies. Enrollment uses `--no-update` to separate these operations. It does not bypass signature
validation: dependent packages are installed only after the signed refresh succeeds.

This ordering matters after a failed signing-key request (#2266). Enrollment may already have written the source;
an initial `apt-get update` then rejects its missing key. Repeating that refresh before the enrollment repair cannot
recover, even if Launchpad is available again. Each PPA-phase retry starts with the operation that can repair it.

Both phases make at most three attempts, wait 20 seconds between retries and retain the final failing status. No repository/key deletion or apt
authentication override is used. The target remains the same ephemeral CI container and the same package set.

Each enrollment is limited to 180 seconds, followed by up to 5 seconds before forced termination. A Launchpad metadata request must not
consume the entire one-hour job before a retry can run. A transient timeout re-enters enrollment to repair partial
state; persistent timeouts retain status124 (or the eventual signal status if forced killing is needed). This bounds
enrollment, not the separate apt refresh/install operations or Launchpad availability.

`python -I -B tests/test_appimage_package_retry.py` executes the workflow's real shell under `sh -e` with authored
command stand-ins. They model a source left behind without its key, require valid signed indexes before installation,
and exercise transient/persistent failures at every phase boundary. Three cases also use real GNU timeout over
authored stalled children with an accelerated clock. No real network or package manager is run.
