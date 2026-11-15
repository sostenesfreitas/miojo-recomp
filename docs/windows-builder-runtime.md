# Incomplete Windows Builder runtime

The Windows Builder is published as a directory-based, self-contained .NET program (command-line only: it has no
window). Installing a global .NET
runtime does not replace files missing from that private runtime directory. Keep the complete installed package,
including its DLLs, dependencies and runtime configuration, together.

Setup checks the Builder executable and its required startup components before reusing an installed version and
before publishing a newly unpacked version. Missing files, empty files
and directories in place of those files fail that check. An incomplete existing installation goes through the
normal checked download/extraction path. Incomplete staging stops before the installed marker is written.

This checks the startup contract, not every optional runtime assembly or the contents/checksums of installed files.
The archive's SHA-256 verification remains separate. The required-file list in `builder/setup/setup.c` must change
with any future switch away from the directory-based publish format.

The setup installs only for `--cli` (RecompLauncher's prepare), so its reasons are `error <text>` lines on its stderr, not
dialogs.

If setup cannot complete the runtime, rerun the official setup and inspect the reported extraction/download error.
When the post-extraction check reports missing startup files, it includes the executable and staging folder paths.
Check the antivirus quarantine/history for files removed from that folder before retrying; repeatedly running setup
does not address files being quarantined again.
A game folder's `hookshot.exe` starts its game without the Builder, so a failed readiness check never stops play. The
fixtures in `tests/test_setup_runtime*.py` use authored files and intercept publication writes; they do not execute the Builder or prove that the original missing-file cause is resolved.
