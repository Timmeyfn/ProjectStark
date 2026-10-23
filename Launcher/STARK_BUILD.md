# Project Stark Launcher

Based on [Reboot Launcher](https://github.com/Auties00/reboot_launcher) by Auties00 (GPL-3.0); this folder stays under the same license.

## What's different
- Project Stark name, window title, exe name (`ProjectStark.exe`) and red/gold colours
- Your locker (skin, pickaxe, glider, emotes) is saved between restarts
- Updates are checked against this repo instead of Reboot Launcher

## Building (Windows)
1. Install [Node.js LTS](https://nodejs.org) and [Flutter](https://docs.flutter.dev/get-started/install/windows), plus Visual Studio's "Desktop development with C++".
2. Double-click `auth_backend\build_stark.bat` (rebuilds the login server with the locker-save fix).
3. Open a Command Prompt in the `gui` folder and run:
   ```
   flutter pub get
   flutter build windows --release
   ```
4. The launcher is in `gui\build\windows\x64\runner\Release\ProjectStark.exe`. Right-click it → Send to → Desktop (create shortcut).

To change the icon, replace `gui\windows\runner\resources\app_icon.ico` and build again.
