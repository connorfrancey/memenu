Mirror's Edge Menu {version}
================================================================

A trainer, TAS and tools menu for Mirror's Edge (PC, 2009): no clip, flight, save states, a level / checkpoint loader,
collision view, macros and frame-by-frame TAS with a timeline, and more.

https://github.com/connorfrancey/memenu


INSTALL
-------
1. Close Mirror's Edge if it is running.
2. Run memenu_installer.exe and accept the administrator prompt (the game lives in Program Files).
   If Windows says "Windows protected your PC": click "More info", then "Run anyway". The installer isn't code-signed.
3. It finds the game by itself (Steam, EA app, other installs). If it doesn't, click Browse and pick MirrorsEdge.exe
   (in the game's Binaries folder).
4. Click Install, then Yes to start the game.

From then on the menu loads every time you start the game, however you start it.


USING IT
--------
INSERT      open / close the menu
END         unload the menu until the game is restarted

Click a module's name to open its settings, right click to close them. The switch on the right of a module turns it
on or off. Right click a setting to give it a hotkey. Everything is saved when the game closes.

Some modules change the game (No Clip, God Mode, Game Speed...). Turn them off before recording or playing a macro.


UNINSTALL
---------
Windows Settings > Apps > Installed apps > "Mirror's Edge Menu" > Uninstall
(or Control Panel > Programs and Features, or run memenu_installer.exe again and click Uninstall).

It asks whether to keep your settings and saved macros, then puts the game folder back the way it was.


TRY IT WITHOUT INSTALLING
-------------------------
Open the "try-without-installing" folder and run medge_launcher.exe (accept the administrator prompt). It starts
Mirror's Edge, or uses it if it's already running, and loads the menu into it for this session only: nothing is
installed, and closing the game removes it. Settings made this way are saved in that folder.

Don't use it while the installed menu is there: one copy at a time.


WHAT IT CHANGES
---------------
It adds a small loader (dinput8.dll) next to MirrorsEdge.exe and the menu in a "memenu" folder beside it. The game's own
files are not modified, so Steam updates and "Verify integrity of game files" leave it alone. A dinput8.dll from another
mod is kept and put back when you uninstall.

To start the game without the menu once: create an empty file called "disabled" in the memenu folder.


PROBLEMS
--------
- The menu doesn't open: make sure the game isn't set to run as administrator or in Windows XP compatibility mode
  (right click MirrorsEdge.exe > Properties > Compatibility).
- The game crashes or misbehaves: the log is memenu\logs\memenu.log in the game's Binaries folder. Please attach it
  when you report a problem: https://github.com/connorfrancey/memenu/issues
- Other overlays (Steam, Discord, MSI Afterburner) usually work alongside, but turn them off if the menu doesn't show.


LICENSE
-------
Mirror's Edge Menu is MIT licensed (LICENSE.txt). It includes third-party software under their own licenses
(THIRD-PARTY-NOTICES.txt). Mirror's Edge is a trademark of Electronic Arts; this project isn't affiliated with EA or DICE.
