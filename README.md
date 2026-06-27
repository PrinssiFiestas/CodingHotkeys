# Coding Hotkeys

AutoHotkey and X11 scripts to increase programming productivity on non-Vi text editors. Made for Finnish keyboard layout and mostly tested with Notepad++ on Windows and Kate on X11, but should be easily modified for other layouts and text editors. See the scripts for details. 

### winvi.ahk

Vim inspired keybinds anywhere on Windows. Dodgier keybinds that are only tested to work with Notepad++ are context sensitive. 

### hotkeys.c

Incomplete port of `winvi.ahk` to X11. Only implements Left+f, Left+t, Right+f, and Right+t at the time of writing. Compile with `cc hotkeys.c -lX11`. 

### keymouse.ahk

Control your mouse using your keyboard. 
