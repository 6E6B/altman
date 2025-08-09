<div align="center">
    <img src="src/assets/images/256x256.png"
            alt="Picture"
            width="256"
            height="256"
            style="display: block; margin: 0 auto" />

<h1>AltMan</h1>
<p>AltMan is a robust and intuitive tool designed to help you manage multiple Roblox accounts effortlessly.
</p>
</div>

---

## Features

- **Multi-Account Management** – Add, organize, and securely store cookies for multiple Roblox accounts.
- **Quick Join** – Instantly join games via **JobID** or **PlaceID**.
- **Friends Integration** – View and manage friends per account.
- **Friend Requests** – Send friend requests directly from the interface.
- **Server Browser** – Explore active Roblox game servers.
- **Advanced Filtering** – Sort servers by ping or player count.
- **Game Discovery** – Search Roblox games by title or keyword.
- **Log Parser** – Convert Roblox logs into a human-readable format.

---

## Preview

![AltMan Preview](src/assets/images/screenshot.png)

---

## Usage Guide

### Adding Accounts

1. Launch **AltMan**.
2. Navigate to `Accounts` on the menu bar.
3. Click `Add Account` > `Add Via Cookie`.
4. Paste your cookie and click **Add Cookie**.

### Joining Games

- **By JobID**: Enter the JobID in the Quick Join field.
- **By PlaceID**: Use a valid PlaceID to connect to a server.
- **By Username**: Connect directly to a user's session (if joins are enabled).

> 💡 **Tip**: You can also join games through the **Servers** or **Games** tabs.

### Managing Friends

1. Select an account from the list.
2. Go to the **Friends** tab to see the current friend list.
3. Use the **Add Friend** button to send requests via username or UserID.

---

## Requirements

- Windows 10 or 11 (Tested for Windows 11 24H2)
- Active internet connection

## Building from Source

### Prerequisites

- Visual Studio 2022 (or Build Tools) with the **Desktop development with C++** workload
- CMake ≥ 3.25
- [vcpkg](https://github.com/microsoft/vcpkg) (any location; set the `VCPKG_ROOT` environment variable)
- Git

### 1. Clone the repository

```bat
git clone https://github.com/crowsyndrome/AltMan.git
cd AltMan
```

### 2. Bootstrap vcpkg (if you do not already have it)

```bat
git clone https://github.com/microsoft/vcpkg.git %USERPROFILE%\vcpkg
%USERPROFILE%\vcpkg\bootstrap-vcpkg.bat
```

### 3. Install dependencies

The project ships with a `vcpkg.json` manifest. With vcpkg on your `%PATH%` simply run:

```bat
%USERPROFILE%\vcpkg\vcpkg.exe install
```

(If you skip this step vcpkg will build the ports automatically the first time CMake configures.)

### 4. Build with plain CMake (command-line)

```bat
mkdir build
cmake -B build -S . ^
  -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -- /m
```

The executable will be generated at `build\altman\altman.exe` together with the required `assets` folder.

### 5. (Optional) Build from CLion

1. Open the project folder in CLion.
2. Go to **File ▸ Settings ▸ Build, Execution, Deployment ▸ CMake** and add\
   `-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake` to _CMake options_.
3. Make sure the **Toolchain** is set to the _Visual Studio_ toolchain (x86_64).
4. Press ▶️ to run the `altman` target.

---

## Security

- Your account cookies are **stored locally and encrypted**.
- All save files are kept inside a **storage** folder in the application's directory.
- **Never** share your cookies with anyone.
- Use the tool at your own risk.

---

## License

This project is licensed under the **MIT License**. See the `LICENSE` file for full details.

---

## Contributing

Pull requests are welcome! For substantial changes, please open an issue to discuss the proposed improvements beforehand.

---

## ⚠️ Disclaimer

This tool is **not affiliated with Roblox Corporation**. Use responsibly and in compliance with [Roblox's Terms of Service](https://en.help.roblox.com/hc/en-us/articles/203313410-Roblox-Terms-of-Use).
