# plasma.eos

Epic Online Services backend for PlasmaEngine's platform services hub. Registers as the `eos`
backend and provides sign-in, achievements, stats, leaderboards, friends, presence, lobbies,
lobby-based matchmaking, invites, entitlements, the Epic overlay, P2P transport, cloud saves on
Player Data Storage and RTC voice.

## Layout

```
Package.plPackage    the manifest - identity, compatibility, components
Eos.plPluginBundle   legacy descriptor, kept until the engine reads manifests directly
Source/
  EosPlugin/         runtime -> plEosPlugin
    EosPlatformBackend.h/.cpp   platform lifetime, result mapping, shared helpers
    Capabilities/               one file per provider interface
Configs/             build rules: engine SDK (PlasmaPackageSdk) and EOS SDK probe (PlasmaPackageEos)
Branding/            icon and banner
```

Runtime only: no editor plugin, no editor-engine-process plugin. Store backends are never
initialised in tool processes, so the editor never touches EOS.

## What you still need

The EOS SDK is not redistributed. Download it from the Epic Developer Portal and set
`EOS_SDK_PATH` to the directory that contains `SDK/Include/eos_sdk.h`,
`SDK/Lib/EOSSDK-Win64-Shipping.lib` and `SDK/Bin/EOSSDK-Win64-Shipping.dll`. At runtime
`EOSSDK-Win64-Shipping.dll` must sit beside the game executable.

## Configuration

`RuntimeConfigs/PlatformServices.ddl`, block `eos`:

| Key                | Meaning                                                                 |
| ------------------ | ----------------------------------------------------------------------- |
| `ProductId`        | Required.                                                               |
| `SandboxId`        | Required.                                                               |
| `DeploymentId`     | Required.                                                               |
| `ClientId`         | Required.                                                               |
| `EncryptionKey`    | 64 hex characters. Without it cloud saves are unavailable.              |
| `LobbyBucket`      | Bucket id for lobbies and matchmaking. Default `plasma`.                |
| `P2PSocket`        | Socket name for the transport. Default `plasma`.                        |
| `LobbyVoice`       | Create and join lobbies with an RTC room. Default `true`.               |
| `AppItemId`        | Catalogue item of the base game; without it the app counts as owned.    |
| `CatalogNamespace` | Namespace for ownership queries. Optional.                              |

Secrets never live in the config. The client secret comes from the `PLASMA_EOS_CLIENT_SECRET`
environment variable in Development builds; missing settings fail `Initialize` with a logged
reason and the hub falls back to the local backend.

`PLASMA_EOS_DEV_AUTH=host:port|credential` signs in through the Developer Authentication Tool.
Launched from the Epic launcher, the `-AUTH_PASSWORD=<code>` exchange code is used. Otherwise the
persistent token from the previous run is tried and the account portal opens when there is none.

## Identity and ids

The canonical user id is `eos:<product user id>`; `plPlatformUser::m_sSecondaryId` carries the
Epic account id. A friend who never played the product has no product user id and is listed as
`eos:epic:<account id>`; such a friend cannot receive a lobby invite.

## Behaviour notes

- **Stats** are ingest-only on EOS. `Sum` stats send the difference to the last server value,
  `Min`/`Max`/`Latest` stats send the value.
- **Leaderboards** are views over a stat. `UploadScore` ingests the bound stat; `KeepBest` versus
  `ForceUpdate` is decided by the aggregation configured in the portal, and detail values are
  dropped. The friends window is ranked locally because user-score queries carry no rank.
- **Lobby attributes** that are integer literals are stored as INT64 so numeric search filters
  compare as numbers; everything else is a string.
- **Lobby chat** does not exist on EOS. `SendChat` writes the member attribute `plasma_chat` as
  `<sequence>|<text>`; members raise `LobbyChatMessage` once per sequence number.
- **Matchmaking** is a loop over the lobby service: search for a public lobby tagged `plasma_mm`
  that satisfies the filters, join it, otherwise create one. The ticket is `Found` when the lobby
  is full; at the timeout a lobby with at least two players still counts as found.
- **Join requests** arrive through the overlay (invite accepted, presence join). The launch
  arguments `-EosLobbyId=<id>` and `-EosConnect=<string>` are read as a launch join request.
- **Overlay**: only the friends page exists. Profile, store and settings answer failure.
- **Cloud saves** ride on Player Data Storage. Files are named
  `<project>/<profile>/<slot>.plsave` with base64url uuids; each file starts with a 40-byte header
  (magic, version, tombstone flag, revision, parent revision). `Publish` reads the current file
  first and compares its revision; EOS has no compare-and-swap, so two clients publishing the same
  slot at the same instant can still race. The provider is called from an I/O task: it queues a
  request to the main thread and blocks until `Tick` completes it. On the main thread it pumps
  `EOS_Platform_Tick` itself.
- **Voice** is the RTC room of the lobby the user is in; `JoinRoom(<lobby id>)` resolves the room
  and completes when it reports connected. When a `plAudioPcmStreamProvider` is registered the
  mixed voice is delivered through a PCM stream on the master bus, otherwise EOS renders it.

## Building

Requires an engine SDK, named by `PLASMA_SDK_ROOT`, and the EOS SDK:

```bash
PLASMA_SDK_ROOT=E:/PlasmaTech/PlasmaEngine2 EOS_SDK_PATH=<sdk root> plasmabuild build EosPlugin
```

Output lands in `Bin/Windows_x64_Development/`.
