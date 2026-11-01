# Discord Rich Presence

Shows what you are doing in TH4 on your Discord profile:

- **In the menus:** "In the main menu"
- **In a game:** `<class>, Level <n>` and `<location> | <difficulty> | <game mode>`, the time
  played in the current session, and the number of players (e.g. "2 of 4") in multiplayer.
- The large image is the art asset named `logo` of the Discord application.

## Turning it on

It is **off by default**. Add to `config.ini`:

```
set DiscordRichPresence on // show your class, level and location on your Discord profile
```

Optional, to use your own Discord application (its name and `logo` image are then shown):

```
set DiscordAppId 1234567890123456789 // empty or absent = the mod's own application
```

The application ID is public information, not a secret. The Client Secret of a Discord
application is never needed and must not be put anywhere in the game or the repository.

## How it works

`src/DiscordRpc.cpp`, no external library. The game connects to the local Discord IPC pipe
(`\\.\pipe\discord-ipc-0` ... `9`), the same protocol the official discord-rpc library uses, so
it works with the official Discord client and with modified clients running arRPC.

- All pipe work happens on one background thread; the game loop never waits for Discord.
- The game loop rebuilds the presence every 2 seconds (`DiscordRpc_Update`, ~0.2 ms) and only
  passes it on when it changed; the thread sends at most one update every 4 seconds.
- If Discord is not running, or is restarted, the thread reconnects every 15 seconds.
- Hooks: `DiscordRpc_Init` (TH1dll.cpp, after the config is read), `DiscordRpc_GameStarted` /
  `DiscordRpc_Update` / `DiscordRpc_InMenu` (Main.cpp, `RunGameLoop`).

## Linux (Wine / Proton)

Discord's pipe does not exist inside Wine, so the presence only works there with a bridge that
forwards it to the native Discord client (e.g. wine-discord-ipc-bridge). Without one the feature
simply does nothing.
