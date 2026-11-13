"""Hookshot Builder, in Python: the same window as the WPF builder (builder/HookshotBuilder), run by python.org's own python.exe and
Qt's DLLs (both signed by their makers), with no executable of Hookshot's own to download. It also does what the setup exe did
(builder/setup/setup.c): it reads a release's hookshot-setup.txt, downloads the parts it names, checks each one's SHA-256 and
unpacks them into %LOCALAPPDATA%\\HookshotBuilder\\versions\\<tag>."""
APP_ID = "NotRexed.Hookshot.Builder"   # (Windows' AppUserModelID: the taskbar groups and shows the window as Hookshot, not Python)
REPO = "Hookshot/Hookshot"
