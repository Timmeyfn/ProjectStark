# Project Stark

Project Stark is a game server for Fortnite, built on top of [Project Reboot 3.0](https://github.com/Milxnor/Project-Reboot-3.0) by Milxnor.

Below are the key points about this project:

## Features

1. **S3-S15 Support**: The project includes support for Fortnite seasons 3 through 15 (main testing is done on 14.60).
2. **S1-S2 & S16-S19 Compatibility**: Seasons 1 and 2, and Season 16 through 19 might not work perfectly.
3. **Bots**: Fill your lobby with AI bots that wander spawn island, jump from the bus, skydive, loot, heal, follow the storm and fight. Bot count, skill, looting, fighting and emotes can be changed in the Bots section of the server window. (Work in progress.)
4. **Stability Fixes**: The bus is now started on the game thread (fixes random crashes when pressing Start Bus), and bots that break are removed instead of crashing the server.
5. **Lategame Enhancements**: Lategame improvements are planned for the future.

## Installation

1. Clone this repository.
2. Build the project with Visual Studio 2022 (**Release | x64**).
3. Run the server using the Reboot Launcher: Settings → Internal files → Game server type **Custom**, and point "Game server" at the built `x64\Release\Project Reboot 3.0.dll`.

## Contributing

Contributions are welcome! Feel free to create pull requests or report issues on the GitHub repository.

## License

This project is licensed under the BSD-3-Clause license, the same as the original Project Reboot 3.0 (see `LICENSE`).

---

**Note**: Project Stark is a work in progress, and further updates and enhancements are planned. Explore, experiment, and contribute to make it even better! 🚀
